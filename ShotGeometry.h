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
enum class ShotType { Aerial = 0, Ground = 1, DoubleTap = 2, Dribble = 3, AirDribble = 4, FlipReset = 5 };

// An air dribble that ended in your final touch: touches in a row in the air, no landing, ball kept close
struct AirDribbleSpan
{
	bool  valid = false;
	int   start = -1;             // your car left the ground or wall
	int   firstTouch = -1;        // first touch of the air chain
	int   touches = 0;            // touches in the air chain, the final touch included
	float avgImpulse = 0;         // average speed change you gave the ball per touch before the final one (small = carried)
	float closeFrac = 0;          // share of the air time the ball was within 230 of your car
	float meanDist = 0;           // average ball-to-car distance in the air
};

// Which way you flipped: front, diagonal front, side (a "90"), diagonal back (a "45"), back
enum class FlipKind { None = 0, Front = 1, DiagonalFront = 2, Side = 3, DiagonalBack = 4, Back = 5 };

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
	bool  bouncedIn = false;      // it hit the floor on the way to the goal line (crossbar and post rattles don't count)
	float cornerDist = 0;         // distance from the nearest top corner of the goal (ball-center units)

	// Double taps: the ball came off the backboard, ceiling or a wall top (no floor bounce) right before your final touch.
	// Triple and quad taps count too: this describes the last tap before the final touch.
	bool  doubleTap = false;
	int   backboardTick = -1;     // when it hit the backboard
	float backboardZ = 0;         // how high (ball center)
	float backboardX = 0;         // how far from the middle (+/- sideways)
	int   setupTouchTick = -1;    // your touch that sent it there
	int   tapSurface = 0;         // 1 backboard, 2 ceiling, 3 top of a side wall / corner
	float reboundDist = 0;        // how far the ball had come off the wall when you hit it again (a pinch against the wall is ~0)

	// Dribbles: the ball rode on your roof into the final touch (the flick). Shot numbers above are measured
	// once the ball has left your car.
	bool  dribble = false;
	int   dribbleStart = -1;      // ball settled on your roof
	int   flickJumpTick = -1;     // jump that started the flick (-1 = no jump)
	float dribbleSec = 0;         // time carried on the roof before the flick
	float dribblePace = 0;        // average ground speed of the ball while you carried it
	float flickLift = 0;          // upward speed the flick gave the ball (how much it rose off your car)

	// Air dribbles: touches in a row in the air leading into the final touch
	bool  airDribble = false;
	int   airStart = -1;          // your car left the ground or wall
	int   airFirstTouch = -1;
	int   airTouches = 0;         // the final touch included
	float airSec = 0;             // from leaving the ground/wall to the final touch
	float airImpulse = 0;         // average push per touch before the final one (small = carried, big = hit)
	float airCloseFrac = 0;       // share of the air time the ball stayed within 230 of your car

	// Flip resets: your wheels touched the ball in the air and gave you your flip back before the final touch
	bool  flipReset = false;
	int   resetTick = -1;         // the (last) reset before the final touch
	int   resets = 0;             // resets during the play
	int   resetTakeoff = -1;      // your car left the ground or wall on the way to the reset
	int   resetDodge = -1;        // the flip you used after the reset (-1 = never flipped)
	bool  flipShot = false;       // the final touch came off that flip (within 1/3 s of starting it)
	bool  resetAfterAirDribble = false;   // air dribble touches before the reset

	// The flip that made the shot: the flick on a dribble, the flip off the reset on a flip reset
	int   flipTick = -1;
	FlipKind flipKind = FlipKind::None;
	bool  flipRight = false;      // flipped to your right
	float flipDelay = 0;          // dribbles: time from the jump to the flip

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
	// The chain of touches in the air that ends at `finalTouch` (a touch tick)
	AirDribbleSpan FindAirDribble(const std::vector<Frame>& frames, int finalTouch);
	// Touching the floor, a wall or the ceiling (the game also reports "on ground" for a moment when your wheels
	// touch the ball in the air: that's a flip reset, not a surface)
	bool OnSurface(const Frame& f);
	// You got your flip back in the air with the ball right there: a flip reset (needs the car's flip state)
	bool IsFlipReset(const Frame& before, const Frame& now);

	ShotAnalysis Analyze(const std::vector<Frame>& frames);

	// From the stick at the moment you flipped
	FlipKind ClassifyFlip(const Frame& atFlip, bool& right);
	// "45 flick", "90 flick", "front flip"... (flick = off a dribble)
	std::string FlipName(FlipKind kind, bool flick);
}
