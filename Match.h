#pragma once
// Match review: a scanned replay (RLCoachMatch.cpp writes it) read back and broken down. Plain C++, no BakkesMod code.
//
// Step 1 (Nov 11): what happened and when. Possession, kickoffs, 50s, passes, clears, giveaways, shots, saves, goals,
// demos and boost, plus each player's numbers. No grades yet: those come with the scoring-chance line (step 3).
// The pitch: blue (team 0) defends the goal at -Y and attacks +Y; orange (team 1) the other way.

#include "RepData.h"
#include <filesystem>
#include <string>
#include <vector>

struct MatchPlayer
{
	int id = 0, team = 0;
	std::string name;
};

struct MatchCar
{
	bool present = false;          // false while demolished or not spawned
	float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
	int pitch = 0, yaw = 0, roll = 0;
	float boost = 0;               // 0-100
	bool onGround = false, supersonic = false;
	// what a rep needs on top of that (for opening a shot like a rep)
	float avx = 0, avy = 0, avz = 0;
	bool jumped = false, doubleJumped = false, handbrake = false;
	float throttle = 0, steer = 0; // -1 to 1
	int onWall = -1, hasFlip = -1; // -1 = the scan didn't record it (scans before Nov 12)
};

struct MatchFrame
{
	int frame = 0;                 // the replay's frame number
	float time = 0;                // seconds into the replay
	int clock = 0;                 // seconds left on the match clock
	bool overtime = false;
	int score[2] = { 0, 0 };
	bool hasBall = false;
	float bx = 0, by = 0, bz = 0, bvx = 0, bvy = 0, bvz = 0;
	std::vector<MatchCar> cars;    // by player id
};

struct MatchData
{
	std::string replayName, replayFile, recordedBy;   // recordedBy: the player whose replay it is
	int replayFrames = 0, fps = 30;
	std::vector<MatchPlayer> players;
	std::vector<MatchFrame> frames;
	struct Touch { int frame = 0, player = 0; };
	std::vector<Touch> touches;    // as the game reported them (may repeat)
	struct Counter { int frame = 0, player = 0, touches = 0, shots = 0, saves = 0, goals = 0, assists = 0, score = 0; };
	std::vector<Counter> counters; // a row each time one of a player's counters changed
};

enum class MomentKind { Kickoff, Goal, Shot, Save, Assist, Fifty, Pass, Clear, Giveaway, Demo, NoBoost, WastedBoost,
	DoubleCommit, Beaten, OpenNet, LeftForBoost,
	// the rest of the catalogue (Nov 12)
	TookMateBoost, Aerial, NoFollowUp, Shadow, CutRotation, BallSide,
	// a touch that swung the scoring chance a long way, either way (Nov 13). Moment::detail says which label it earned.
	Play };

// The numbers that have a rank baseline (generated from ranked replays, see tools/match)
enum BaseStat : int {
#include "MatchBaseStats.inc"
	Base_Count };

// What kind of giveaway or clear a moment was (Moment::detail)
namespace MomentDetail
{
	constexpr int GiveRelief = 1;      // ended deep in their half: pressure off, no danger
	constexpr int GiveMidfield = 2;    // straight to an opponent with room
	constexpr int GiveDangerous = 3;   // from your own third
	constexpr int GiveBoomed = 4;      // hit hard up the pitch with no teammate ahead
	constexpr int ClearSide = 1;       // out to the side or the corner
	constexpr int ClearMiddle = 2;     // across the front of your own goal
	// for a Play: the quick-chat label, by how far the touch moved the scoring chance
	constexpr int PlayCalculated = 1;  // the top half a percent of touches
	constexpr int PlayGreat = 2;       // the next few percent: "What a save!", "Nice shot!", "Great pass!"...
	constexpr int PlayWhoops = 3;      // the worst few percent
	constexpr int PlayBlunder = 4;     // the worst half a percent
}

