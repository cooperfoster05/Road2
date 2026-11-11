#pragma once
// Learn mode lessons (Oct 26): one mechanic taught from a fixed model rep, step by step. The flow is progressive-part
// practice: step 1, step 2, steps 1-2 together, step 3, steps 1-3 together, ... then the whole thing. Every attempt
// starts from the model's own car and ball at that stage's start (a perfect lead-in), gets a 1-5 score, and the next
// stage unlocks after 3 of your last 5 at full speed score a 4 or 5. Auto ramp runs 50 / 75 / 90 / 100% game speed and
// only moves up after 2 in a row at 4-5. (Plan and the research behind it: design doc, Oct 25.)
// Plain C++, no BakkesMod code.
#include "RepData.h"
#include "ShotBook.h"
#include "ShotGeometry.h"
#include "Segmenter.h"
#include <deque>
#include <string>
#include <vector>

struct LessonStep
{
	std::string name;             // "Pop and jump"
	std::string what;             // one line: what the step is
	std::string why;              // why it matters
	std::vector<std::string> tips;  // best practices
	std::string cue;              // the one thing to focus on
	int start = 0;                // model tick each attempt at this step starts from
};

// A stage of the lesson: one step, or steps first..last together (the last stage is the whole mechanic)
struct LessonStage
{
	int first = 0, last = 0;
	std::string name;
	bool whole = false;
};

struct StageScore
{
	int score = 0;                       // 1-5 (0 = not scored: the attempt was cut short)
	std::vector<int> stepScores;         // per step in the stage
	std::vector<std::string> lines;      // why, in plain words, worst first
	float lineMatch = -1;                // average distance from the model's path (info only; -1 = not measured)
};

// Shot (Nov 2): the foundation lessons graded by the review's own grader (aerial, power shot, dribble...). Move (Nov 2):
// movement with no ball (half flip, wavedash, powerslide turn, speedflip), scored on time and speed.
enum class LessonKind { AirDribble, DoubleTap, CeilingShot, FlipReset, Kickoff, Shot, Move };

// A kickoff in a kickoff lesson's model (Nov 1): the model holds the right spawn's kickoff, then the same one mirrored
// from the left spawn (back centre: one). Model ticks of what happens in each.
struct KickoffSide
{
	int start = -1, end = -1;      // the spawn (time zero) and the last tick of this kickoff
	int jump = -1, dodge = -1;     // the speedflip's jump and its diagonal dodge (-1 for a plain flip-in)
	int land = -1;                 // wheels back down after it
	int flipIn = -1;               // the jump that starts the flip into the ball
	int touch = -1, arrive = -1;   // the hit, and when the car got within 200 of the ball (the openings book's measure)
};

struct Lesson
{
	LessonKind kind = LessonKind::AirDribble;
	std::string id, name;
	std::vector<Frame> model;
	ShotAnalysis modelShot;
	std::vector<LessonStep> steps;
	std::vector<LessonStage> stages;
	// Model timings the stage ends use (model ticks)
	int popTick = -1, firstAirTick = -1, carryEndTick = -1, shotTick = -1;
	int leaveTick = -1, setupTick = -1, boardTick = -1;   // double taps: off the wall, the setup touch, the backboard
	int landTick = -1, flipTick = -1, resetTick = -1;     // ceiling shots: on the ceiling, the flip; flip resets: the reset
	// Kickoffs (Nov 1)
	std::vector<KickoffSide> koSides;
	bool koSpeedflip = false;
	float koStars[3] = { 0, 0, 0 };       // time to the ball for 1, 2 and 3 stars (s)
	std::string koStarsWho;                // who gets there in those times
	std::string koSpawn;                   // "diagonal", "off-centre", "back centre"
	std::vector<std::string> notes;        // more about it, under the steps (kickoffs: variations, what decides them, teammates)
	// Shot lessons (Nov 2)
	ShotType shotType = ShotType::Ground;
	std::vector<SegKind> stepSegs;         // the rep segment each step is graded on
	ShotReference modelRef;                // the model as "your fastest goal on this shot" (the grader's timing benchmark)
	bool power = false;                    // power shot: the whole thing is scored on how hard it went in
	bool needDoubleJump = false;           // fast aerial: a second jump straight after the first
	bool needWallTakeoff = false;          // off the wall: leave from the wall
	bool needFrontFlick = false;           // front flick: the dribble's flick has to be straight forward
	bool needWallTouch = false;            // wall hit: the shot touch on the wall
	bool needBackboard = false;            // backboard read: the ball off the backboard before your first touch
	// Move lessons (Nov 2): what each is measured on (model ticks)
	std::string moveKind;                  // "half_flip", "wavedash", "powerslide", "speedflip"
	int moveStart = -1, moveAct = -1, moveDone = -1;   // the run-up, the move (jump / handbrake), back in control
	int dodgeTick = -1;                    // the flip

