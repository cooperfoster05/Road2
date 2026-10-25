#include "pch.h"
#include "Habits.h"
#include "Grader.h"
#include "RepLibrary.h"
#include "Units.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace
{
	constexpr float kPi = 3.14159265f;
	constexpr int   kLateTicks = 60;      // "the last half second"
	constexpr int   kMinAirTicks = 12;
	constexpr int   kMinGoals = 8;        // need this many goals of a type before comparing against them
	constexpr float kNoteSpread = 1.0f;   // worse than your goals' average by this many spreads = worth a note

	float NoseAngle(const Frame& f)
	{
		float p = f.carPitch * kPi / 32768.0f, y = f.carYaw * kPi / 32768.0f;
		float fx = std::cos(p) * std::cos(y), fy = std::cos(p) * std::sin(y), fz = std::sin(p);
		float dx = f.ballX - f.carX, dy = f.ballY - f.carY, dz = f.ballZ - f.carZ;
		float len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1e-3f) return 0.0f;
		float c = std::max(-1.0f, std::min(1.0f, (fx * dx + fy * dy + fz * dz) / len));
		return std::acos(c) * 180.0f / kPi;
	}

	// Which habits apply to each shot type, and which way is worse (+1 = higher is worse, -1 = lower is worse)
	struct Rule { Habit habit; int worse; float minGap; };
	std::vector<Rule> RulesFor(ShotType t)
	{
		switch (t)
		{
		// (Holding boost on aerials and slow flip reset shots are coach checks now, see Coach.cpp)
		case ShotType::Aerial:     return { { Habit::NoseTouch, +1, 8.0f } };
		case ShotType::Ground:     return { { Habit::NoseTouch, +1, 8.0f } };
		case ShotType::DoubleTap:  return { { Habit::NoseLate, +1, 8.0f } };
		case ShotType::Dribble:    return { { Habit::CarSpeed, -1, 150.0f } };
		case ShotType::AirDribble: return { { Habit::NoseLate, +1, 8.0f }, { Habit::CarSpeed, -1, 150.0f } };
		case ShotType::FlipReset:  return { { Habit::NoseLate, +1, 8.0f } };
		case ShotType::CeilingShot: return { { Habit::NoseLate, +1, 8.0f } };
		}
		return {};
	}

	float Value(const HabitMetrics& m, Habit h)
	{
		switch (h)
		{
		case Habit::NoseLate:  return m.noseLate;
		case Habit::NoseTouch: return m.noseTouch;
		case Habit::BoostFrac: return m.boostFrac;
		case Habit::CarSpeed:  return m.carSpeed;
		}
		return -1;
	}

	const char* TypeWord(ShotType t)
	{
		switch (t)
		{
		case ShotType::Ground:     return "ground shot";
		case ShotType::DoubleTap:  return "double tap";
		case ShotType::Dribble:    return "dribble";
		case ShotType::AirDribble: return "air dribble";
		case ShotType::FlipReset:  return "flip reset";
		case ShotType::CeilingShot: return "ceiling shot";
		default:                   return "aerial";
		}
	}

	std::string CachePath(const std::filesystem::path& root) { return (root / "habits_cache.csv").string(); }
	std::string CacheHeader() { return std::format("file,type,goal,nose_late,nose_touch,boost_frac,car_speed,flip,v{}", Grader::kVersion); }
	constexpr int kMinFlicks = 15;          // tries of a flick before comparing it
	constexpr float kFlickGap = 0.20f;      // another flick of yours goes in this much more often = worth a note
}

HabitMetrics Habits::Measure(const std::vector<Frame>& fr, const ShotAnalysis& shot)
{
	HabitMetrics m;
	const int n = static_cast<int>(fr.size());
	const int k = shot.touchTick;
	if (!shot.valid || k < 2 || k >= n) return m;

	// The stretch in the air right before the touch (the ball touching your wheels isn't a landing)
	int takeoff = k - 1;
	while (takeoff > 0 && !ShotGeometry::OnSurface(fr[takeoff])) takeoff--;
	takeoff++;
	int airTicks = k - takeoff;
	if (airTicks >= kMinAirTicks)
	{
		float sum = 0;
		int cnt = 0, boost = 0;
		for (int i = takeoff; i < k; i++)
		{
			if (fr[i].boost) boost++;
			if (i >= k - kLateTicks) { sum += NoseAngle(fr[i]); cnt++; }
		}
		m.noseLate = cnt ? sum / cnt : -1.0f;
		m.boostFrac = static_cast<float>(boost) / airTicks;
	}
	m.noseTouch = NoseAngle(fr[k - 1]);
	const Frame& c = fr[k];
	m.carSpeed = std::sqrt(c.carVX * c.carVX + c.carVY * c.carVY + c.carVZ * c.carVZ);
	m.valid = true;
	return m;
}

