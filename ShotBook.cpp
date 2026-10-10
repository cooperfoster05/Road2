#include "pch.h"
#include "ShotBook.h"
#include "RepLibrary.h"
#include "Segmenter.h"
#include "ShotGeometry.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
	std::vector<std::string> Split(const std::string& line)
	{
		std::vector<std::string> out;
		std::string cur;
		std::stringstream ss(line);
		while (std::getline(ss, cur, ',')) out.push_back(cur);
		return out;
	}
}

void ShotBook::Update(const std::filesystem::path& root)
{
	{
		std::lock_guard<std::mutex> dl(drillMtx);
		LoadDrills(root);
	}
	std::lock_guard<std::mutex> lock(mtx);
	const auto cachePath = root / "shot_cache.csv";

	if (!loadedCache)
	{
		bool oldFormat = false;
		{
			std::ifstream in(cachePath);
			std::string line;
			std::getline(in, line); // header
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				auto p = Split(line);
				if (p.size() < 6) { oldFormat = true; continue; }   // from before double taps were tracked: rescanned below
				Entry e{ p[1], p[2] == "1", std::strtof(p[3].c_str(), nullptr), std::strtof(p[4].c_str(), nullptr), p[5] == "1" };
				entries[p[0]] = e;
			}
		}
		if (oldFormat)
		{
			// Start the file over with just the up-to-date lines
			std::ofstream fresh(cachePath, std::ios::trunc);
			fresh << "file,shot_key,goal,touch_sec,jump_sec,double_tap\n";
			for (const auto& [file, e] : entries)
				fresh << file << ',' << e.shot << ',' << (e.goal ? 1 : 0) << ',' << e.touchSec << ',' << e.jumpSec << ',' << (e.doubleTap ? 1 : 0) << '\n';
		}
		loadedCache = true;
	}

	bool isNew = !std::filesystem::exists(cachePath);
	std::ofstream out(cachePath, std::ios::app);
	if (isNew) out << "file,shot_key,goal,touch_sec,jump_sec,double_tap\n";

	for (const RepListItem& item : RepLibrary::ListReps(root))
	{
		if (entries.count(item.file) || item.imported) continue;   // someone else's reps never set your benchmarks
		Entry e{ item.shotKey, item.goal, -1, -1 };

		std::vector<Frame> frames;
		std::string err;
		if (RepLibrary::LoadRep(root, item.file, frames, err))
		{
			ShotAnalysis sa = ShotGeometry::Analyze(frames);
			if (sa.valid)
			{
				e.doubleTap = sa.doubleTap;
				e.touchSec = (sa.touchTick - sa.startTick) / 120.0f;
				// The jump that led into the shot touch
				RepSegments segs = Segmenter::Split(frames, item.eventTick);
				for (const Segment& sg : segs.segs)
					if (sg.kind == SegKind::Takeoff && sg.start < sa.touchTick) e.jumpSec = (sg.start - sa.startTick) / 120.0f;
			}
		}
		entries[item.file] = e;
		if (out) out << item.file << ',' << e.shot << ',' << (e.goal ? 1 : 0) << ',' << e.touchSec << ',' << e.jumpSec << ',' << (e.doubleTap ? 1 : 0) << '\n';
	}
}

ShotReference ShotBook::Get(const std::string& shotKey)
{
	std::lock_guard<std::mutex> lock(mtx);
	return Lookup(shotKey);
}

bool ShotBook::TryGet(const std::string& shotKey, ShotReference& out)
{
	std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
	if (!lock.owns_lock() || !loadedCache) return false;
	out = Lookup(shotKey);
	return true;
}