struct Moment
{
	MomentKind kind = MomentKind::Shot;
	int index = 0;                 // into MatchData::frames
	int frame = 0;                 // the replay's frame number (to jump there)
	float time = 0;
	int clock = 0;
	int team = -1;                 // whose moment it is (-1 = nobody's: an even kickoff)
	int player = -1, other = -1;   // who did it, and who else was in it (the pass's receiver, the 50's other car)
	std::string text;              // one line, names included
	std::string brief;             // for a Play: what it did, without the label or the name ("Cut blue's chance from 64% to 2%.")
	int detail = 0;                // the kind of giveaway or clear (MomentDetail); for a beaten challenge, 1 = as the last player back
	float boost = -1;              // the player's boost at that moment, where it matters (50s, beaten challenges)
	float swing = 0;               // for a Play: how far the touch moved their team's chance minus the other team's (-1 to 1)
};

// The review's grades (Nov 13): the rep review's scale, best first. Areas are graded against the lobby's rank.
namespace ReviewGrade
{
	constexpr int Calculated = 0, Clean = 1, Solid = 2, Sloppy = 3, Costly = 4, Throw = 5, Count = 6;
}
enum ReviewArea : int { Area_Attack, Area_Defence, Area_BallUse, Area_Positioning, Area_Boost, Area_Speed, Area_Count };
struct AreaGrade
{
	float score = -1;              // 0..100: the average of where each of its numbers sits among players of the lobby's rank
	int grade = -1;                // ReviewGrade, by where that score sits among those players' scores
	std::string why;               // one line with the numbers behind it
};

// A finding about a player or a team, with its reasons (step 2 of the takeaways plan, Nov 12)
struct Takeaway
{
	int player = -1;               // -1 = about the team
	int team = -1;
	bool good = false;             // a strength, or something to work on
	float weight = 0;              // how much it mattered: bigger shows first
	std::string claim;             // the finding, blunt
	std::string detail;            // the numbers behind it
	std::string tryThis;           // what to try instead (empty for a strength)
	std::vector<int> moments;      // into MatchAnalysis::moments: every instance, to watch
};

// What led to a goal: the defending team's mistakes in the six seconds before it
struct GoalChain
{
	int goalMoment = -1;           // into moments
	int against = -1;              // the team that conceded
	std::vector<int> moments;      // into moments, in time order
	std::string text;              // "balls gave it away in midfield, then foster. was beaten as the last player back"
};

struct PossessionSpan
{
	int from = 0, to = 0;          // indices into frames
	float t0 = 0, t1 = 0;          // the same, as seconds into the replay
	int team = -1;                 // -1 = nobody's (kickoff wait, after a goal)
	int player = -1;
};

