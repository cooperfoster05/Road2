#pragma once
// The Combine (Oct 28): short, scored events for skills one shot can't grade (saves, recoveries, wall and backboard
// reads, first touches, and with a bot 50s, shadow defense and kickoffs). Each event runs as a set built from a seed: the
// same seed always gives the same set, so you can grind it, roll a new one, or share it with a friend.
// Plain C++ plus RocketSim (already loaded by the ball predictor). No BakkesMod code.
#include "RepData.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Combine
{
	struct Vec3 { float x = 0, y = 0, z = 0; };

	// ---- The possibility check ----
	// Could ANY car, driven perfectly, get to this ball before it crosses the goal line? Deliberately generous: the
	// car may turn instantly and push flat out in any direction, with a free jump and dodge. A shot is only thrown
	// out when even that can't make it, so hard-but-possible shots always stay in. The slack (how much spare time
	// even that perfect car has) is what sets a shot's difficulty.
	struct CarStart
	{
		Vec3 pos, vel;
		bool onSurface = true;   // on the floor, a wall or the ceiling (no gravity pulling it off the line yet)
		// The way it faces (radians, Unreal pitch / yaw / roll). Only the realistic estimate uses it.
		float pitch = 0, yaw = 0, roll = 0;
		bool hasFacing = false;
	};
	struct BallStart { Vec3 pos, vel, angVel; };

	struct ReachResult
	{
		bool  onTarget = false;   // the ball, left alone, goes in (within 4 s)
		float tCross = -1;        // when it fully crosses the line (s)
		bool  possible = false;   // a perfect car could touch it before then
		float tReach = -1;        // the earliest such touch (s)
		float slack = -1;         // tCross - tReach: spare time for a perfect car (bigger = easier)
		// The realistic estimate (difficulty only): turning to face it, real acceleration, real aerial rise
		float tReachReal = -1;    // -1: not reachable in time by the realistic estimate
		float slackReal = -9;
	};

	// goalSign: which goal the ball is heading for (+1 the orange goal at y = +5120, -1 the blue goal)
	ReachResult SaveReach(const BallStart& ball, const CarStart& car, int goalSign);

	// The generous reach test for one point at one time (exposed for tests)
	bool CanReach(const CarStart& car, const Vec3& target, float t);
	// The realistic one (a good player, not a perfect one): never used to throw a shot out, only to rate it
	bool CanReachReal(const CarStart& car, const Vec3& target, float t);

	// ---- Saves ----
	// One shot: where the ball starts and how it's struck, and where you start (always defending the blue goal, y = -5120)
	struct SaveShot
	{
		BallStart ball;
		CarStart car;             // with its facing (from the real defender, or made up)
		Vec3 carAngVel;
		float boost = 100;        // 0-100 (the real defender's, for real situations)
		float carYaw = 0;         // radians, the way you face (= car.yaw)
		float slack = 0;          // spare time for a perfect car (the check)
		float slackReal = 0;      // spare time by the realistic estimate (sets the level)
		float tCross = 0;         // how long until it would cross the line
		bool real = false;        // built from a real save in a match replay
	};
	// A real save situation (data/Road2/combine/save_situations.csv): the shot just after it was hit, and the
	// defender who saved it, turned so the ball heads for the blue goal
	struct SaveSituation
	{
		BallStart ball;
		CarStart car;
		Vec3 carAngVel;
		float boost = 100;
		char source = 'C';        // 'P' pro game, 'C' clip, '1'-'6' ranked (gold to SSL)
		// passes only (pass_situations.csv): where the receiver got it
		float recvZ = 0, recvSpeed = 0, recvDt = 0;
	};
	std::vector<SaveSituation> LoadSaveSituations(const std::string& csvText);
	constexpr int kSaveLevels = 5;
	// The spare-time band each level draws from (seconds): level 1 has plenty, level 5 almost none
	void SaveLevelBand(int level, float& minSlack, float& maxSlack);
	const char* SaveLevelName(int level);
	// The set for a seed: always the same shots for the same seed and level. Every shot is on target and possible.
	// With real situations, every shot is a real save (mirrored and nudged a little); without, they're made up.
	std::vector<SaveShot> MakeSaveSet(uint32_t seed, int level, int count = 10, const std::vector<SaveSituation>* real = nullptr);

	// ---- Recovery ----
	// One throw: you're launched into the air with a tilt and a spin, and have to land on your wheels and keep going.
	struct RecoveryThrow
	{
		Vec3 pos, vel, angVel;
		float pitch = 0, yaw = 0, roll = 0;   // radians
		float tHit = 0;           // when the car would first touch a surface if you did nothing (s)
		Vec3 normal;              // that surface's normal
		float tTurn = 0;          // a perfect car's time to turn its wheels to that surface (generous)
		float tTurnReal = 0;      // the same with each axis's real turn rate and the starting spin (for difficulty)
		float margin = 0;         // tHit - tTurnReal: spare time (bigger = easier)
		// The race back into the play (Oct 29): get to the target (by your back post, at most 3000 away) any way you like
		Vec3 land, landVel;       // where and how fast it would touch down if you did nothing
		Vec3 target;              // the spot to get to (on the floor)
		float par = 0;            // a good player's time to land well and get there (s, from the throw)
	};
	struct RecoveryCheck { bool possible = false; float tHit = -1, tTurn = 0, tTurnReal = 0; Vec3 normal, land, landVel; };
	constexpr float kRecoveryTargetRadius = 350.0f;
	// A good player's time to drive a path from a landing (speed, the part of it already heading there, how far off line)
	float RealDriveTime(float speed, float vAlong, float theta, float path);
	// Could a perfect car get its wheels down from here before it lands? Only "no" when even the fastest possible turn
	// (any axis at the roll rate, the starting spin ignored) can't make it.
	RecoveryCheck CheckRecovery(const Vec3& pos, const Vec3& vel, const Vec3& angVel, float pitch, float yaw, float roll);
	void RecoveryLevelBand(int level, float& minMargin, float& maxMargin);
	// The time cuts (as a share of par) for 3, 2 and 1 points: tighter at higher levels
	void RecoveryPointCuts(int level, float& three, float& two, float& one);
	std::vector<RecoveryThrow> MakeRecoverySet(uint32_t seed, int level, int count = 10);

	// ---- Reads (Oct 29) ----
	// The ball off a wall or a backboard: get a touch on it after it comes off and before it lands, and do something
	// with it. Kind 0: a shot off your own backboard (clear it); 1: a ball off the side wall in their half (score it);
	// 2: a shot off their backboard (score it). Every setup is checked in RocketSim: a good player (the realistic
	// estimate) can touch it inside that window; how much spare time they'd have sets the level.
	struct ReadSetup
	{
		BallStart ball;
		CarStart car;
		float boost = 50;
		int kind = 0;
		float tWall = 0, tFloor = 0;   // when it comes off the wall or backboard; when the read window ends (it lands, or drops below the level's height)
		float tReachReal = 0, slackReal = 0;   // the best moment to meet it, and how early a good player could be there
	};
	void ReadLevelBand(int level, float& minSlack, float& maxSlack);
	// The read has to come before the ball drops below this (ball centre; 0 = before it lands)
	float ReadMinHeight(int level);
	std::vector<ReadSetup> MakeReadSet(uint32_t seed, int level, int count = 10);

	// ---- First touches (Oct 29) ----
	// Real passes from match replays: the ball just after your teammate's touch, you where the receiver was (turned so
	// you attack the orange goal). Levels by how the ball arrives: on the ground, bouncing, lofted, aerial, high or fast.
	struct TouchShot
	{
		BallStart ball;
		CarStart car;
		Vec3 carAngVel;
		float boost = 50;
		float recvZ = 0, recvSpeed = 0, recvDt = 0;
		char source = 'C';
	};
	bool TouchLevelFits(int level, float recvZ, float recvSpeed);
	const char* TouchLevelNote(int level);
	std::vector<TouchShot> MakeTouchSet(uint32_t seed, int level, int count, const std::vector<SaveSituation>& passes);

	// ---- Bot events (Oct 30): 50s and shadow defense ----
	// Real moments from match replays. You play one real player; the bot plays back the other one's real path, and the
	// ball follows its real path until you get close (then it's free). Turned so you defend blue (-y). Levels by the
	// rank of the game it came from: gold and platinum up to SSL and pros.
	struct PathFrame { float t = 0; Vec3 pos, vel, angVel; float q[4] = { 1, 0, 0, 0 }; };
	struct BallFrame { float t = 0; Vec3 pos, vel, angVel; };
	struct Challenge
	{
		char kind = 'F';          // 'F' a 50/50, 'D' shadow defense
		char source = 'C';        // 'P' pro, 'C' clip, '1'-'6' ranked gold to SSL
		float boost = 50;
		float tContact = 0;       // a 50: when the real players met it (s from the start)
		float ballEnd = 0;        // when the ball stops following its real path
		float botEnd = 0;         // when the bot's path ends
		PathFrame you;            // where you start (t = 0)
		std::vector<PathFrame> bot;
		std::vector<BallFrame> ball;
	};
	std::vector<Challenge> LoadChallenges(const std::string& text);
	// The same, fast (Nov 10): the big CSVs (27 MB and 18 MB of text) took seconds to parse the first time a set was
	// built. The parsed moments are kept next to the CSV as <name>.bin and read back in one go; the .bin is rebuilt
	// whenever the CSV's size or date changes (an update's new file).
	std::vector<Challenge> LoadChallengesCached(const std::filesystem::path& csv);
	int ChallengeLevel(char source);   // 1-5 by the game's rank; 0 = not used
	const char* ChallengeLevelNote(int level);
	std::vector<Challenge> MakeChallengeSet(uint32_t seed, int level, int count, char kind, const std::vector<Challenge>& all);
	// When the bot's path first gets to the ball on the centre spot (s from the start; 99 if it never does): kickoffs
	float BotArrival(const Challenge& c);
	// Unreal pitch / yaw / roll (radians) from a rotation quaternion (w, x, y, z)
	void QuatToPYR(const float q[4], float& pitch, float& yaw, float& roll);
	// The bot's pose at time t (positions and speeds blended, rotation slerped); past its end, its last frame
	PathFrame BotPoseAt(const Challenge& c, float t);
	// The ball at time t on its real path; false once t is past it
	bool BallAt(const Challenge& c, float t, BallFrame& out);

	// Share codes: "SV3-1A2B3C4D" = saves, level 3, seed 1A2B3C4D
	std::string SetCode(const char* event, int level, uint32_t seed);
	bool ParseSetCode(const std::string& code, std::string& event, int& level, uint32_t& seed);
}
