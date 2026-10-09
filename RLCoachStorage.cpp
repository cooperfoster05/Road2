#include "pch.h"
#include "RLCoach.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <format>
#include <unordered_set>

// ============================================================
// Storage upkeep: runs once in the background when the plugin
// loads (and when you click "Clean up now"):
//   1. move old loose rep files into compressed session bundles
//   2. update the shot book and auto grades (saved to disk)
//   3. delete full recordings past their keep time, then trim
//      the oldest un-starred ones if over the size cap.
// Summaries, ratings and grades are never deleted.
// ============================================================

namespace
{
	// Age in days of a "YYYY-MM-DD HH:MM:SS" time; -1 if it can't be read
	double AgeDays(const std::string& savedAt, std::time_t now)
	{
		std::tm t{};
		if (std::sscanf(savedAt.c_str(), "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
			return -1.0;
		t.tm_year -= 1900;
		t.tm_mon -= 1;
		t.tm_isdst = -1;
		std::time_t then = std::mktime(&t);
		if (then == static_cast<std::time_t>(-1)) return -1.0;
		return std::difftime(now, then) / 86400.0;
	}
}

void RLCoach::SetStorageMsg(const std::string& msg)
{
	std::lock_guard<std::mutex> lock(storageMsgMutex);
	storageMsg = msg;
}

std::string RLCoach::GetStorageMsg()
{
	std::lock_guard<std::mutex> lock(storageMsgMutex);
	return storageMsg;
}

void RLCoach::StartHousekeeping()
{
	if (!writer || housekeepBusy) return;
	if (housekeepThread.joinable()) housekeepThread.join();
	housekeepBusy = true;
	housekeepStop = false;
	auto root = writer->Folder();
	auto session = writer->SessionBundle();
	float keepD = *keepDays, keepRatedD = *keepRatedDays, capMb = *storageCapMb;
	housekeepThread = std::thread([this, root, session, keepD, keepRatedD, capMb]() {
		Housekeep(root, session, keepD, keepRatedD, capMb);
		housekeepBusy = false;
	});
}

void RLCoach::Housekeep(const std::filesystem::path& root, const std::filesystem::path& session,
	float keepD, float keepRatedD, float capMb)
{
	// 1. Old loose files -> bundles. Bailed (reset) reps keep only their summary unless starred.
	std::unordered_set<std::string> bailed;
	bool anyLoose = false;
	for (const RepStore::RecInfo& r : RepStore::List(root)) if (r.legacy) { anyLoose = true; break; }
	if (anyLoose)
	{
		SetStorageMsg("Packing old rep files into compressed bundles...");
		for (const RepListItem& it : RepLibrary::ListReps(root))
			if (it.endReason == "reset" && !it.kept) bailed.insert(it.file);
		int moved = RepStore::MigrateLegacy(root, bailed, &housekeepStop);
		LOG("RL Coach: packed {} old rep files", moved);
	}
	if (housekeepStop) return;

	// 2. Shot book and grades first, so nothing is deleted before it's been measured
	SetStorageMsg("Updating shot book and grades...");
	shotBook.Update(root);
	if (housekeepStop) return;
	gradeBook.Refresh(root, shotBook);
	if (housekeepStop) return;

	// 3. Clean up
	SetStorageMsg("Cleaning up old recordings...");
	int removed = TrimStorage(root, session, keepD, keepRatedD, capMb);
	uint64_t bytes = RepStore::TotalBytes(root);
	SetStorageMsg(std::format("Last cleanup: {} recording{} removed. Recordings use {:.1f} MB.",
		removed, removed == 1 ? "" : "s", bytes / (1024.0 * 1024.0)));
}

int RLCoach::TrimStorage(const std::filesystem::path& root, const std::filesystem::path& session,
	float keepD, float keepRatedD, float capMb)
{
	std::unordered_map<std::string, RepStore::RecInfo> recs;
	for (RepStore::RecInfo& r : RepStore::List(root)) recs[r.name] = r;
	std::unordered_set<std::string> best = shotBook.BestFiles();
	std::vector<RepListItem> items = RepLibrary::ListReps(root);   // newest first
	std::time_t now = std::time(nullptr);

	std::unordered_set<std::string> drop;
	std::vector<std::pair<const RepListItem*, uint64_t>> mayTrim;   // candidates for the size cap
	uint64_t total = 0;
	for (const RepListItem& it : items)
	{
		auto rec = recs.find(it.file);
		if (rec == recs.end()) continue;
		uint64_t bytes = rec->second.bytes;
		bool safe = it.kept || best.count(it.file) || rec->second.file == session;
		if (safe) { total += bytes; continue; }
		double age = AgeDays(it.savedAt, now);
		double limit = it.rating > 0 ? keepRatedD : keepD;
		if (age > limit) { drop.insert(it.file); continue; }
		total += bytes;
		mayTrim.push_back({ &it, bytes });
	}

	// Over the cap: unrated before rated, oldest first
	uint64_t cap = static_cast<uint64_t>(capMb) * 1024ull * 1024ull;
	if (total > cap)
	{
		std::stable_sort(mayTrim.begin(), mayTrim.end(), [](const auto& a, const auto& b) {
			bool ar = a.first->rating > 0, br = b.first->rating > 0;
			if (ar != br) return !ar;
			return a.first->savedAt < b.first->savedAt;
		});
		for (const auto& [it, bytes] : mayTrim)
		{
			if (total <= cap) break;
			drop.insert(it->file);
			total -= bytes;
		}
	}
	return RepStore::Remove(root, drop, session);
}

void RLCoach::ToggleKeep(const std::string& file)
{
	if (!writer) return;
	for (RepListItem& it : repList)
	{
		if (it.file != file) continue;
		it.kept = !it.kept;
		writer->SetKept(file, it.kept);
		return;
	}
}

void RLCoach::RenderStorageSettings()
{
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	if (writer)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - storageStatsAt > std::chrono::seconds(2))
		{
			uiStorageBytes = RepStore::TotalBytes(writer->Folder());
			storageStatsAt = now;
		}
	}
	ImGui::Text("Recordings use %.1f MB of your %.0f MB cap", uiStorageBytes / (1024.0 * 1024.0), *storageCapMb);

	int kd = static_cast<int>(*keepDays);
	if (ImGui::SliderInt("Keep recordings (days)", &kd, 1, 60)) setCvar("rlcoach_keep_days", static_cast<float>(kd));
	int krd = static_cast<int>(*keepRatedDays);
	if (ImGui::SliderInt("Keep rated reps' recordings (days)", &krd, 7, 730)) setCvar("rlcoach_keep_rated_days", static_cast<float>(krd));
	int cap = static_cast<int>(*storageCapMb);
	if (ImGui::SliderInt("Size cap (MB)", &cap, 100, 10240))
	{
		cap = std::max(100, (cap / 50) * 50);
		setCvar("rlcoach_storage_cap_mb", static_cast<float>(cap));
	}
	ImGui::TextWrapped("Summaries, your ratings and auto grades are kept forever. Starred reps (Keep in the rep viewer) "
		"and your fastest goal on each shot are never cleaned up. Bailed reps keep only their summary.");

	bool busy = housekeepBusy;
	if (busy) ImGui::TextDisabled("Clean up now");
	else if (ImGui::Button("Clean up now")) StartHousekeeping();
	std::string msg = GetStorageMsg();
	if (!msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", msg.c_str()); }
	if (writer && writer->FailedSaves() > 0)
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%d save(s) failed this session. Is the disk full?", writer->FailedSaves());
}
