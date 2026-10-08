#pragma once
// Plain data types for a recorded rep. No BakkesMod code in here on purpose,
// so the recording format and later analysis can move to another platform.

#include <string>
#include <vector>

// One physics tick (120 per second) of a rep
struct Frame
{
	int   tick = 0;          // tick index within the rep, starts at 0
	float t = 0.0f;          // seconds since rep start (tick / 120)
	float wallMs = 0.0f;     // real milliseconds since rep start (to verify tick rate)

	// Controller inputs
	float throttle = 0, steer = 0, pitch = 0, yaw = 0, roll = 0;
	bool  jump = false, boost = false, handbrake = false;

	// Car state (Unreal units; rotation in Unreal rotator units, 65536 = 360 degrees)
	float carX = 0, carY = 0, carZ = 0;
	int   carPitch = 0, carYaw = 0, carRoll = 0;
	float carVX = 0, carVY = 0, carVZ = 0;
	float carAVX = 0, carAVY = 0, carAVZ = 0;
	float boostAmount = 0;   // 0-100
	bool  onGround = false, onWall = false, hasFlip = false;

	// Ball state
	float ballX = 0, ballY = 0, ballZ = 0;
	float ballVX = 0, ballVY = 0, ballVZ = 0;
	float ballAVX = 0, ballAVY = 0, ballAVZ = 0;

	// Ball touch by this car (reported by the game, lands on the tick after contact)
	bool  touch = false;
	float touchX = 0, touchY = 0, touchZ = 0;
};

// Summary of a finished rep, written to the reps index
struct RepSummary
{
	std::string fileName;
	std::string savedAt;
	std::string mode;        // "freeplay" or "training"
	std::string map;
	std::string endReason;   // goal, floor, timeout, reset
	float durationS = 0;
	int   ticks = 0;
	int   touches = 0;
	bool  goal = false;
};
