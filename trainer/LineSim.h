#pragma once
// Plays a line out in the sim exactly like an engine line (same start, same tick, same end rules, same grading and
// the same shot-type lock), with the inputs coming from outside one tick at a time: the bot's policy, or recorded
// inputs being replayed for the robustness check.
#include "Data.h"
#include "../Grader.h"
#include "../Segmenter.h"
#include "../third_party/RocketSim/src/RocketSim.h"
#include <memory>
#include <random>
#include <string>
#include <vector>

struct LineResult
{
	bool  touched = false;
	bool  wentIn = false, floorFirst = false;
	int   eventTick = -1, touchTick = -1;
	ShotAnalysis shot;
	RepGrade grade;
	bool  sameMechanic = false;
	std::string why;          // why the lock said no
	float quality = 0;        // the engine's Quality(): grade first, then how close to the next grade
	std::vector<Frame> frames;
	std::vector<RocketSim::CarControls> ctl;   // inputs by tick (index = rep tick), for exact replays
};

class LineSim
{
public:
	explicit LineSim(int carBody = 0);
	RocketSim::Arena* arena() { return A.get(); }
	RocketSim::Car* car() { return C; }

	// Start from tick s of the rep. nudge: the engine's numbered nudge (0 = none).
	void Start(const Rep& rep, int s, int nudge = 0);
	// Extra random jitter on top (training only): car / ball position and velocity, in uu and uu/s
	void Jitter(std::mt19937& rng, float carPos, float carVel, float ballPos, float ballVel);

	// One tick with these inputs. Returns false once the line is over (then call Finish).
	bool Tick(const RocketSim::CarControls& c);
	// Replays of recorded inputs play every input as recorded (the engine's replays don't hand off either)
	void ReplayMode() { handsOffRule = false; }
	bool HandsOff() const { return handOff >= 0 && tick >= handOff; }   // the hit is done: the bot lets go
	bool Touched() const { return firstTouch >= 0; }
	int  TickNow() const { return tick; }
	int  StartTick() const { return s0; }
	const Frame& Now() const { return res.frames.back(); }
	int  NoTouchDeadline() const { return deadline; }

	// Plays the rest hands-off (if not over yet) and grades it
	const LineResult& Finish();
	LineResult& Result() { return res; }

	// The engine's Quality() for a finished line (copied from Engine.cpp so it scores lines the same way)
	static float Quality(const LineResult& r, const ShotReference& ref);

private:
	std::unique_ptr<RocketSim::Arena> A;
	RocketSim::Car* C = nullptr;
	const Rep* rep = nullptr;
	LineResult res;
	int s0 = 0, tick = 0, maxTick = 0, deadline = 0;
	int firstTouch = -1, lastTouch = -1, handOff = -1, stopAt = -1;
	bool bounced = false, over = false, finished = false, handsOffRule = true;
};
