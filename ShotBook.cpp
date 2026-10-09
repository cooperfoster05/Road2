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
	std::lock_guard<std::mutex> lock(mtx);
	const auto cachePath = root / "shot_cache.csv";

	if (!loadedCache)
	{
		std::ifstream in(cachePath);
		std::string line;
		std::getline(in, line); // header
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			auto p = Split(line);
			if (p.size() < 5) continue;
			Entry e{ p[1], p[2] == "1", std::strtof(p[3].c_str(), nullptr), std::strtof(p[4].c_str(), nullptr) };
			entries[p[0]] = e;
		}
		loadedCache = true;
	}

	bool isNew = !std::filesystem::exists(cachePath);
	std::ofstream out(cachePath, std::ios::app);
	if (isNew) out << "file,shot_key,goal,touch_sec,jump_sec\n";

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
				e.touchSec = (sa.touchTick - sa.startTick) / 120.0f;
				// The jump that led into the shot touch
				RepSegments segs = Segmenter::Split(frames, item.eventTick);
				for (const Segment& sg : segs.segs)
					if (sg.kind == SegKind::Takeoff && sg.start < sa.touchTick) e.jumpSec = (sg.start - sa.startTick) / 120.0f;
			}
		}
		entries[item.file] = e;
		if (out) out << item.file << ',' << e.shot << ',' << (e.goal ? 1 : 0) << ',' << e.touchSec << ',' << e.jumpSec << '\n';
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
	bool goal, float touchSec, float jumpSec)
{
	std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
	if (!lock.owns_lock() || !loadedCache) return false;   // the next full scan will pick it up
	if (entries.count(file)) return true;
	entries[file] = Entry{ shotKey, goal, touchSec, jumpSec };
	std::ofstream out(root / "shot_cache.csv", std::ios::app);
	if (out) out << file << ',' << shotKey << ',' << (goal ? 1 : 0) << ',' << touchSec << ',' << jumpSec << '\n';
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

ShotReference ShotBook::Lookup(const std::string& shotKey)
{
	ShotReference r;
	for (const auto& [file, e] : entries)
	{
		if (e.shot != shotKey) continue;
		r.attempts++;
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