struct MatchPlayerStats
{
	int touches = 0, shots = 0, saves = 0, goals = 0, assists = 0, score = 0;
	float onBallSecs = 0;          // time as the last player to touch it
	float avgBoost = 0, noBoostSecs = 0, boostCollected = 0, boostWasted = 0;
	int bigPads = 0, smallPads = 0;
	float supersonicPct = 0, airPct = 0, avgSpeed = 0;
	float defPct = 0, midPct = 0, offPct = 0;      // share of the time in each third, from your own goal
	float behindBallPct = 0;       // goal side of the ball
	float closestPct = 0;          // nearest of your team to the ball
	float lastBackPct = 0;         // furthest back of your team
	int fifties = 0, fiftiesWon = 0, passes = 0, giveaways = 0, clears = 0, demosTaken = 0;
	// the game-sense measurements (Nov 12)
	int giveRelief = 0, giveMidfield = 0, giveDangerous = 0, giveBoomed = 0;
	int clearsMiddle = 0;
	int doubleCommits = 0;         // times they went for a ball a teammate was already on
	int beaten = 0, beatenLast = 0;// challenged and the ball went past them; of those, as the last player back
	int leftNetOpen = 0;           // times they were the last one to leave an open net
	int leftForBoost = 0;          // took a full pad as the last player back while the other team had the ball in their half
	int stolenPads = 0;            // full pads taken in the other team's end
	int lowBoostFifties = 0;       // 50s taken with under 12 boost
	int defensiveActions = 0;      // saves, clears and other touches in their own third
	int kickoffsTaken = 0;
	int goalsAgainstIn = 0;        // goals against with one of their mistakes in the chain
	float aheadSecs = 0;           // in the other team's third while the ball was in their own third
	float stoppedPct = 0;          // nearly still on the ground while the ball was live
	float recoverSecs = 0;         // on average, how long to get goal side again after their team lost the ball
	float firstPct = 0;            // nearest of their team to the ball (the same as closestPct, kept for the rules)
	// the rest of the catalogue (Nov 12)
	float boostUsed = 0;           // all the boost they burned
	float supersonicBoost = 0;     // of that, burned on the ground while already at top speed (it buys nothing)
	int tookMateBoost = 0;         // full pads taken with plenty already, from under a teammate who was low and heading for it
	int aerialsWon = 0, aerialsLost = 0;   // got to a high ball first with an opponent up there too, or was the one beaten to it
	int shadows = 0;               // spells of backing off goal side of the ball carrier instead of diving in
	int cutRotations = 0;          // chased the ball from the wrong side of it while a teammate had it covered
	int returns = 0, returnsBallSide = 0;  // times they came back into their own goal area, and how many on the ball's side
	float bunchedPct = 0;          // share of live time within two car lengths of a teammate
	// positioning (Nov 13). Roles each frame: first = nearest the ball, last = nearest their own goal of the rest,
	// second = whoever is left (3v3). Distances in unreal units; depths measured up the pitch from their own goal line.
	float firstSharePct = 0;       // share of the time as first
	float lastSharePct = 0;        // share of the time as last (not counting time as first)
	float goalSideTheirsPct = 0;   // goal side of the ball while the other team had it
	float supportDist = 0;         // as second (2v2: the one not on the ball), how far from the first, with their team on the ball
	float supportBack = 0;         // the same, how far behind the ball (up-pitch distance, negative = ahead of it)
	float lastDefDepth = 0;        // as last, how far up from their own goal line while the other team had the ball
	float lastAttDepth = 0;        // as last, how far up from their own goal line while their team had it in the other half
	float flatPct = 0;             // level with a teammate (within 500 up the pitch) and neither on the ball: no depth
	float wideSupportPct = 0;      // as second with their team attacking: off to the side of the ball (1,200+ across) rather than straight behind
	float farFromPlayPct = 0;      // over 5,000 from the ball (out of the play)
	float posLevel = -1;           // the rank their positioning numbers look like: 0 gold .. 5 supersonic legend (-1 = too short to say)
	// rank baselines and the scoring-chance line (Nov 13)
	float level = -1;              // the rank this game's numbers look like: 0 gold .. 5 supersonic legend (-1 = too short to say)
	float chanceAdded = 0;         // every touch's swing added up: what their touches did for the team's chances
	int playsGood = 0, playsBad = 0;   // touches that earned a label, good and bad
	// the review (Nov 13)
	AreaGrade area[Area_Count];
	float impact = 0;              // chanceAdded minus the average player's in this game: what the review shows as Impact
	float impactPct = -1;          // where impact sits among players of the lobby's rank (0..100)
	float overallScore = -1;       // the areas and impact together, 0..100
	int overall = -1;              // ReviewGrade for the whole game
	int touchGrades[ReviewGrade::Count] = { 0, 0, 0, 0, 0, 0 };   // each judged touch on the same scale, by its swing
	std::string summary;           // two sentences: the best of the game and the leak
};

