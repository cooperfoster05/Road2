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
enum class ShotType { Aerial = 0, Ground = 1, DoubleTap = 2 };

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

	// Double taps: the ball came off the backboard (no floor bounce) right before your final touch
	bool  doubleTap = false;
	int   backboardTick = -1;     // when it hit the backboard
	float backboardZ = 0;         // how high (ball center)
	float backboardX = 0;         // how far from the middle (+/- sideways)
	int   setupTouchTick = -1;    // your touch that sent it there

	// Your car at the touch
	bool  carGrounded = false;    // wheels on the ground when you hit it
	float carSpeed = 0;           // car speed into the touch
	float approachAngleDeg = 0;   // angle between where your car was driving and the push the ball needed (ground view)
};

namespace ShotGeometry
{
	const char* TypeName(ShotType t);

	// Ball bouncing off the back wall (outside the goal mouth) in ticks [from, to]: the last such tick, or -1
	int LastBackboard(const std::vector<Frame>& frames, int from, int to);
	bool IsBackboardHit(const Frame& before, const Frame& after);   // frames two ticks apart
	ShotAnalysis Analyze(const std::vector<Frame>& frames);
}
