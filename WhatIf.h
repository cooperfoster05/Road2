#pragma once
// v3 "What if": from shortly before your shot, the simulator replays what you did, then tries small changes
// to your own inputs (flip timing and direction, flipping into the touch, boost, air roll, aim). Each try is
// graded with the normal grader, and the best one that still works when the start is nudged slightly is kept.
// Plain C++ plus RocketSim, no BakkesMod code. RocketSim must already be loaded (the ball predictor does that).

#include "RepData.h"
#include "ShotBook.h"
#include <atomic>
#include <string>
#include <vector>

enum class WhatIfStatus
{
	None,           // not run
	NotNeeded,      // already a 5
	NoShot,         // no shot touch to work from
	CantSimulate,   // the sim's replay of your real inputs didn't match what happened, so its "what ifs" can't be trusted
	NothingBetter,  // no small change did better
	Found,
	Cancelled
};

struct WhatIfResult
{
	WhatIfStatus status = WhatIfStatus::None;
	int   startTick = -1;     // where the simulator takes over from your real rep
	int   realGrade = 0;      // your real shot's grade
	int   simGrade = 0;       // your real inputs replayed in the simulator

	// The best change (status Found)
	std::string cue;          // short, for people: "Flip into the touch (front flip)"
	std::string detail;       // the numbers behind it, for the hover
	int   grade = 0;          // what it would have been graded
	bool  wentIn = false;
	std::string gradeWhy;     // the grader's reason for that grade
	int   held = 0, heldOf = 0;   // how many slightly nudged starts it still worked from
	std::vector<Frame> frames;    // your real rep up to startTick, then the what-if
	int   eventTick = -1;

	std::string note;         // how the search went (for the hover)
	int   depth = 1;
	int   tries = 0;
	float ms = 0;
};

namespace WhatIf
{
	// The moment the search starts from: just before the flip that made the shot, or 0.3 s before the touch.
	// -1 if the rep has no shot to work from.
	int StartTick(const std::vector<Frame>& frames);

	// Runs the search; stops early if cancel is set or maxSeconds runs out.
	// depth 1: about 260 simulator runs (around a second). depth 2 ("search deeper"): about 3,000 (several seconds).
	// carBody: 0 Octane (Fennec and friends), 1 Dominus, 2 Plank (Batmobile), 3 Breakout, 4 Hybrid, 5 Merc.
	WhatIfResult Search(const std::vector<Frame>& frames, bool wentIn, bool floorBeforeGoal, int eventTick,
		const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int depth = 1);

	const char* StatusText(WhatIfStatus s);
}
