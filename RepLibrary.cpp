#include "pch.h"
#include "RepLibrary.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace
{
	std::vector<std::string> SplitCsv(const std::string& line)
	{
		std::vector<std::string> parts;
		std::string cur;
		std::stringstream ss(line);
		while (std::getline(ss, cur, ',')) parts.push_back(cur);
		if (!line.empty() && line.back() == ',') parts.push_back("");
		return parts;
	}

	std::string TrimCr(std::string s)
	{
		while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
		return s;
	}

	using Setter = std::function<void(Frame&, const std::string&)>;

	Setter F(float Frame::* m) { return [m](Frame& f, const std::string& v) { f.*m = std::strtof(v.c_str(), nullptr); }; }
	Setter I(int Frame::* m)   { return [m](Frame& f, const std::string& v) { f.*m = std::atoi(v.c_str()); }; }
	Setter B(bool Frame::* m)  { return [m](Frame& f, const std::string& v) { f.*m = (v == "1"); }; }

	const std::unordered_map<std::string, Setter>& Setters()
	{
		static const std::unordered_map<std::string, Setter> table = {
			{"tick", I(&Frame::tick)}, {"t", F(&Frame::t)}, {"wall_ms", F(&Frame::wallMs)},
			{"throttle", F(&Frame::throttle)}, {"steer", F(&Frame::steer)}, {"pitch", F(&Frame::pitch)},
			{"yaw", F(&Frame::yaw)}, {"roll", F(&Frame::roll)},
			{"jump", B(&Frame::jump)}, {"boost", B(&Frame::boost)}, {"handbrake", B(&Frame::handbrake)},
			{"car_x", F(&Frame::carX)}, {"car_y", F(&Frame::carY)}, {"car_z", F(&Frame::carZ)},
			{"car_pitch", I(&Frame::carPitch)}, {"car_yaw", I(&Frame::carYaw)}, {"car_roll", I(&Frame::carRoll)},
			{"car_vx", F(&Frame::carVX)}, {"car_vy", F(&Frame::carVY)}, {"car_vz", F(&Frame::carVZ)},
			{"car_avx", F(&Frame::carAVX)}, {"car_avy", F(&Frame::carAVY)}, {"car_avz", F(&Frame::carAVZ)},
			{"boost_amount", F(&Frame::boostAmount)},
			{"on_ground", B(&Frame::onGround)}, {"on_wall", B(&Frame::onWall)}, {"has_flip", B(&Frame::hasFlip)},
			{"ball_x", F(&Frame::ballX)}, {"ball_y", F(&Frame::ballY)}, {"ball_z", F(&Frame::ballZ)},
			{"ball_vx", F(&Frame::ballVX)}, {"ball_vy", F(&Frame::ballVY)}, {"ball_vz", F(&Frame::ballVZ)},
			{"ball_avx", F(&Frame::ballAVX)}, {"ball_avy", F(&Frame::ballAVY)}, {"ball_avz", F(&Frame::ballAVZ)},
			{"touch", B(&Frame::touch)},
			{"touch_x", F(&Frame::touchX)}, {"touch_y", F(&Frame::touchY)}, {"touch_z", F(&Frame::touchZ)},
		};
		return table;
	}
}

bool RepLibrary::LoadRep(const std::filesystem::path& file, std::vector<Frame>& out, std::string& err)
{
	out.clear();
	std::ifstream in(file);
	if (!in) { err = "Couldn't open " + file.filename().string(); return false; }

	std::string line;
	if (!std::getline(in, line)) { err = "Empty file"; return false; }
	std::vector<std::string> header = SplitCsv(TrimCr(line));

	// Map each column to its setter once; unknown columns are skipped
	std::vector<const Setter*> cols;
	for (const std::string& name : header)
	{
		auto it = Setters().find(name);
		cols.push_back(it == Setters().end() ? nullptr : &it->second);
	}

	while (std::getline(in, line))
	{
		line = TrimCr(line);
		if (line.empty()) continue;
		std::vector<std::string> parts = SplitCsv(line);
		Frame f;
		for (size_t i = 0; i < parts.size() && i < cols.size(); i++)
			if (cols[i]) (*cols[i])(f, parts[i]);
		out.push_back(f);
	}

	if (out.empty()) { err = "No frames in file"; return false; }
	return true;
}

std::vector<RepListItem> RepLibrary::ListReps(const std::filesystem::path& root)
{
	std::vector<RepListItem> items;

	// Latest rating per file (newest line wins)
	std::unordered_map<std::string, int> ratings;
	{
		std::ifstream in(root / "ratings.csv");
		std::string line;
		std::getline(in, line); // header
		while (std::getline(in, line))
		{
			auto p = SplitCsv(TrimCr(line));
			if (p.size() >= 2) ratings[p[0]] = std::atoi(p[1].c_str());
		}
	}

	std::ifstream in(root / "reps_index.csv");
	std::string line;
	if (!std::getline(in, line)) return items;
	std::vector<std::string> header = SplitCsv(TrimCr(line));
	auto col = [&](const char* name) -> int {
		for (size_t i = 0; i < header.size(); i++) if (header[i] == name) return static_cast<int>(i);
		return -1;
	};
	int cFile = col("file"), cSaved = col("saved_at"), cReason = col("end_reason"), cDur = col("duration_s"),
		cTouch = col("touches"), cGoal = col("goal"), cShot = col("shot_key");

	while (std::getline(in, line))
	{
		auto p = SplitCsv(TrimCr(line));
		auto get = [&](int c) { return (c >= 0 && c < static_cast<int>(p.size())) ? p[c] : std::string(); };
		RepListItem it;
		it.file = get(cFile);
		if (it.file.empty()) continue;
		it.savedAt = get(cSaved);
		it.endReason = get(cReason);
		it.duration = std::strtof(get(cDur).c_str(), nullptr);
		it.touches = std::atoi(get(cTouch).c_str());
		it.goal = get(cGoal) == "1";
		it.shotKey = get(cShot);
		auto r = ratings.find(it.file);
		it.rating = r == ratings.end() ? 0 : r->second;
		items.push_back(it);
	}

	std::reverse(items.begin(), items.end());
	return items;
}
