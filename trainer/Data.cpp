#include "Data.h"
#include "../Grader.h"
#include "../Mechanics.h"
#include "../RepLibrary.h"
#include "../Segmenter.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

const char* Data::TypeFile(ShotType t)
{
	switch (t)
	{
	case ShotType::Aerial: return "aerial";
	case ShotType::Ground: return "ground";
	case ShotType::DoubleTap: return "doubletap";
	case ShotType::Dribble: return "dribble";
	case ShotType::AirDribble: return "airdribble";
	case ShotType::FlipReset: return "flipreset";
	}
	return "other";
}

fs::path Data::HeldoutFile(const fs::path& dir, ShotType t) { return dir / (std::string("heldout_") + TypeFile(t) + ".txt"); }

std::unordered_set<std::string> Data::ReadList(const fs::path& file)
{
	std::unordered_set<std::string> out;
	std::ifstream in(file);
	std::string line;
	while (std::getline(in, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if (!line.empty() && line[0] != '#') out.insert(line);
	}
	return out;
}

std::vector<Frame> Data::Mirror(const std::vector<Frame>& fr)
{
	std::vector<Frame> m = fr;
	for (Frame& f : m)
	{
		// Positions and velocities: x flips. Spins are pseudovectors: under x -> -x they become (wx, -wy, -wz).
		f.carX = -f.carX; f.carVX = -f.carVX; f.carAVY = -f.carAVY; f.carAVZ = -f.carAVZ;
		f.ballX = -f.ballX; f.ballVX = -f.ballVX; f.ballAVY = -f.ballAVY; f.ballAVZ = -f.ballAVZ;
		f.touchX = -f.touchX;
		// Facing: yaw -> 180 - yaw, roll -> -roll (pitch stays). Inputs: steer, yaw and roll flip.
		int yaw = 32768 - f.carYaw;
		while (yaw > 32768) yaw -= 65536;
		while (yaw <= -32768) yaw += 65536;
		f.carYaw = yaw;
		f.carRoll = -f.carRoll;
		f.steer = -f.steer; f.yaw = -f.yaw; f.roll = -f.roll;
	}
	return m;
}

void Data::Finish(Rep& r, ShotBook& book, const std::string& shotKey)
{
	r.real = ShotGeometry::Analyze(r.fr);
	const int n = static_cast<int>(r.fr.size());
	if (r.eventTick < 0 || r.eventTick >= n) r.eventTick = n - 1;
	RepSegments segs = Segmenter::Split(r.fr, r.eventTick);
	if (!shotKey.empty()) r.ref = book.Get(shotKey);   // (mirrored copies keep the original's)
	RepGrade g = Grader::GradeRep(r.fr, segs, r.real, r.wentIn, r.floorBeforeGoal, r.ref);
	r.realGrade = r.real.valid ? g.overall : 0;
	r.sContact = r.real.valid ? std::max(r.real.startTick, r.real.touchTick - 60) : 1;
	r.sMech = Mechanics::MechanicStart(r.real, n);
}

std::vector<Rep> Data::Load(const fs::path& root, ShotBook& book, ShotType type,
	const std::unordered_set<std::string>& heldout, int which, int threads)
{
	struct Row { std::string file, shotKey; bool goal = false, floor = false; int eventTick = -1; };
	std::vector<Row> rows;
	{
		std::ifstream in(root / "reps_index.csv");
		std::string line;
		std::getline(in, line);
		std::vector<std::string> head;
		{ std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) { while (!c.empty() && c.back() == '\r') c.pop_back(); head.push_back(c); } }
		auto col = [&](const char* name) { for (size_t i = 0; i < head.size(); i++) if (head[i] == name) return static_cast<int>(i); return -1; };
		const int cFile = col("file"), cGoal = col("goal"), cEvent = col("event_tick"), cFloor = col("floor_before_goal"), cKey = col("shot_key");
		while (std::getline(in, line))
		{
			while (!line.empty() && line.back() == '\r') line.pop_back();
			std::vector<std::string> p;
			{ std::stringstream ss(line); std::string c; while (std::getline(ss, c, ',')) p.push_back(c); if (!line.empty() && line.back() == ',') p.push_back(""); }
			auto at = [&](int c) { return c >= 0 && c < static_cast<int>(p.size()) ? p[c] : std::string(); };
			Row r;
			r.file = at(cFile);
			if (r.file.empty()) continue;
			const bool held = heldout.count(r.file) > 0;
			if ((which == 1 && !held) || (which == 2 && held)) continue;
			r.goal = at(cGoal) == "1";
			r.floor = at(cFloor) == "1";
			r.eventTick = at(cEvent).empty() ? -1 : std::atoi(at(cEvent).c_str());
			r.shotKey = at(cKey);
			rows.push_back(r);
		}
	}
	book.Update(root);
	std::vector<Rep> out;
	std::mutex mx;
	std::atomic<size_t> next{ 0 };
	auto work = [&] {
		for (size_t i; (i = next.fetch_add(1)) < rows.size();)
		{
			const Row& row = rows[i];
			Rep r;
			std::string err;
			if (!RepLibrary::LoadRep(root, row.file, r.fr, err) || r.fr.size() < 8) continue;
			r.file = row.file;
			r.wentIn = row.goal;
			r.floorBeforeGoal = row.floor;
			r.eventTick = row.eventTick;
			Finish(r, book, row.shotKey);
			if (!r.real.valid || r.real.type != type) continue;
			std::lock_guard<std::mutex> l(mx);
			out.push_back(std::move(r));
		}
	};
	std::vector<std::thread> ts;
	for (int t = 0; t < std::max(1, threads); t++) ts.emplace_back(work);
	for (auto& t : ts) t.join();
	std::sort(out.begin(), out.end(), [](const Rep& a, const Rep& b) { return a.file < b.file; });
	return out;
}