double HabitBook::Stat::Sd() const
{
	if (n < 2) return 0;
	double mean = Mean();
	return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
}

void HabitBook::Update(const std::filesystem::path& root)
{
	std::unordered_map<std::string, Entry> loaded;
	bool cacheOk = false;
	{
		std::ifstream in(CachePath(root));
		std::string line;
		if (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			cacheOk = line == CacheHeader();
		}
		while (cacheOk && std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::vector<std::string> p;
			std::stringstream ss(line);
			std::string cell;
			while (std::getline(ss, cell, ',')) p.push_back(cell);
			if (p.size() < 8) continue;
			Entry e;
			e.type = std::atoi(p[1].c_str());
			e.goal = p[2] == "1";
			e.m.noseLate = std::strtof(p[3].c_str(), nullptr);
			e.m.noseTouch = std::strtof(p[4].c_str(), nullptr);
			e.m.boostFrac = std::strtof(p[5].c_str(), nullptr);
			e.m.carSpeed = std::strtof(p[6].c_str(), nullptr);
			e.flip = std::atoi(p[7].c_str());
			e.m.valid = true;
			loaded[p[0]] = e;
		}
	}

	// Measure every rep with a recording that isn't cached yet (the grader version changed = start over)
	std::ofstream out;
	if (cacheOk) out.open(CachePath(root), std::ios::app);
	else
	{
		out.open(CachePath(root), std::ios::trunc);
		if (out) out << CacheHeader() << '\n';
	}
	for (const RepListItem& item : RepLibrary::ListReps(root))
	{
		if (loaded.count(item.file) || !item.hasRecording || item.imported || item.endReason == "reset") continue;
		std::vector<Frame> frames;
		std::string err;
		if (!RepLibrary::LoadRep(root, item.file, frames, err)) continue;
		ShotAnalysis shot = ShotGeometry::Analyze(frames);
		if (!shot.valid) continue;
		Entry e;
		e.type = static_cast<int>(shot.type);
		e.goal = item.goal;
		e.m = Habits::Measure(frames, shot);
		if (!e.m.valid) continue;
		e.flip = shot.type == ShotType::Dribble && shot.flipTick >= 0 ? static_cast<int>(shot.flipKind) : 0;
		loaded[item.file] = e;
		if (out) out << std::format("{},{},{},{:.2f},{:.2f},{:.3f},{:.0f},{}\n", item.file, e.type, e.goal ? 1 : 0,
			e.m.noseLate, e.m.noseTouch, e.m.boostFrac, e.m.carSpeed, e.flip);
	}

	// Yardsticks: your goals, per shot type
	Stat fresh[kTypes][kHabits];
	int fN[kFlips] = {}, fGoals[kFlips] = {};
	for (const auto& [file, e] : loaded)
	{
		if (e.type == static_cast<int>(ShotType::Dribble) && e.flip > 0 && e.flip < kFlips)
		{
			fN[e.flip]++;
			if (e.goal) fGoals[e.flip]++;
		}
		if (!e.goal || e.type < 0 || e.type >= kTypes) continue;
		for (int h = 0; h < kHabits; h++)
		{
			float v = Value(e.m, static_cast<Habit>(h));
			if (v >= 0) fresh[e.type][h].Add(v);
		}
	}

	std::lock_guard<std::mutex> lock(mtx);
	entries = std::move(loaded);
	for (int t = 0; t < kTypes; t++)
		for (int h = 0; h < kHabits; h++) stats[t][h] = fresh[t][h];
	for (int f = 0; f < kFlips; f++) { flickN[f] = fN[f]; flickGoals[f] = fGoals[f]; }
	ready = true;
}