struct MatchAnalysis
{
	bool ok = false;
	std::string error;
	std::string replayName, replayFile, recordedBy;
	int teamSize = 0;
	float seconds = 0;             // of play covered
	int score[2] = { 0, 0 };
	std::vector<MatchPlayer> players;
	std::vector<MatchPlayerStats> stats;        // by player id
	std::vector<Moment> moments;                // in time order
	std::vector<PossessionSpan> possession;
	std::vector<char> live;                     // per frame: 1 while the ball is in play
	std::vector<signed char> possTeam;          // per frame: the team that touched it last, -1 nobody's
	float possessionPct[2] = { 0, 0 };
	float territoryPct[2] = { 0, 0 };           // share of live time the ball was in the OTHER team's half
	int kickoffs = 0, kickoffsWon[2] = { 0, 0 };
	int doubleCommits[2] = { 0, 0 };
	float openNetSecs[2] = { 0, 0 };
	int openNets[2] = { 0, 0 };
	int counterGoals[2] = { 0, 0 };             // goals scored within six seconds of winning the ball
	std::vector<std::vector<int>> passTo;       // [from][to]: who found whom
	int shotsStayed[2] = { 0, 0 };              // shots that didn't go in and left the ball in the other team's end
	int shotsNoFollow[2] = { 0, 0 };            // of those, with no teammate close enough for what came off it
	float bunchedPct[2] = { 0, 0 };             // share of live time two teammates were within two car lengths
	// rank baselines and the scoring-chance line (Nov 13)
	float lobbyLevel = -1;                      // the rank this lobby's numbers look like (0 gold .. 5 supersonic legend)
	std::vector<float> chance[2];               // per frame: the chance each team scores in the next ten seconds (0 to 1)
	std::vector<float> frameTime;               // per frame: seconds into the replay (to draw the chance line)
	int bigChances[2] = { 0, 0 };               // spells where a team's chance went over 35%
	std::vector<float> touchSwings;             // every judged touch's swing (for setting the label cut-offs)
	std::vector<int> touchSwingPlayer;          // whose touch each of those was
	std::vector<Takeaway> takeaways;            // strongest first
	std::vector<GoalChain> chains;              // one per goal that had a chain
	// for the timeline: one sample every half second
	std::vector<float> sampleTime;              // seconds into the replay
	std::vector<int> sampleFrame;               // the replay frame
	std::vector<std::vector<float>> boost;      // [player][sample], -1 = not on the pitch
	float t0 = 0, t1 = 0;                       // the replay time covered
	int frame0 = 0, frame1 = 0;
};

namespace Match
{
	bool Load(const std::filesystem::path& file, MatchData& out, std::string& err);
	MatchAnalysis Analyze(const MatchData& d);
	const char* KindName(MomentKind k);

	// The scoring-chance model's view of one moment, from one team's side (always attacking up the pitch, ball on the
	// right): where the ball is and where it's going, who is nearest for each team, who is covering the goal.
	// `possTeam` is the team that touched it last (-1 nobody's). The same numbers train the model (tools/match) and
	// feed it in the plugin, so they can't drift apart.
	constexpr int kChanceFeatures = 21;
	// `skip`: leave that player out, as if they weren't on the pitch
	bool ChanceFeatures(const MatchData& d, int index, int team, int possTeam, float out[kChanceFeatures], int skip = -1);
	float Chance(const float features[kChanceFeatures]);   // 0 to 1: that team scores within ten seconds

	// Rank baselines: what's typical at each rank for each number, from ranked replays (gold to supersonic legend).
	const char* LevelName(int level);                                   // 0 "Gold" .. 5 "Supersonic Legend"
	float StatValue(const MatchPlayerStats& s, int stat, float seconds); // counts come back per five minutes of play
	const float* Baseline(int teamSize, int level, int stat);           // five numbers: the 10th, 25th, 50th, 75th, 90th percentile
	int StatBetter(int stat);                                           // +1 higher is better, -1 lower is better, 0 neither
	const char* GradeName(int grade);                                   // "Calculated" .. "Throw"
	const char* GradeMark(int grade);                                   // "!!", "!", "" (Solid is a tick, drawn), "?!", "?", "??"
	const char* AreaName(int area);                                     // "Attack" .. "Speed"
	float LevelOf(int teamSize, int stat, float raw);                   // a fitted level (Base_level, Base_posLevel) as a rank, 0..5
	int QuarterOf(int teamSize, int level, int stat, float value);      // 0 bottom quarter .. 3 top quarter at that rank (by value, not by better)
	bool StatPerFive(int stat);

	// One player's shot as a rep: the seconds leading up to a touch and what came of it, turned into the same
	// 120-a-second frames a free play rep has, so the rep review can read it. A replay has about 22 frames a second
	// and no stick inputs, so positions are filled in between frames and the jump and boost presses are worked out
	// from what the car did. Orange players are turned around so the shot always goes up the pitch, like a rep.
	struct ShotRep
	{
		std::vector<Frame> frames;
		bool goal = false;
		int eventTick = 0;         // the tick the ball went in, was touched by someone else, or the window ended
		std::string endReason;     // goal, saved, miss
		int stoppedBy = -1;        // saved: the player who got to it
	};
	// frameNumber: the replay frame of the moment (a shot, a goal or a save's shot). False if there's nothing usable.
	bool MakeShotRep(const MatchData& d, int player, int frameNumber, ShotRep& out, std::string& err);
}