bool ShotBook::TryAdd(const std::filesystem::path& root, const std::string& file, const std::string& shotKey,
	bool goal, float touchSec, float jumpSec, bool doubleTap)
{
	std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
	if (!lock.owns_lock() || !loadedCache) return false;   // the next full scan will pick it up
	if (entries.count(file)) return true;
	entries[file] = Entry{ shotKey, goal, touchSec, jumpSec, doubleTap };
	std::ofstream out(root / "shot_cache.csv", std::ios::app);
	if (out) out << file << ',' << shotKey << ',' << (goal ? 1 : 0) << ',' << touchSec << ',' << jumpSec << ',' << (doubleTap ? 1 : 0) << '\n';
	return true;
}

std::unordered_set<std::string> ShotBook::BestFiles()
{
	std::lock_guard<std::mutex> lock(mtx);
	std::unordered_map<std::string, std::pair<float, std::string>> best;   // shot -> (touch time, file)
	for (const auto& [file, e] : entries)
	{
		if (!e.goal || e.touchSec < 0) continue;
		auto it = best.find(e.shot);
		if (it == best.end() || e.touchSec < it->second.first) best[e.shot] = { e.touchSec, file };
	}
	std::unordered_set<std::string> out;
	for (const auto& [shot, b] : best) out.insert(b.second);
	return out;
}

bool ShotBook::SameShot(const std::string& a, const std::string& b)
{
	if (a == b) return true;
	auto parse = [](const std::string& k, int out[5]) {
		std::stringstream ss(k);
		std::string part;
		int n = 0;
		while (n < 5 && std::getline(ss, part, '_')) out[n++] = std::atoi(part.c_str());
		return n == 5;
	};
	int pa[5], pb[5];
	if (!parse(a, pa) || !parse(b, pb)) return false;
	for (int i = 0; i < 5; i++) if (std::abs(pa[i] - pb[i]) > 100) return false;
	return true;
}

void ShotBook::LoadDrills(const std::filesystem::path& root)
{
	if (loadedDrills) return;
	std::ifstream in(root / "drills.csv");
	std::string line;
	std::getline(in, line); // header
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		auto p = Split(line);
		if (p.size() >= 2) drills[p[0]] = p[1] == "1";
	}
	loadedDrills = true;
}

bool ShotBook::DrillLocked(const std::string& shotKey)
{
	for (const auto& [key, on] : drills)
		if (on && SameShot(key, shotKey)) return true;
	return false;
}

bool ShotBook::IsDoubleTapDrill(const std::string& shotKey)
{
	std::lock_guard<std::mutex> dl(drillMtx);
	return DrillLocked(shotKey);
}

void ShotBook::SetDoubleTapDrill(const std::filesystem::path& root, const std::string& shotKey, bool on)
{
	std::lock_guard<std::mutex> dl(drillMtx);
	LoadDrills(root);
	// Turning it off clears every neighbouring key too, so the shot really stops being a drill
	if (!on)
		for (auto& [key, v] : drills) if (SameShot(key, shotKey)) v = false;
	drills[shotKey] = on;
	bool isNew = !std::filesystem::exists(root / "drills.csv");
	std::ofstream out(root / "drills.csv", std::ios::app);
	if (isNew) out << "shot_key,double_tap_drill\n";
	if (!on)
	{
		for (const auto& [key, v] : drills) if (SameShot(key, shotKey)) out << key << ",0\n";
	}
	else out << shotKey << ",1\n";
}

ShotReference ShotBook::Lookup(const std::string& shotKey)
{
	ShotReference r;
	{
		std::lock_guard<std::mutex> dl(drillMtx);
		r.doubleTapDrill = DrillLocked(shotKey);
	}
	for (const auto& [file, e] : entries)
	{
		if (e.shot != shotKey) continue;
		r.attempts++;
		if (e.doubleTap) r.doubleTaps++;
		if (!e.goal || e.touchSec < 0) continue;
		r.goals++;
		if (!r.valid || e.touchSec < r.touchSec)
		{
			r.valid = true;
			r.bestFile = file;
			r.touchSec = e.touchSec;
			r.jumpSec = e.jumpSec;
		}
	}
	return r;
}
