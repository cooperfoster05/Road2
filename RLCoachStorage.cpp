#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cwctype>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

// ============================================================
// Storage upkeep: runs once in the background when the plugin
// loads (and when you click "Clean up now"):
//   1. move old loose rep files into compressed session bundles
//   2. update the shot book and auto grades (saved to disk)
//   3. delete full recordings past their keep time, then trim
//      the oldest un-starred ones if over the size cap.
// Summaries, ratings and grades are never deleted. Neither are
// recordings listed in protected_reps.txt (the engine and bot
// benchmark reps, one file name per line; Oct 24).
// Nov 4 (the retention question, Coop agreed): an unrated rep that
// auto-graded 4 or 5, or that you opened in the review (where the
// smallest fix and the engine line are found; reviewed.csv), keeps
// its recording at least 30 days instead of the unrated keep time.
// ============================================================

namespace
{
	std::string NowStamp(const char* fmt)
	{
		std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char buf[64];
		std::strftime(buf, sizeof(buf), fmt, &local);
		return buf;
	}

	// Letters, digits, - and _ only, so it's safe in file names and CSV
	std::string CleanTag(const std::string& in)
	{
		std::string out;
		for (char ch : in)
			if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') out += ch;
		if (out.size() > 24) out.resize(24);
		return out.empty() ? std::string("shared") : out;
	}

	std::vector<std::string> SplitLine(const std::string& line)
	{
		std::vector<std::string> parts;
		std::string cur;
		for (char ch : line)
		{
			if (ch == ',') { parts.push_back(cur); cur.clear(); }
			else if (ch != '\r' && ch != '\n') cur += ch;
		}
		parts.push_back(cur);
		return parts;
	}

	void OpenFolder(const std::filesystem::path& folder)
	{
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
#ifdef _WIN32
		ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
	}

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

// Rocket League's own files, found from where the game is running (the same on Steam and Epic):
// ...\rocketleague\Binaries\Win64\RocketLeague.exe -> ...\rocketleague\TAGame\CookedPCConsole
std::filesystem::path RLCoach::GameCookedFolder()
{
#ifdef _WIN32
	wchar_t exe[1024] = {};
	const DWORD n = GetModuleFileNameW(nullptr, exe, 1023);
	if (n > 0 && n < 1023) return std::filesystem::path(exe).parent_path().parent_path().parent_path() / "TAGame" / "CookedPCConsole";
#endif
	return {};
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
		LOG("Road2: packed {} old rep files", moved);
	}
	if (housekeepStop) return;