std::vector<HabitNote> HabitBook::Notes(const ShotAnalysis& shot, const HabitMetrics& m)
{
	std::vector<HabitNote> notes;
	if (!shot.valid || !m.valid) return notes;
	int t = static_cast<int>(shot.type);
	if (t < 0 || t >= kTypes) return notes;

	std::lock_guard<std::mutex> lock(mtx);
	if (!ready) return notes;
	for (const Rule& r : RulesFor(shot.type))
	{
		const Stat& s = stats[t][static_cast<int>(r.habit)];
		float v = Value(m, r.habit);
		if (s.n < kMinGoals || v < 0) continue;
		double mean = s.Mean(), sd = std::max(s.Sd(), 1e-6);
		double gap = (v - mean) * r.worse;          // > 0 = worse than your goals
		if (gap < r.minGap || gap / sd < kNoteSpread) continue;

		HabitNote note;
		note.habit = r.habit;
		note.severity = static_cast<float>(gap / sd);
		const char* word = TypeWord(shot.type);
		switch (r.habit)
		{
		case Habit::NoseLate:
			note.cue = "Point your nose at the ball as you come in";
			note.detail = std::format("Nose was {:.0f} deg off the ball in the last half second (your {} goals: about {:.0f})", v, word, mean);
			break;
		case Habit::NoseTouch:
			note.cue = "Point your nose at the ball when you hit it";
			note.detail = std::format("Nose was {:.0f} deg off the ball at the touch (your {} goals: about {:.0f})", v, word, mean);
			break;
		case Habit::BoostFrac:
			note.cue = "Feather your boost instead of holding it";
			note.detail = std::format("Boosted {:.0f}% of the flight (your {} goals: about {:.0f}%)", v * 100.0f, word, mean * 100.0);
			break;
		case Habit::CarSpeed:
			note.cue = "Carry more speed into the shot";
			note.detail = std::format("Hit it at {} (your {} goals average {})", Units::Speed(v), word, Units::Speed(static_cast<float>(mean)));
			break;
		}
		notes.push_back(note);
	}
	std::sort(notes.begin(), notes.end(), [](const HabitNote& a, const HabitNote& b) { return a.severity > b.severity; });

	// Flick choice: another kind of flick of yours goes in a lot more often (needs 15+ tries of each)
	int k = static_cast<int>(shot.flipKind);
	if (shot.type == ShotType::Dribble && shot.flipTick >= 0 && k > 0 && k < kFlips && flickN[k] >= kMinFlicks)
	{
		double mine = static_cast<double>(flickGoals[k]) / flickN[k];
		int best = -1;
		double bestRate = 0;
		for (int f = 1; f < kFlips; f++)
		{
			if (f == k || flickN[f] < kMinFlicks) continue;
			double r = static_cast<double>(flickGoals[f]) / flickN[f];
			if (r > bestRate) { bestRate = r; best = f; }
		}
		if (best > 0 && bestRate - mine >= kFlickGap)
		{
			HabitNote note;
			note.habit = Habit::FlickChoice;
			std::string other = ShotGeometry::FlipName(static_cast<FlipKind>(best), true);
			std::string used = ShotGeometry::FlipName(shot.flipKind, true);
			note.cue = std::format("Your {}s go in more often than your {}s", other, used);
			note.detail = std::format("{}: {:.0f}% of {} in   {}: {:.0f}% of {} in", other, bestRate * 100.0, flickN[best],
				used, mine * 100.0, flickN[k]);
			note.severity = 0.5f;
			notes.push_back(note);
		}
	}
	if (notes.size() > 2) notes.resize(2);
	return notes;
}

std::vector<std::string> HabitBook::FlickSummary()
{
	std::vector<std::string> out;
	std::lock_guard<std::mutex> lock(mtx);
	if (!ready) return out;
	for (int f = 1; f < kFlips; f++)
	{
		if (flickN[f] == 0) continue;
		out.push_back(std::format("{}: {} tries, {:.0f}% went in", ShotGeometry::FlipName(static_cast<FlipKind>(f), true),
			flickN[f], 100.0 * flickGoals[f] / flickN[f]));
	}
	return out;
}
