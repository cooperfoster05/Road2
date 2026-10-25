#pragma once
// v2 "why" notes: how you moved into the touch, compared with your own goals on the same mechanic.
// Only habits your rated reps showed actually matter are used (Oct 11 analysis, see design doc):
//   nose on the ball before the touch (air mechanics), nose at the touch (aerials, ground shots),
//   boost use on aerials (misses over-boosted), car speed into the shot (dribbles, air dribbles, flip resets).
// Notes explain a grade; they never change it. Plain C++, no BakkesMod code.

#include "RepData.h"
#include "ShotGeometry.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct HabitMetrics
{
	bool  valid = false;
	float noseLate = -1;     // average angle between your nose and the ball over the last 0.5 s in the air before the touch (deg); -1 = not in the air
	float noseTouch = -1;    // the same angle as you hit it
	float boostFrac = -1;    // share of the air time before the touch you held boost; -1 = not in the air
	float carSpeed = 0;      // your car's speed at the touch
};

enum class Habit { NoseLate = 0, NoseTouch = 1, BoostFrac = 2, CarSpeed = 3, FlickChoice = 4 };

struct HabitNote
{
	Habit habit = Habit::NoseLate;
	std::string cue;         // what to do (shown)
	std::string detail;      // the numbers, against your goals (shown on hover)
	float severity = 0;      // how far past your usual, in spreads (bigger = mention first)
};

namespace Habits
{
	HabitMetrics Measure(const std::vector<Frame>& frames, const ShotAnalysis& shot);
}

// Your goals' typical numbers per shot type: the yardstick for the notes.
// Built in the background from every rep that went in; measurements are cached in habits_cache.csv.
class HabitBook
{
public:
	void Update(const std::filesystem::path& root);
	// Up to 2 notes for one rep (empty while the book is still loading, or when nothing stands out)
	std::vector<HabitNote> Notes(const ShotAnalysis& shot, const HabitMetrics& m);
	// Your flicks off a dribble: how often each kind goes in (settings readout)
	std::vector<std::string> FlickSummary();

private:
	struct Stat
	{
		int n = 0;
		double sum = 0, sum2 = 0;
		void Add(double v) { n++; sum += v; sum2 += v * v; }
		double Mean() const { return n ? sum / n : 0; }
		double Sd() const;
	};
	struct Entry { int type = 0; bool goal = false; int flip = 0; HabitMetrics m; };
	static constexpr int kTypes = 7, kHabits = 4, kFlips = 6;
	int flickN[kFlips] = {}, flickGoals[kFlips] = {};   // dribbles only, by FlipKind

	std::mutex mtx;
	std::unordered_map<std::string, Entry> entries;
	Stat stats[kTypes][kHabits];
	bool ready = false;
};
