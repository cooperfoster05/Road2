#include "pch.h"
#include "RepLibrary.h"
#include "RepStore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
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

	// One CSV column: which Frame member it fills (Nov 11: parsed in place with from_chars, about 8x faster than the
	// old string-per-cell way; a rep is read every time it's watched, graded or used by a lesson)
	struct Col
	{
		enum Kind { Skip, Float, Int, Bool } kind = Skip;
		float Frame::* f = nullptr;
		int Frame::* i = nullptr;
		bool Frame::* b = nullptr;
	};
	Col F(float Frame::* m) { Col c; c.kind = Col::Float; c.f = m; return c; }
	Col I(int Frame::* m) { Col c; c.kind = Col::Int; c.i = m; return c; }
	Col B(bool Frame::* m) { Col c; c.kind = Col::Bool; c.b = m; return c; }

	const std::unordered_map<std::string, Col>& Columns()
	{
		static const std::unordered_map<std::string, Col> table = {
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

	// A number from [a, b). Anything from_chars can't read whole (a leading +, spaces, "nan") goes the old way.
	// A value that isn't a real number becomes 0: a damaged or doctored shared file must not put NaN into the game.
	float ToFloat(const char* a, const char* b)
	{
		float v = 0.0f;
		auto r = std::from_chars(a, b, v);
		if (r.ec != std::errc() || r.ptr != b) v = std::strtof(std::string(a, b).c_str(), nullptr);
		return std::isfinite(v) ? v : 0.0f;
	}
	int ToInt(const char* a, const char* b)
	{
		int v = 0;
		auto r = std::from_chars(a, b, v);
		if (r.ec != std::errc() || r.ptr != b) v = std::atoi(std::string(a, b).c_str());
		return v;
	}
}

bool RepLibrary::LoadRep(const std::filesystem::path& root, const std::string& name, std::vector<Frame>& out, std::string& err)
{
	out.clear();
	std::string csv;
	if (!RepStore::Read(root, name, csv, err)) return false;
	return ParseRep(csv, out, err);
}

bool RepLibrary::ParseRep(const std::string& csv, std::vector<Frame>& out, std::string& err)
{
	out.clear();
	const char* p = csv.data();
	const char* const end = p + csv.size();
	// one line: [p, le), without its line ending
	auto lineEnd = [end](const char* from) { const char* q = from; while (q < end && *q != '\n') q++; return q; };
	auto trimmed = [](const char* a, const char* b) { while (b > a && (b[-1] == '\r' || b[-1] == '\n')) b--; return b; };

	if (p >= end) { err = "Empty file"; return false; }
	// Map each column to its Frame member once; unknown columns are skipped
	std::vector<Col> cols;
	{
		const char* le = lineEnd(p);
		const char* b = trimmed(p, le);
		const char* a = p;
		for (const char* q = p; ; q++)
		{
			if (q == b || *q == ',')
			{
				auto it = Columns().find(std::string(a, q));
				cols.push_back(it == Columns().end() ? Col{} : it->second);
				a = q + 1;
				if (q == b) break;
			}
		}
		p = le < end ? le + 1 : end;
	}
	out.reserve(static_cast<size_t>(end - p) / 180 + 8);

	while (p < end)
	{
		const char* le = lineEnd(p);
		const char* b = trimmed(p, le);
		if (b > p)
		{
			Frame f;
			const char* a = p;
			size_t ci = 0;
			for (const char* q = p; ci < cols.size(); q++)
			{
				if (q == b || *q == ',')
				{
					const Col& c = cols[ci++];
					switch (c.kind)
					{
					case Col::Float: f.*c.f = ToFloat(a, q); break;
					case Col::Int: f.*c.i = ToInt(a, q); break;
					case Col::Bool: f.*c.b = (q - a == 1 && *a == '1'); break;
					default: break;
					}
					a = q + 1;
					if (q == b) break;
				}
			}
			out.push_back(f);
		}
		p = le < end ? le + 1 : end;
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

	// Recovery ratings (newest line wins)
	std::unordered_map<std::string, int> recoveries;
	{
		std::ifstream rin(root / "recovery_ratings.csv");
		std::string line;
		std::getline(rin, line); // header
		while (std::getline(rin, line))
		{
			auto p = SplitCsv(TrimCr(line));
			if (p.size() >= 2) recoveries[p[0]] = std::atoi(p[1].c_str());
		}
	}

	// Keep marks (newest line wins)
	std::unordered_map<std::string, bool> kept;
	{
		std::ifstream kin(root / "kept.csv");
		std::string line;
		std::getline(kin, line); // header
		while (std::getline(kin, line))
		{
			auto p = SplitCsv(TrimCr(line));
			if (p.size() >= 2) kept[p[0]] = p[1] == "1";
		}
	}

	// Which recordings still exist
	std::unordered_map<std::string, bool> recorded;
	for (const RepStore::RecInfo& r : RepStore::List(root)) recorded[r.name] = true;

	std::ifstream in(root / "reps_index.csv");
	std::string line;
	if (!std::getline(in, line)) return items;
	std::vector<std::string> header = SplitCsv(TrimCr(line));
	auto col = [&](const char* name) -> int {
		for (size_t i = 0; i < header.size(); i++) if (header[i] == name) return static_cast<int>(i);
		return -1;
	};
	int cFile = col("file"), cSaved = col("saved_at"), cReason = col("end_reason"), cDur = col("duration_s"),
		cTouch = col("touches"), cGoal = col("goal"), cShot = col("shot_key"), cEvent = col("event_tick"), cBounce = col("floor_before_goal"), cMode = col("mode");

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
		std::string ev = get(cEvent);
		it.eventTick = ev.empty() ? -1 : std::atoi(ev.c_str());
		it.floorBeforeGoal = get(cBounce) == "1";
		it.mode = get(cMode);
		if (it.mode.rfind("import:", 0) == 0) { it.imported = true; it.importedFrom = it.mode.substr(7); }
		auto r = ratings.find(it.file);
		it.rating = r == ratings.end() ? 0 : r->second;
		auto rr = recoveries.find(it.file);
		it.recoveryRating = rr == recoveries.end() ? 0 : rr->second;
		auto k = kept.find(it.file);
		it.kept = k != kept.end() && k->second;
		it.hasRecording = recorded.count(it.file) > 0;
		items.push_back(it);
	}

	std::reverse(items.begin(), items.end());
	return items;
}