	bool valid() const { return !model.empty() && !stages.empty(); }
	int StageStart(int stage) const { return steps[stages[stage].first].start; }
	// The tick (model time) the stage's watch clip ends
	int StageWatchEnd(int stage) const;
	// Kickoff steps that start mid-kickoff: the model drives you from this tick up to the step's start, so you take over
	// already moving (a running start); -1 for none
	int StageRideFrom(int stage) const;
	bool StageRideByInput(int stage) const;   // the running start plays the model's inputs, not its frames (Move: half flip, speedflip)
	// Whether the stage includes the finish (it then ends on a goal or a miss, like any practice attempt)
	bool StageHasFinish(int stage) const { return stages[stage].last == static_cast<int>(steps.size()) - 1; }
};

namespace Lessons
{
	// Builds a lesson from its model rep (data/Road2/lessons/<id>_model.csv). Invalid if the rep isn't that mechanic.
	Lesson AirDribble(const std::vector<Frame>& model);
	Lesson DoubleTap(const std::vector<Frame>& model);
	Lesson CeilingShot(const std::vector<Frame>& model);
	Lesson FlipReset(const std::vector<Frame>& model);
	// Kickoffs (Nov 1): kickoff_flipin, kickoff_diagonal, kickoff_offcentre, kickoff_back
	Lesson Kickoff(const std::string& id, const std::vector<Frame>& model);
	// Foundations (Nov 2): aerial, power_shot, dribble, fast_aerial, off_the_wall (graded like a rep), and the moves
	// half_flip, wavedash, powerslide_turn, speedflip (timed against the model)
	Lesson Shot(const std::string& id, const std::vector<Frame>& model);
	Lesson Move(const std::string& id, const std::vector<Frame>& model);
	Lesson Build(const std::string& id, const std::vector<Frame>& model);

	// During an attempt (full = the model up to the stage start, then what you've done; ticks in model time):
	// true once a stage that ends before the finish is over (popped / first touch / carry time up, or the ball is gone)
	bool StageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick);

	// Scores a finished attempt
	// compareToModel: also check you stayed near the model's line (off only for tests on reps that didn't start there)
	StageScore Score(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst,
		const ShotReference& ref, bool compareToModel = true);
}

// Your progress on one lesson: what's unlocked, recent scores at full speed, and the auto ramp
struct LessonProgress
{
	int unlocked = 0;                         // highest stage you can open
	bool completed = false;                   // passed the last stage (the whole mechanic)
	int testOut = 0;                          // tiers 1-2 (Nov 2): 0 not tried, 1 failed (the steps are open), 2 passed
	std::vector<int> best;                    // best score per stage
	std::vector<std::deque<int>> recentFull;  // last 5 scores at 100% per stage
	int rampStreak = 0;                       // 4-5s in a row at the current auto-ramp speed

	static constexpr float kRamp[4] = { 0.50f, 0.75f, 0.90f, 1.00f };
	static constexpr int kUnlockOf = 5, kUnlockNeed = 3, kRampNeed = 2;

	void Fit(int stages);
	int PassedRecent(int stage) const;        // 4-5s among the last 5 at full speed

	// Records an attempt. Returns what changed: speed (the new auto-ramp speed, or the same), and whether the next
	// stage just unlocked.
	struct Change { float speed = 1.0f; bool rampedUp = false; bool unlockedNext = false; bool completed = false; };
	Change Record(int stage, int score, float speed, bool autoRamp);

	std::string Save(const std::string& lessonId) const;   // csv lines
	void Load(const std::string& lessonId, const std::string& csv);
};

// The skill tree (Oct 28): every lesson, what it needs first, and whether it's built yet (it has a model rep and
// scoring). Lessons whose prerequisites aren't complete are locked, unless you turn the locks off.
struct LessonInfo
{
	std::string id, name, blurb;
	std::vector<std::string> needs;   // lesson ids to complete first
	bool built = false;               // false = coming soon (no model or scoring yet)
	int tier = 0;                     // the skill tree's tier, 1-6 (0: kickoffs, their own group)
	bool TestOut() const { return tier == 1 || tier == 2; }   // can be passed whole, skipping the steps
};

namespace Lessons
{
	const std::vector<LessonInfo>& Catalog();
	const LessonInfo* Find(const std::string& id);
}
