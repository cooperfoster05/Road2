#pragma once
// Shot math for one rep: where the shot was going, how far off target, where on the ball
// you should have hit it, and how early you could have reached the ball.
// Plain C++, no BakkesMod code. Tuned on recorded reps (see design doc).

#include "RepData.h"
#include <string>
#include <vector>

struct V3
{
	float x = 0, y = 0, z = 0;
};

// Which grader a rep goes to, decided from the shot touch
enum class ShotType { Aerial = 0, Ground = 1, DoubleTap = 2, Dribble = 3 };

// A dribble that ended in your final touch: the ball rode on your roof, then you flicked it (or it came off)
struct DribbleSpan
{
	bool valid = false;
	int  start = -1;              // ball settled on your roof (the carry before the flick)
	int  lastRoof = -1;           // last tick it was still on your roof
	int  flickJump = -1;          // the jump that started the flick (-1 = no jump: it rolled off or you bumped it)
	int  release = -1;            // last tick of your final touch (the ball leaves the car)
};

struct ShotAnalysis
{
	ShotType type = ShotType::Aerial;
	bool  valid = false;          // false if the rep has no touch to analyze
	int   startTick = 0;          // ball starts moving
	int   touchTick = -1;         // frame that reports the shot touch (the last touch of the rep)
	float goalSign = 1;           // +1 = shooting at the +Y goal

	// Where the shot was headed
	bool  reachesGoalLine = false;
	float crossX = 0, crossZ = 0; // ball center at the goal line
	float missDist = 0;           // 0 = on target; otherwise units outside the goal mouth
	std::string missDir;          // "", "left", "right", "high", "low", or combos

	// Aim and contact
	float aimErrDeg = 0;          // angle between your shot and the closest on-target shot at the same speed
	V3    ballCenter;             // ball center at contact
	V3    actualContact;          // where you hit the ball
	V3    idealContact;           // where on the ball would have sent it on target
	V3    shotDir;                // unit direction the ball left your car
	V3    idealDir;               // unit direction it needed to leave
	float contactErr = 0;         // distance between actual and ideal contact points (units)
	float contactUp = 0;          // + means you hit higher on the ball than ideal
	float contactLeft = 0;        // + means you hit further left on the ball than ideal (your view toward goal)

	// Timing
	int   earliestTick = -1;      // earliest tick a fast, direct aerial could have reached the ball
	float lateBy = 0;             // seconds between that and your touch
	bool  earliestValid = false;  // where to hit the ball at that earliest moment to score
	V3    earliestBall;           // ball center at earliestTick
	V3    earliestContact;        // ideal contact point on the ball then

	// Shot quality
	float shotSpeed = 0;          // ball speed off your car
	bool  dipsToFloor = false;    // on its current path the ball hits the floor before the goal line
	float cornerDist = 0;         // distance from the nearest top corner of the goal (ball-center units)

	// Double taps: the ball came off the backboard, ceiling or a wall top (no floor bounce) right before your final touch.
	// Triple and quad taps count too: this describes the last tap before the final touch.
	bool  doubleTap = false;
	int   backboardTick = -1;     // when it hit the backboard
	float backboardZ = 0;         // how high (ball center)
	float backboardX = 0;         // how far from the middle (+/- sideways)
	int   setupTouchTick = -1;    // your touch that sent it there
	int   tapSurface = 0;         // 1 backboard, 2 ceiling, 3 top of a side wall / corner

	// Dribbles: the ball rode on your roof into the final touch (the flick). Shot numbers above are measured
	// once the ball has left your car.
	bool  dribble = false;
	int   dribbleStart = -1;      // ball settled on your roof
	int   flickJumpTick = -1;     // jump that started the flick (-1 = no jump)
	float dribbleSec = 0;         // time carried on the roof before the flick
	float dribblePace = 0;        // average ground speed of the ball while you carried it
	float flickLift = 0;          // upward speed the flick gave the ball (how much it rose off your car)

	// Your car at the touch
	bool  carGrounded = false;    // wheels on the ground when you hit it
	float carSpeed = 0;           // car speed into the touch
	float approachAngleDeg = 0;   // angle between where your car was driving and the push the ball needed (ground view)
};

namespace ShotGeometry
{
	const char* TypeName(ShotType t);

	// "Wall taps": the ball bouncing off the backboard, the ceiling, or the top of a side wall / corner
	// (not the floor, not inside the goal). Checked on frames two ticks apart.
	// Surface: 0 none, 1 backboard, 2 ceiling, 3 top of a side wall or corner.
	int  WallTapSurface(const Frame& before, const Frame& after);
	bool IsWallTap(const Frame& before, const Frame& after);
	int  LastWallTap(const std::vector<Frame>& frames, int from, int to);   // last tap tick in [from, to], or -1

	// Ball sitting on your roof (dribbling)
	bool BallOnRoof(const Frame& f);
	// Ball on your roof, or being pushed along right in front of / beside you at your speed (pickups, hood dribbles)
	bool BallWithCar(const Frame& f);
	// The roof carry that leads into the touch ending at `release` (last tick of your final touch)
	DribbleSpan FindDribble(const std::vector<Frame>& frames, int release);

	ShotAnalysis Analyze(const std::vector<Frame>& frames);
}