	// 2. Shot book and grades first, so nothing is deleted before it's been measured
	SetStorageMsg("Updating shot book and grades...");
	shotBook.Update(root);
	if (housekeepStop) return;
	gradeBook.Refresh(root, shotBook, &housekeepStop);
	if (housekeepStop) return;
	habitBook.Update(root);   // measures habits before any recording is cleaned up (the numbers are kept)
	if (housekeepStop) return;
	coachBook.Update(root);
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
	std::unordered_set<std::string> protectedReps;
	{
		std::ifstream in(root / "protected_reps.txt");
		std::string line;
		while (std::getline(in, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			if (!line.empty()) protectedReps.insert(line);
		}
	}
	std::vector<RepListItem> items = RepLibrary::ListReps(root);   // newest first
	std::time_t now = std::time(nullptr);
	constexpr double kKeepWorthDays = 30.0;
	std::unordered_map<std::string, int> autoGrades;
	gradeBook.Copy(autoGrades);
	std::unordered_set<std::string> reviewed;
	{
		std::ifstream in(root / "reviewed.csv");
		std::string line;
		while (std::getline(in, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			if (!line.empty()) reviewed.insert(line);
		}
	}

	std::unordered_set<std::string> drop;
	std::vector<std::pair<const RepListItem*, uint64_t>> mayTrim;   // candidates for the size cap
	uint64_t total = 0;
	for (const RepListItem& it : items)
	{
		auto rec = recs.find(it.file);
		if (rec == recs.end()) continue;
		uint64_t bytes = rec->second.bytes;
		bool safe = it.kept || best.count(it.file) || protectedReps.count(it.file) || rec->second.file == session;
		if (safe) { total += bytes; continue; }
		double age = AgeDays(it.savedAt, now);
		double limit = (it.rating > 0 || it.recoveryRating > 0) ? keepRatedD : keepD;
		if (auto g = autoGrades.find(it.file); (g != autoGrades.end() && g->second >= 4) || reviewed.count(it.file)) limit = std::max(limit, kKeepWorthDays);
		if (age > limit) { drop.insert(it.file); continue; }
		total += bytes;
		mayTrim.push_back({ &it, bytes });
	}

	// Over the cap: unrated before rated, oldest first
	uint64_t cap = static_cast<uint64_t>(capMb) * 1024ull * 1024ull;
	if (total > cap)
	{
		std::stable_sort(mayTrim.begin(), mayTrim.end(), [](const auto& a, const auto& b) {
			bool ar = a.first->rating > 0 || a.first->recoveryRating > 0, br = b.first->rating > 0 || b.first->recoveryRating > 0;
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

// ============================================================
// Sharing: export your starred reps to one file; import files
// friends send you. Imported reps are starred (never cleaned up),
// tagged with who they came from, and never become your benchmarks.
// ============================================================

void RLCoach::StartExport()
{
	if (!writer || shareBusy) return;
	if (shareThread.joinable()) shareThread.join();
	shareBusy = true;
	auto root = writer->Folder();
	shareThread = std::thread([this, root]() {
		std::vector<std::pair<std::string, std::string>> chunks;
		std::string index = "file,saved_at,mode,map,end_reason,duration_s,ticks,touches,goal,event_tick,floor_before_goal,shot_key\n";
		int n = 0;
		for (const RepListItem& it : RepLibrary::ListReps(root))
		{
			if (!it.kept || !it.hasRecording || it.imported) continue;
			std::string csv, err;
			if (!RepStore::Read(root, it.file, csv, err)) continue;
			chunks.emplace_back(it.file, std::move(csv));
			index += std::format("{},{},training,,{},{:.3f},0,{},{},{},{},{}\n", it.file, it.savedAt, it.endReason, it.duration,
				it.touches, it.goal ? 1 : 0, it.eventTick, it.floorBeforeGoal ? 1 : 0, it.shotKey);
			n++;
		}
		if (n == 0)
		{
			SetStorageMsg("Nothing to export yet: star the reps you want to share (the star column in the rep viewer).");
			shareBusy = false;
			return;
		}
		chunks.emplace_back("__index.csv", index);
		auto file = root / "exports" / ("Road2_reps_" + NowStamp("%Y-%m-%d_%H-%M-%S") + ".rlc");
		std::string err;
		if (RepStore::WriteBundle(file, chunks, err))
			SetStorageMsg(std::format("Exported {} starred rep{} to exports\\{}", n, n == 1 ? "" : "s", file.filename().string()));
		else
			SetStorageMsg("Export failed: " + err);
		shareBusy = false;
	});
}

void RLCoach::StartImport()
{
	if (!writer || shareBusy) return;
	if (shareThread.joinable()) shareThread.join();
	shareBusy = true;
	auto root = writer->Folder();
	shareThread = std::thread([this, root]() {
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path inbox = root / "imports";
		fs::create_directories(inbox / "done", ec);
		int files = 0, added = 0, skipped = 0;
		std::string problems;

		std::vector<fs::path> todo;
		for (const auto& e : fs::directory_iterator(inbox, ec))
			if (e.is_regular_file() && e.path().extension() == ".rlc") todo.push_back(e.path());

		for (const fs::path& file : todo)
		{
			std::vector<std::pair<std::string, std::string>> chunks;
			std::string err;
			if (!RepStore::ReadBundle(file, chunks, err)) { problems += " " + file.filename().string() + ": " + err + "."; continue; }
			const std::string* index = nullptr;
			std::unordered_map<std::string, const std::string*> byName;
			for (const auto& [name, csv] : chunks)
			{
				if (name == "__index.csv") index = &csv;
				else byName[name] = &csv;
			}
			if (!index) { problems += " " + file.filename().string() + ": not a Road2 export."; continue; }

			std::string tag = CleanTag(file.stem().string());
			fs::path bundle = root / "sessions" / ("imported_" + tag + ".rlc");
			std::istringstream lines(*index);
			std::string line;
			std::getline(lines, line);
			std::vector<std::string> header = SplitLine(line);
			auto col = [&](const char* name) { for (size_t i = 0; i < header.size(); i++) if (header[i] == name) return static_cast<int>(i); return -1; };
			int cFile = col("file"), cSaved = col("saved_at"), cMap = col("map"), cReason = col("end_reason"), cDur = col("duration_s"),
				cTicks = col("ticks"), cTouch = col("touches"), cGoal = col("goal"), cEvent = col("event_tick"), cBounce = col("floor_before_goal"), cShot = col("shot_key");
			while (std::getline(lines, line))
			{
				std::vector<std::string> p = SplitLine(line);
				auto get = [&](int c) { return (c >= 0 && c < static_cast<int>(p.size())) ? p[c] : std::string(); };
				std::string orig = get(cFile);
				auto rec = byName.find(orig);
				if (orig.empty() || rec == byName.end()) continue;
				std::string newName = "imp_" + tag + "_" + orig;
				if (RepStore::Has(root, newName)) { skipped++; continue; }
				if (!RepStore::Append(root, bundle, newName, *rec->second, err)) continue;

				RepSummary s;
				s.fileName = newName;
				s.savedAt = get(cSaved);
				s.mode = "import:" + tag;
				s.map = get(cMap);
				s.endReason = get(cReason);
				s.durationS = std::strtof(get(cDur).c_str(), nullptr);
				s.ticks = std::atoi(get(cTicks).c_str());
				s.touches = std::atoi(get(cTouch).c_str());
				s.goal = get(cGoal) == "1";
				s.eventTick = std::atoi(get(cEvent).c_str());
				s.floorBeforeGoal = get(cBounce) == "1";
				s.shotKey = get(cShot);
				writer->AddIndexRow(s);
				writer->SetKept(newName, true);
				added++;
			}
			files++;
			fs::rename(file, inbox / "done" / file.filename(), ec);
		}

		// Let the queued index lines land before the rep list reloads
		for (int i = 0; i < 200 && writer->Busy(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (todo.empty())
			SetStorageMsg("No files to import: put .rlc files from friends in the imports folder first.");
		else
			SetStorageMsg(std::format("Imported {} rep{} from {} file{}{}{}", added, added == 1 ? "" : "s", files, files == 1 ? "" : "s",
				skipped ? std::format(" ({} already here)", skipped) : std::string(), problems));
		repListStale = true;
		shareBusy = false;
	});
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

void RLCoach::OpenDataSubfolder(const char* name)
{
	if (writer) OpenFolder(writer->Folder() / name);
}

void RLCoach::OpenDataFolder()
{
	if (writer) OpenFolder(writer->Folder());
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
	UiStyle::Heading("Recordings");
	ImGui::Text("Recordings use %.1f MB of your %.0f MB cap", uiStorageBytes / (1024.0 * 1024.0), *storageCapMb);

	int kd = static_cast<int>(*keepDays);
	if (UiStyle::SliderInt("Keep recordings (days)", &kd, 1, 60)) setCvar("road2_keep_days", static_cast<float>(kd));
	int krd = static_cast<int>(*keepRatedDays);
	if (UiStyle::SliderInt("Keep rated reps' recordings (days)", &krd, 7, 730)) setCvar("road2_keep_rated_days", static_cast<float>(krd));
	int cap = static_cast<int>(*storageCapMb);
	if (UiStyle::SliderInt("Size cap (MB)", &cap, 100, 10240))
	{
		cap = std::max(100, (cap / 50) * 50);
		setCvar("road2_storage_cap_mb", static_cast<float>(cap));
	}
	ImGui::TextWrapped("Summaries, your ratings and auto grades are kept forever. Starred reps (the star in the rep viewer) "
		"and your fastest goal on each shot are never cleaned up. Reps that auto-graded 4 or 5, and any rep you've opened in the review, "
		"keep their recording at least 30 days. Bailed reps keep only their summary.");

	ImGui::Spacing();
	UiStyle::Heading("Sharing reps");
	ImGui::TextDisabled("Export packs your starred reps into one file. To watch a friend's reps, put their file in the imports folder");
	ImGui::TextDisabled("(rename it to their name first, e.g. jake.rlc) and click Import. Their reps never count as your benchmarks.");
	bool sharing = shareBusy;
	if (sharing) ImGui::TextDisabled("Working...");
	else
	{
		if (UiStyle::QuietButton("Export starred reps")) StartExport();
		ImGui::SameLine();
		if (UiStyle::QuietButton("Import shared reps")) StartImport();
		ImGui::SameLine();
		if (UiStyle::QuietButton("Open exports folder") && writer) OpenFolder(writer->Folder() / "exports");
		ImGui::SameLine();
		if (UiStyle::QuietButton("Open imports folder") && writer) OpenFolder(writer->Folder() / "imports");
	}
	ImGui::Spacing();

	UiStyle::Heading("Clean up");
	bool busy = housekeepBusy;
	if (busy) ImGui::TextDisabled("Clean up now");
	else if (UiStyle::QuietButton("Clean up now")) StartHousekeeping();
	std::string msg = GetStorageMsg();
	if (!msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", msg.c_str()); }
	if (writer && writer->FailedSaves() > 0)
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%d save(s) failed this session. Is the disk full?", writer->FailedSaves());
}

namespace { void Road2ModuleAnchor() {} }   // an address inside this DLL (RetireOldPlugin)

// ============================================================
// From RLCoach to Road2 (Nov 6). Only ever does anything for someone who ran a build from before v1 (Coop).
// ============================================================
std::filesystem::path RLCoach::DataRoot(const std::filesystem::path& base, bool& moved)
{
	namespace fs = std::filesystem;
	moved = false;
	std::error_code ec;
	const fs::path oldDir = base / "RLCoach", newDir = base / "Road2";
	if (!fs::exists(newDir, ec) && fs::exists(oldDir, ec))
	{
		fs::rename(oldDir, newDir, ec);
		if (ec)
		{
			LOG("Road2: couldn't rename {} to Road2 ({}); using it as it is", oldDir.string(), ec.message());
			return oldDir;
		}
		moved = true;
		LOG("Road2: moved your data from {} to {}", oldDir.string(), newDir.string());
	}
	return newDir;
}

void RLCoach::CarryOverOldSettings(const std::filesystem::path& base)
{
	// BakkesMod keeps cvars in cfg\config.cfg (name "value") and binds in cfg\binds.cfg (bind key "command")
	const std::filesystem::path cfg = base.parent_path() / "cfg";
	int values = 0, binds = 0;
	{
		std::ifstream in(cfg / "config.cfg");
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.rfind("rlcoach_", 0) != 0) continue;
			const size_t sp = line.find(' ');
			if (sp == std::string::npos) continue;
			std::string value = line.substr(sp + 1);
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
			CVarWrapper cv = cvarManager->getCvar("road2_" + line.substr(8, sp - 8));
			if (cv.IsNull()) continue;
			cv.setValue(value);
			values++;
		}
	}
	{
		std::ifstream in(cfg / "binds.cfg");
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.rfind("bind ", 0) != 0 || line.find("rlcoach_") == std::string::npos) continue;
			size_t at;
			while ((at = line.find("rlcoach_")) != std::string::npos) line.replace(at, 8, "road2_");
			cvarManager->executeCommand(line, false);
			binds++;
		}
	}
	LOG("Road2: carried over {} settings and {} key binds from the old RLCoach names", values, binds);
}

void RLCoach::RetireOldPlugin(const std::filesystem::path& base)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path plugins = base.parent_path() / "plugins";
	const fs::path old = plugins / "RLCoach.dll";
	if (!fs::exists(old, ec)) return;
#ifdef _WIN32
	// Never this plugin itself (if it was built under the old name)
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH] = {};
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Road2ModuleAnchor), &self) && GetModuleFileNameW(self, path, MAX_PATH))
	{
		std::wstring me = fs::path(path).filename().wstring();
		for (auto& ch : me) ch = static_cast<wchar_t>(towlower(ch));
		if (me == L"rlcoach.dll") return;
	}
#endif
	LOG("Road2: an old RLCoach.dll is still installed; unloading and removing it");
	gameWrapper->SetTimeout([this, old, plugins](GameWrapper*) {
		cvarManager->executeCommand("plugin unload rlcoach", false);
		gameWrapper->SetTimeout([this, old, plugins](GameWrapper*) {
			std::error_code e2;
			fs::remove(old, e2);
			if (e2) LOG("Road2: couldn't delete {} ({}); delete it by hand", old.string(), e2.message());
			// load Road2 at startup instead of RLCoach
			const fs::path cfgFile = plugins.parent_path() / "cfg" / "plugins.cfg";
			std::ifstream in(cfgFile);
			std::ostringstream out;
			std::string line;
			bool haveRoad2 = false, changed = false;
			std::vector<std::string> lines;
			while (std::getline(in, line)) lines.push_back(line);
			in.close();
			for (const std::string& l : lines) if (l.find("plugin load road2") != std::string::npos) haveRoad2 = true;
			for (std::string l : lines)
			{
				std::string t = l;
				if (!t.empty() && t.back() == '\r') t.pop_back();
				if (t == "plugin load rlcoach") { changed = true; if (haveRoad2) continue; l = "plugin load road2"; haveRoad2 = true; }
				out << l << "\n";
			}
			if (changed) { std::ofstream f(cfgFile, std::ios::binary); f << out.str(); }
		}, 1.0f);
	}, 1.0f);
}

// ============================================================
// The grading check (Nov 10): the same reps, graded again, must come out the same unless the change was meant.
// The reps are the benchmark ones in protected_reps.txt (never cleaned up); without that file, your 200 newest
// recordings. Graded with no shot reference, so your newer goals can't move the result.
// ============================================================
void RLCoach::GradeCheck(bool save)
{
	if (!writer) return;
	const std::filesystem::path root = writer->Folder();
	std::vector<std::string> files;
	{
		std::ifstream in(root / "protected_reps.txt");
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!line.empty()) files.push_back(line);
		}
	}
	const std::vector<RepListItem> all = RepLibrary::ListReps(root);
	std::unordered_map<std::string, const RepListItem*> byFile;
	for (const RepListItem& it : all) byFile[it.file] = &it;
	if (files.empty())
		for (auto it = all.rbegin(); it != all.rend() && files.size() < 200; ++it) if (it->hasRecording) files.push_back(it->file);
	auto gradeOf = [&](const std::string& file, std::string& out) {
		auto f = byFile.find(file);
		if (f == byFile.end()) return false;
		std::vector<Frame> fr;
		std::string err;
		if (!RepLibrary::LoadRep(root, file, fr, err) || fr.size() < 4) return false;
		const ShotAnalysis shot = ShotGeometry::Analyze(fr);
		const RepSegments segs = Segmenter::Split(fr, f->second->eventTick >= 0 ? f->second->eventTick : static_cast<int>(fr.size()) - 1);
		const RepGrade g = Grader::GradeRep(fr, segs, shot, f->second->goal, f->second->floorBeforeGoal, ShotReference{});
		out = std::to_string(g.overall) + "," + ShotGeometry::TypeName(shot.type);
		for (const SegmentGrade& sg : g.segs) out += std::string("|") + Grader::Name(sg.grade);
		return true;
	};
	if (save)
	{
		std::ofstream out(root / "grade_check.csv", std::ios::trunc);
		int n = 0;
		for (const std::string& file : files)
		{
			std::string g;
			if (gradeOf(file, g)) { out << file << "," << g << "\n"; n++; }
		}
		LOG("Road2 grade check: saved the grades of {} reps (grader v{}) to grade_check.csv", n, Grader::kVersion);
		return;
	}
	std::ifstream in(root / "grade_check.csv");
	if (!in) { LOG("Road2 grade check: nothing saved yet. Run: road2_grade_check save"); return; }
	std::string line;
	int same = 0, changed = 0, missing = 0;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const size_t c = line.find(',');
		if (c == std::string::npos) continue;
		const std::string file = line.substr(0, c), was = line.substr(c + 1);
		std::string now;
		if (!gradeOf(file, now)) { missing++; continue; }
		if (now == was) { same++; continue; }
		if (++changed <= 25) LOG("Road2 grade check: {} was {} now {}", file, was, now);
	}
	LOG("Road2 grade check (grader v{}): {} the same, {} changed, {} not found{}", Grader::kVersion, same, changed, missing,
		changed > 25 ? " (the first 25 are listed above)" : changed == 0 ? ". Nothing moved." : "");
}
