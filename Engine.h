#pragma once
// v3 stage 3: the engine line. From a moment in your rep (by default, when the ball starts moving), a bot takes
// over your car with none of your inputs and plays the best shot it can find: it predicts the ball, picks when
// and where to meet it, drives, jumps and flies there, and hits it toward the goal. Every option is played out in
// the simulator and graded with the normal grader; the best one that also works from slightly nudged starts wins.
// It plays perfect inputs on purpose: it shows the ceiling for the shot, not what's comfortable.
// Plain C++ plus RocketSim, no BakkesMod code. RocketSim must already be loaded (the ball predictor does that).

#include "RepData.h"
#include "ShotBook.h"
#include <atomic>
#include <string>
#include <vector>

enum class EngineStatus { None, NoBall, NothingFound, Found, Cancelled };

struct EngineResult
{
	EngineStatus status = EngineStatus::None;
	int   startTick = -1;     // where the bot takes over
	std::string cue;          // "Engine: aerial off the ground at 0.42 s, meets it at 1.31 s, 1,150 high"
	std::string compare;      // how that differs from what you did, line by line
	std::string detail;       // the numbers, for the hover
	int   grade = 0;
	bool  wentIn = false;
	std::string gradeWhy;
	int   realGrade = 0;
	int   held = 0, heldOf = 0;
	std::vector<Frame> frames;    // your real rep up to startTick, then the engine's line
	int   eventTick = -1;
	int   touchTick = -1;     // when the engine hits it
	int   tries = 0;
	float ms = 0;
	std::string note;
};

namespace Engine
{
	// startTick -1 = from when the ball starts moving. depth 1 about 450 sim runs, depth 2 about 2,500.
	EngineResult Line(const std::vector<Frame>& frames, bool wentIn, bool floorBeforeGoal, int eventTick,
		const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick = -1, int depth = 1);
	const char* StatusText(EngineStatus s);
}
