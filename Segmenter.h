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

// Decides when a shot is over (goals come from the game, so they're handled by the caller).
// Aerial play ends when the ball hits the floor. Ground play doesn't: a rolling or bouncing ball is normal there,
// so it ends once the ball stops heading toward the goal (missed wide, stalled, or came back off the wall).
class ShotEndTracker
{
public:
	void Reset() { *this = ShotEndTracker(); }

	// Feed every recorded frame in order. Returns "" while the shot is live, otherwise "floor" or "miss",
	// with eventTick set to when it really ended.
	std::string Update(const Frame& f, bool hadPrev, float prevBallZ, int framesSoFar, int& eventTick);

	bool GroundPlay() const { return touches > 0 && groundTouch; }

private:
	bool carLeftFloor = false;   // jumped or drove up a wall before any touch
	bool groundTouch = false;    // the latest touch was a ground shot
	int  touches = 0;
	int  touchTick = -1;
	int  lastTouchFrame = -1000;
	int  goalSign = 0;           // which goal the ground shot was hit toward (+1 / -1), 0 = not known yet
	int  awayRun = 0;            // ticks in a row the ball hasn't been heading toward that goal
};

