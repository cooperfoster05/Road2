#pragma once
// v2 coaching playbook: known mistakes for each mechanic (from how top players and coaches teach it),
// each with a check on the recording and a short cue. The coach walks each mechanic's chain in order
// (setup -> approach -> the key moment -> the finish) and names the earliest broken step as the root cause.
//
// A check only speaks once your own rated reps back it up: it has to fire clearly more often on your misses
// (rated 1-2) than on your goals. Everything else stays hidden, so the coach only says what it can stand behind.
// Plain C++, no BakkesMod code.

#include "Habits.h"
#include "RepData.h"
#include "Segmenter.h"
#include "ShotGeometry.h"
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct CoachNote
{
	std::string label;        // "Root cause:" on a miss, "Fix first:" on a goal that could be better, "Also:"
	std::string cue;          // what to do, in plain words (shown)
	std::string detail;       // the number behind it (shown on hover)
	bool rootCause = false;   // the earliest broken step in the chain
	SegKind seg = SegKind::Approach;   // the part of the rep it happened in (its Practice button gets highlighted)
};

namespace Coach
{
	inline constexpr int kChecks = 13;
	using Values = std::array<float, kChecks>;   // one per check; negative = doesn't apply to this rep

	Values Measure(const std::vector<Frame>& frames, const ShotAnalysis& shot);
	// Up to 2 notes: the root cause first, then one more check or your own-habit note
	std::vector<CoachNote> Combine(const std::vector<int>& firing, const Values& v, int overall, ShotType type,
		const std::vector<HabitNote>& habits);
	const char* CheckId(int check);
}

// Which checks your rated reps back up, per mechanic (rebuilt in the background, measurements cached in coach_cache.csv)
class CoachBook
{
public:
	void Update(const std::filesystem::path& root);
	// Checks that fire on this rep and are backed by your data, in chain order (empty while loading)
	std::vector<int> Firing(const ShotAnalysis& shot, const Coach::Values& v);
	// overall = the rep's auto grade: a 4 or 5 gets no notes (nothing to fix)
	std::vector<CoachNote> Notes(const std::vector<Frame>& frames, const ShotAnalysis& shot, int overall, HabitBook& habits);
	// One line per check: whether your reps back it, and how often it fired on misses vs goals (settings readout)
	std::vector<std::string> Summary();

private:
	struct Entry { int type = 0; Coach::Values v{}; };
	std::mutex mtx;
	std::array<bool, Coach::kChecks> backed{};
	std::array<int, Coach::kChecks> badN{}, badFire{}, goodN{}, goodFire{};
	bool ready = false;
};
