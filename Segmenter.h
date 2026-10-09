#pragma once
// Cuts a rep into segments: approach, takeoff, flight, contact (one per touch), result, recovery.
// Plain C++, no BakkesMod code.

#include "RepData.h"
#include <string>
#include <vector>

enum class SegKind { Approach, Takeoff, Flight, Contact, Result, Recovery };

struct Segment
{
	SegKind kind = SegKind::Approach;
	int start = 0;   // first tick (inclusive)
	int end = 0;     // last tick (inclusive)
	int touch = 0;   // which touch this belongs to (1-based) for contact and flight between touches
};

struct RepSegments
{
	std::vector<Segment> segs;
	std::vector<int> touchTicks;  // tick each touch was reported
	int startTick = 0;
	int firstJump = -1;
	int secondJump = -1;
	int lastTouch = -1;
	int recoveredTick = -1;       // back in control after the last touch (-1 = not within the rep)
	bool whiff = false;           // never touched the ball
};

namespace Segmenter
{
	RepSegments Split(const std::vector<Frame>& frames, int eventTick);
	const char* Name(SegKind k);

	// Back in control: wheels down (ground or wall) and nose roughly along the direction of travel
	bool InControl(const Frame& f);
}
