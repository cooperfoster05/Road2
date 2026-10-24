#pragma once
// The engine's sim start and tick, for tools outside the engine (the bot trainer and its benchmark).
// Same physics reset as every engine run: fresh wheels, the recorded grip, every boost pad up, the same arena clock.
// So a line played here plays out exactly like the engine would play it.
// Plain C++ plus RocketSim, no BakkesMod code.

#include "RepData.h"
#include <vector>

namespace RocketSim { class Arena; class Car; struct CarControls; struct CarConfig; }

namespace EngineSim
{
	// Car and ball exactly where the recording had them at tick s. nudge > 0: the engine's robustness nudge number
	// (the checks use 301-306), so a line checked here gets the same nudged starts as an engine line.
	void Start(RocketSim::Arena* arena, RocketSim::Car* car, const std::vector<Frame>& fr, int s, int nudge = 0);

	// One tick with these controls. `prev` is the frame for the current tick (its inputs get filled in);
	// returns the next tick's frame, with the touch flagged the way the engine flags it.
	Frame Step(RocketSim::Arena* arena, RocketSim::Car* car, const RocketSim::CarControls& c, Frame& prev);

	// The car body by the plugin's numbering (0 Octane, 1 Dominus, 2 Plank, 3 Breakout, 4 Hybrid, 5 Merc)
	const RocketSim::CarConfig& Body(int carBody);
}
