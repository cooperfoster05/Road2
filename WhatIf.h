#pragma once
// v3 "What if": from shortly before your shot, the simulator replays what you did, then tries small changes
// to your own inputs (flip timing and direction, flipping into the touch, boost, air roll, aim). Each try is
// graded with the normal grader, and the best one that still works when the start is nudged slightly is kept.
// Plain C++ plus RocketSim, no BakkesMod code. RocketSim must already be loaded (the ball predictor does that).

#include "RepData.h"
#include "ShotBook.h"
#include "Segmenter.h"
#include <atomic>
#include <utility>
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
	CleanerWayIn,   // stage 2: no goal found, but a way into the key moment that fixes what the coach flagged
	Cancelled
};

// Where the change happens: at the shot (stage 1), or on the way to the moment that set it up (stage 2:
// getting the reset, the setup touch of a double tap, the first touch of an air dribble). Engine (stage 3, Engine.h)
// isn't a search here: the plugin shows the engine line through the same result slot.
enum class WhatIfPoint { Shot, KeyMoment, Engine };

struct WhatIfResult
{
	WhatIfPoint point = WhatIfPoint::Shot;
	std::string what;         // stage 2: "the reset", "the setup touch", "the first touch"
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

	std::string fixes;        // CleanerWayIn: what it fixes, in words ("Closing on the ball at 34 km/h -> 12 km/h")
	std::string note;         // how the search went (for the hover)

	// The engine's line only (Oct 24): shown as the simplified, copyable version when that holds up
	bool  simplified = false;
	int   inputChanges = 0, inputChangesPerfect = 0;
	std::vector<std::pair<int, int>> fineSpans;   // where the engine's fine inputs had to stay ("fine aim here")
	int   depth = 1;
	int   tries = 0;
	float ms = 0;
};

// Timing windows (Oct 28): how much earlier or later a key press could have come and still worked. Info only:
// never part of a grade. The press and everything after it move together (you did the same thing, just sooner or
// later), one tick (8.3 ms) at a time, up to 18 ticks (150 ms) either way.
struct TimingWindow
{
	std::string what;          // "the jump off the wall", "the flip into the shot"
	int  tick = -1;            // your press
	bool checked = false;      // false: the sim couldn't follow your own inputs from here, so there's no window to show
	int  lo = 1, hi = 0;       // shifts (ticks, - = earlier) that still score as the same mechanic; lo > hi = none found
	bool loOpen = false, hiOpen = false;   // it still worked at the edge of what was tried (the window is at least this wide)
	int  cleanLo = 1, cleanHi = 0;         // the part of it that's a 4-5 (lo > hi = none)
	bool youIn = false;        // your timing is inside the window
	int  testedLo = 0, testedHi = 0;
	std::vector<signed char> result;       // per tested shift: -1 couldn't press there, 0 no, 1 scores, 2 scores as a 4-5
	float ms = 0;
};

namespace WhatIf
{
	// The key presses of the shot (the jump that left the surface, the flip into the shot) and their windows.
	// About 120 sim runs per press; a second or two on one core. Stops early if cancel is set.
	std::vector<TimingWindow> Timing(const std::vector<Frame>& frames, bool wentIn, bool floorBeforeGoal, int eventTick,
		const ShotReference& ref, int carBody, const std::atomic<bool>* cancel);

	// One plain sentence for a window, for the hover ("" when there's nothing worth showing)
	std::string TimingText(const TimingWindow& w);
	// The short version for the line beside the strip ("Flip into the shot: room 140 ms early, 85 ms late")
	std::string TimingShort(const TimingWindow& w);
	// Ticks as milliseconds, rounded to 5
	int TimingMs(int ticks);

	// The moment the search starts from: just before the flip that made the shot, or 0.3 s before the touch
	// (stage 2: up to half a second before the key moment). -1 if there's nothing to work from.
	int StartTick(const std::vector<Frame>& frames, WhatIfPoint point = WhatIfPoint::Shot);

	// Stage 2: does this rep have a key moment to work on, which breakdown line is it, and what to call it
	bool KeyMoment(const std::vector<Frame>& frames, SegKind& seg, std::string& what);

	// fixChecks (stage 2): the coach's checks that fire on this rep on the way to the key moment and are backed by
	// your reps. If no goal turns up, the search still looks for a way in that fixes them.
	// Runs the search; stops early if cancel is set or maxSeconds runs out.
	// depth 1: about 260 simulator runs (around a second). depth 2 ("search deeper"): about 3,000 (several seconds).
	// carBody: 0 Octane (Fennec and friends), 1 Dominus, 2 Plank (Batmobile), 3 Breakout, 4 Hybrid, 5 Merc.
	WhatIfResult Search(const std::vector<Frame>& frames, bool wentIn, bool floorBeforeGoal, int eventTick,
		const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int depth = 1,
		WhatIfPoint point = WhatIfPoint::Shot, const std::vector<int>* fixChecks = nullptr);

	const char* StatusText(WhatIfStatus s);
}
