#include "pch.h"
#include "Grader.h"
#include "Units.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
	// ---- Cutoffs for the overall 1-5, one set per shot type ----
	struct Cutoffs
	{
		float closeMiss;    // missed by this much or less = "Close" (2)
		float upperZ;       // ball center height at the goal line; above = upper half of the net
		float fast;         // shot speed for a "fast" shot
		float efficient;    // touch within this many seconds of your fastest goal on the shot
		float aimPoint;     // aim error (degrees) that still earns the aim point toward a 4
		// A 5 = everything a 4 has, and elite at something
		float eliteAim, eliteTiming, cornerDist, rocket;
	};
	// Aerials: tuned on Coop's 361 rated reps (Oct 9), 82% exact match
	constexpr Cutoffs kAerial{ 150.0f, 350.0f, 2800.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	// Ground shots: only closeMiss is used; goals are graded by height in the net (below)
	constexpr Cutoffs kGround{ 180.0f, 300.0f, 2200.0f, 0.25f, 3.5f, 1.6f, 0.10f, 200.0f, 2800.0f };

	// Ground shot goals, tuned on Coop's 45 rated ground goals (Oct 9): how high the ball crossed the line is
	// what separated his 3s, 4s and 5s. Speed and aim barely mattered (almost every goal was fast and on line).
	constexpr float kGroundGoodZ = 360.0f;     // ball center at the goal line: below = low shot (3)
	constexpr float kGroundGreatZ = 500.0f;    // at or above = top shelf (5)
	constexpr float kGroundSlow = 2000.0f;     // slower than this = a 3 whatever the height (not in the data yet)
	// Double taps: only closeMiss is used (goals are graded by height, below). Tuned on Oct 9 double-tap reps:
	// a miss within 245 was a 2; height in the net split 3s (all under 250) from 4s (all over 410).
	constexpr Cutoffs kDoubleTap{ 245.0f, 350.0f, 2800.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	// Dribbles: only closeMiss is used (goals are graded by the flick, below). Tuned on Coop's 163 rated free play
	// dribbles (Oct 10), 88% exact: a miss within 140 was a 2 (almost all of them just over the bar).
	constexpr Cutoffs kDribble{ 140.0f, 350.0f, 2400.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	// Air dribbles: closeMiss, and upperZ = high enough in the net for a 4 (details below). 480 -> 460 on Oct 24:
	// matched Coop's ratings on 3 more of his 88 rated air dribble goals
	constexpr Cutoffs kAirDribble{ 150.0f, 460.0f, 2400.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	// Flip resets, tuned on Coop's 117 rated flip resets (Oct 11): closeMiss, upperZ for a 4, and fast = the speed
	// a 4 needs. Most of his goals were 3s; height in the net (and not dribbling it in slowly) made the 4s.
	constexpr Cutoffs kFlipReset{ 250.0f, 450.0f, 1700.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	constexpr float kFrGreatZ = 560.0f, kFrGreatSpeed = 2400.0f;   // a 5: top of the net and hard
	constexpr float kFrClosingClean = 150.0f;  // reset: moving into the ball at least this fast = Clean
	constexpr float kFrLostBall = 700.0f;      // after the reset the ball got farther than this from your car = lost it
	// Ceiling shots (Oct 24, first pass from the coaching research; no rated ceiling shots yet, so tune once there are):
	// closeMiss, and upperZ = the top half of the net for the 5
	constexpr Cutoffs kCeilingShot{ 250.0f, 360.0f, 2000.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	constexpr float kCsApexClean = 1700.0f;     // setup: the ball peaked at least this high (without touching the ceiling)
	constexpr float kCsApexSolid = 1400.0f;
	constexpr float kCsWallSpeedClean = 1400.0f, kCsWallSpeedSolid = 900.0f;   // your speed into the wall touch (coaches: full speed)
	constexpr float kCsLeaveClean = 250.0f, kCsLeaveSolid = 500.0f;           // met the ball this close to the side of your line off the ceiling
	constexpr float kCsFivePlay = 3.0f;         // 5: wall touch to shot this quick...
	constexpr float kCsFiveCornerX = 580.0f;    // ...and top half of the net or this far toward a corner
	const Cutoffs& For(ShotType t)
	{
		return t == ShotType::Ground ? kGround : t == ShotType::DoubleTap ? kDoubleTap : t == ShotType::Dribble ? kDribble
			: t == ShotType::AirDribble ? kAirDribble : t == ShotType::FlipReset ? kFlipReset : t == ShotType::CeilingShot ? kCeilingShot : kAerial;
	}

	// Dribble goals: the flick's lift (how fast it sent the ball up off your roof) and its speed split 3s from 4s.
	// A 5 = a 4 that's also hard and headed for a corner.
	constexpr float kFlickLift = 520.0f;       // upward speed off the flick for a "real" flick
	constexpr float kFlickSpeed = 2400.0f;     // ball speed off the flick
	constexpr float kFlickRocket = 2800.0f;    // 5: this hard...
	constexpr float kFlickCornerX = 550.0f;    // ...and this far from the middle of the goal at the line
	// Coop: a dribble you're barely moving on shouldn't score well however good the flick is.
	// (None of the first 163 were this slow; his slowest still averaged 3s.)
	constexpr float kDribbleSlowPace = 900.0f;
	// Air dribbles, tuned on Coop's 113 rated air dribbles (Oct 10), 81% exact. Goals: high in the net with a
	// real chain of touches = 4; a 5 is also headed for a corner off soft, carried touches (his "glued to the car").
	// Misses: close (within 150), or a long carry (5+ touches or 4.5 s) before losing it = 2.
	constexpr int   kAdChainFor4 = 3;          // touches in the air, the final one included
	constexpr float kAdCornerX = 580.0f;       // 5: this far from the middle of the goal at the line...
	constexpr float kAdSoftPush = 500.0f;      // ...off touches averaging this much push or less (carried, not hit)
	constexpr int   kAdLongTouches = 5;
	constexpr float kAdPopClean = 950.0f;      // setup: popped off the wall at or below this (about 46% up)
	constexpr float kAdPopLate = 1230.0f;      // past this (60% up) you've fallen behind the ball
	constexpr float kAdLongSec = 4.5f;
	Grade FromAirControl(int touches, float push)
	{
		if (touches >= 5 && push <= 450.0f) return Grade::Clean;
		if (touches >= 3 && push <= 750.0f) return Grade::Solid;
		return Grade::Sloppy;
	}

	// "45 flick (left)" etc.
	std::string FlickWords(const ShotAnalysis& s)
	{
		if (s.flipTick < 0) return "flick";
		std::string name = ShotGeometry::FlipName(s.flipKind, true);
		bool sided = s.flipKind == FlipKind::DiagonalFront || s.flipKind == FlipKind::Side || s.flipKind == FlipKind::DiagonalBack;
		return sided ? name + (s.flipRight ? " (right)" : " (left)") : name;
	}
	std::string Capital(std::string s)
	{
		if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
		return s;
	}

	Grade FromDribblePace(float pace)
	{
		if (pace >= 1400.0f) return Grade::Clean;
		if (pace >= 1000.0f) return Grade::Solid;
		if (pace >= 800.0f) return Grade::Sloppy;
		return Grade::Costly;
	}

	constexpr float kDtGoodZ = 360.0f;      // double tap goal at/above this height at the line = a 4
	constexpr float kDtReadFast = 2600.0f;  // "hard" in the description
	// A 5 (Coop, Oct 24): a 4 that's a quick play (DoubleTapPace), hit at least ~2.5 car lengths off the wall (a
	// tap on the crossbar can be a 4, not a 5). A floor bounce in from the side still caps it at a 3; one coming
	// down from above the goal doesn't count against it.
	constexpr float kDtFivePace = 0.65f, kDtFiveOffWall = 300.0f, kDtSideAngle = 35.0f;

	// Double tap setup (Oct 24, from the coaching research and Coop's rated double taps): a high spot on the
	// backboard, not past the middle toward the far side when you came from a side, and not so wide it's outside
	// the goal (his setups past ~1,800 from the middle were all 1s). Hit too hard (fast into the board and it came
	// 1,500+ off the wall before you got there) = Sloppy: coaches warn the rebound runs away from you.
	constexpr float kDtSideCarX = 800.0f;      // the setup touch this far from the middle = you came from a side
	constexpr float kDtFarSideClean = 600.0f;  // a Clean spot can be at most this far past the middle, toward the far side
	constexpr float kDtTooHardSpeed = 2600.0f, kDtTooHardRebound = 1500.0f;
	Grade FromBackboardSpot(const ShotAnalysis& shot, std::string& note)
	{
		const float z = shot.backboardZ, ax = std::fabs(shot.backboardX);
		const bool fromSide = std::fabs(shot.setupCarX) >= kDtSideCarX;
		const float nearX = fromSide ? shot.backboardX * (shot.setupCarX > 0.0f ? 1.0f : -1.0f) : ax;   // - = far side
		if (shot.intoBoardSpeed >= kDtTooHardSpeed && shot.reboundDist >= kDtTooHardRebound)
		{
			note = std::format(" (hit it too hard: it came {:.0f} off the wall before you got there)", shot.reboundDist);
			return Grade::Sloppy;
		}
		if (ax > 1800.0f) { note = " (too wide: outside the goal)"; return z >= 300.0f && z <= 1700.0f && ax <= 2000.0f ? Grade::Sloppy : Grade::Costly; }
		if (fromSide && nearX < -1200.0f) { note = " (crossed way over to the far side)"; return Grade::Sloppy; }
		if (z >= 500.0f && z <= 1300.0f && ax <= 1600.0f && nearX >= -kDtFarSideClean) return Grade::Clean;
		if (fromSide && nearX < -kDtFarSideClean) note = " (past the middle, toward the far side)";
		else if (z < 500.0f) note = " (low on the board)";
		else if (z > 1300.0f) note = " (high on the board)";
		if (z >= 300.0f && z <= 1700.0f) return Grade::Solid;
		return Grade::Sloppy;
	}
	Grade FromReadTime(float sec)
	{
		if (sec <= 0.35f) return Grade::Clean;
		if (sec <= 0.70f) return Grade::Solid;
		if (sec <= 1.10f) return Grade::Sloppy;
		return Grade::Costly;
	}

	// Ground shots: how well your driving line lined up with the push the ball needed
	Grade FromApproachAngle(float deg)
	{
		if (deg <= 8.0f) return Grade::Clean;
		if (deg <= 18.0f) return Grade::Solid;
		if (deg <= 30.0f) return Grade::Sloppy;
		return Grade::Costly;
	}
	Grade Worse(Grade a, Grade b) { return static_cast<int>(a) >= static_cast<int>(b) ? a : b; }

	Grade FromAim(float deg)
	{
		if (deg <= 2.5f) return Grade::Clean;
		if (deg <= 5.0f) return Grade::Solid;
		if (deg <= 10.0f) return Grade::Sloppy;
		return Grade::Costly;
	}

	Grade FromDelay(float sec)
	{
		float a = std::fabs(sec);
		if (a <= 0.10f) return Grade::Clean;
		if (a <= 0.25f) return Grade::Solid;
		if (a <= 0.50f) return Grade::Sloppy;
		return Grade::Costly;
	}

	bool IsMistake(Grade g) { return g == Grade::Sloppy || g == Grade::Costly || g == Grade::OpenNet || g == Grade::Throw; }

	std::string Side(float v, const char* pos, const char* neg) { return v >= 0 ? pos : neg; }
}

const char* Grader::Name(Grade g)
{
	switch (g)
	{
	case Grade::Calculated: return "Calculated";
	case Grade::Clean:      return "Clean";
	case Grade::Solid:      return "Solid";
	case Grade::Sloppy:     return "Sloppy";
	case Grade::Costly:     return "Costly";
	case Grade::OpenNet:    return "Open net";
	case Grade::Throw:      return "Throw";
	default:                return "";
	}
}

const char* Grader::OverallName(int o)
{
	switch (o)
	{
	case 1: return "Missed";
	case 2: return "Close";
	case 3: return "Bad shot, went in";
	case 4: return "Good shot, went in";
	case 5: return "Great shot, went in";
	default: return "";
	}
}

RepGrade Grader::GradeRep(const std::vector<Frame>& fr, const RepSegments& segs, const ShotAnalysis& shot,
	bool wentIn, bool bouncedIn, const ShotReference& ref)
{
	RepGrade g;
	auto gradeSeg = [&](int idx, Grade grade, std::string why) {
		if (idx < 0) return;
		g.segs.push_back(SegmentGrade{ idx, grade, std::move(why) });
	};
	auto findSeg = [&](SegKind kind, int touch) {
		int found = -1;
		for (int i = 0; i < static_cast<int>(segs.segs.size()); i++)
			if (segs.segs[i].kind == kind && (touch == 0 || segs.segs[i].touch == touch)) found = i;
		return found;
	};

	// ---- Whiff ----
	if (segs.whiff || !shot.valid)
	{
		g.overall = 1;
		g.overallWhy = "No touch on the ball";
		int last = -1;
		for (int i = 0; i < static_cast<int>(segs.segs.size()); i++) if (segs.segs[i].kind != SegKind::Recovery) last = i;
		gradeSeg(last, Grade::Throw, "Never touched the ball");
		g.biggestFix = g.segs.empty() ? -1 : 0;
		return g;
	}

	const int shotTouch = static_cast<int>(segs.touchTicks.size());  // segments are numbered by touch, 1-based
	// Bouncing in is a flaw for aerials; for ground shots the ball being on the floor is normal
	bool bounced = (shot.type == ShotType::Aerial || shot.type == ShotType::CeilingShot) && (bouncedIn || (wentIn && shot.dipsToFloor));
	float myTouchSec = (shot.touchTick - shot.startTick) / 120.0f;
	float vsBest = ref.valid ? myTouchSec - ref.touchSec : 0.0f;

	// ---- Takeoff and flight: timing against your fastest goal on this shot ----
	int takeoffIdx = findSeg(SegKind::Takeoff, shotTouch);
	int flightIdx = findSeg(SegKind::Flight, shotTouch);
	if (ref.valid && ref.bestFile.empty() == false)
	{
		float myJumpSec = takeoffIdx >= 0 ? (segs.segs[takeoffIdx].start - shot.startTick) / 120.0f : -1.0f;
		if (takeoffIdx >= 0 && ref.jumpSec >= 0)
		{
			float jumpDiff = myJumpSec - ref.jumpSec;
			std::string why = std::fabs(jumpDiff) <= 0.10f
				? std::format("Jumped on time ({:+.2f}s vs your fastest goal on this shot)", jumpDiff)
				: std::format("Jumped {:.2f}s {} than on your fastest goal on this shot", std::fabs(jumpDiff), jumpDiff > 0 ? "later" : "earlier");
			if (vsBest < 0.0f)
				gradeSeg(takeoffIdx, Grade::Clean, std::format("Fastest touch yet on this shot ({:.2f}s quicker than your old best)", -vsBest));
			else
				gradeSeg(takeoffIdx, FromDelay(jumpDiff), why);

			if (flightIdx >= 0)
			{
				float flightDiff = (myTouchSec - myJumpSec) - (ref.touchSec - ref.jumpSec);
				std::string fwhy = std::fabs(flightDiff) <= 0.10f
					? "Got to the ball as fast as your best attempt once airborne"
					: std::format("Took {:.2f}s {} in the air than your fastest goal on this shot{}", std::fabs(flightDiff),
						flightDiff > 0 ? "longer" : "less", flightDiff > 0.25f ? " (waiting on the ball)" : "");
				gradeSeg(flightIdx, flightDiff < 0 ? Grade::Clean : FromDelay(flightDiff), fwhy);
			}
		}
		else if (flightIdx >= 0)
		{
			gradeSeg(flightIdx, vsBest < 0 ? Grade::Clean : FromDelay(vsBest),
				std::format("Touched {:.2f}s {} than your fastest goal on this shot", std::fabs(vsBest), vsBest >= 0 ? "later" : "earlier"));
		}
	}

	// ---- Approach (ground shots): your driving line into the ball, and timing ----
	if (shot.type == ShotType::Ground)
	{
		int approachIdx = findSeg(SegKind::Approach, shotTouch);
		Grade lineGrade = FromApproachAngle(shot.approachAngleDeg);
		std::string why = std::format("Drove in {:.0f} deg off the line the ball needed, at {}", shot.approachAngleDeg, Units::Speed(shot.carSpeed));
		Grade grade = lineGrade;
		if (ref.valid && !ref.bestFile.empty() && takeoffIdx < 0)
		{
			Grade timeGrade = vsBest < 0.0f ? Grade::Clean : FromDelay(vsBest);
			if (static_cast<int>(timeGrade) > static_cast<int>(lineGrade))
				why = std::format("Got there {:.2f}s later than your fastest goal on this shot ({:.0f} deg off the ideal line)", vsBest, shot.approachAngleDeg);
			grade = Worse(lineGrade, timeGrade);
		}
		gradeSeg(approachIdx, grade, why);
	}

	// ---- Double tap: the setup touch into the backboard, and the read off it ----
	if (shot.type == ShotType::DoubleTap)
	{
		int setupIdx = findSeg(SegKind::Setup, 0);
		const char* surface = shot.tapSurface == 2 ? "the ceiling" : shot.tapSurface == 3 ? "the top of the wall" : "the backboard";
		float ax = std::fabs(shot.backboardX);
		if (shot.tapSurface == 1)
		{
			std::string note;
			Grade sg = FromBackboardSpot(shot, note);
			const bool fromSide = std::fabs(shot.setupCarX) >= kDtSideCarX;
			const char* side = !fromSide || ax < 150.0f ? "from the middle"
				: (shot.backboardX > 0.0f) == (shot.setupCarX > 0.0f) ? "toward your side" : "toward the far side";
			gradeSeg(setupIdx, sg, std::format("Sent it to the backboard {:.0f} high, {:.0f} {}{}", shot.backboardZ, ax, side, note));
		}
		else
			gradeSeg(setupIdx, Grade::Solid, std::format("Sent it off {} ({:.0f} high)", surface, shot.backboardZ));
		int readIdx = findSeg(SegKind::Read, shotTouch);
		float readSec = (shot.touchTick - shot.backboardTick) / 120.0f;
		gradeSeg(readIdx, FromReadTime(readSec), std::format("Hit it {:.2f}s after it came off {} (you were {:.0f} from where you met it when it bounced)",
			readSec, surface, shot.meetDist));
	}

	// ---- Dribble: the carry, then the flick ----
	if (shot.type == ShotType::Dribble)
	{
		gradeSeg(findSeg(SegKind::Dribble, 0), FromDribblePace(shot.dribblePace),
			std::format("Carried it {:.1f}s at {} on average", shot.dribbleSec, Units::Speed(shot.dribblePace)));
		int flickIdx = findSeg(SegKind::Flick, 0);
		if (shot.flickJumpTick < 0)
			gradeSeg(flickIdx, Grade::Sloppy, "No flick: the ball came off your car without a jump");
		else if (!wentIn)
		{
			std::string why = std::format("{} left your car {:.1f} deg off", Capital(FlickWords(shot)), shot.aimErrDeg);
			if (!shot.missDir.empty()) why += std::format(" (went {})", shot.missDir);
			gradeSeg(flickIdx, FromAim(shot.aimErrDeg), why);
		}
		else
		{
			bool lifted = shot.flickLift >= kFlickLift, hard = shot.shotSpeed >= kFlickSpeed;
			Grade fg = lifted && hard ? (shot.shotSpeed >= kFlickRocket ? Grade::Clean : Grade::Solid) : Grade::Sloppy;
			gradeSeg(flickIdx, fg, std::format("{} sent it up at {} and off at {}{}", Capital(FlickWords(shot)), Units::Speed(shot.flickLift),
				Units::Speed(shot.shotSpeed), !lifted ? " (not much lift)" : !hard ? " (not much power)" : ""));
		}
	}

	// ---- Air dribble: how well you carried it ----
	if (shot.type == ShotType::AirDribble)
	{
		// The setup (Oct 24, coaching research): pop it off the wall low (roughly a third to under half way up;
		// past 60% coaches say take the ceiling shot instead) and jump right after the pop so you don't fall behind.
		// Off the ground: hit it and jump together.
		if (shot.popTick >= 0)
		{
			const float wallFrac = shot.popZ / 2044.0f;
			Grade hg = !shot.popOnWall || shot.popZ <= kAdPopClean ? Grade::Clean : shot.popZ <= kAdPopLate ? Grade::Solid : Grade::Sloppy;
			const float jClean = 0.15f, jSolid = shot.popOnWall ? 0.25f : 0.30f;
			Grade jg = shot.popJumpSec <= jClean ? Grade::Clean : shot.popJumpSec <= jSolid ? Grade::Solid : Grade::Sloppy;
			Grade pg = Worse(hg, jg);
			std::string why = shot.popOnWall
				? std::format("Popped it off the wall {:.0f} high ({:.0f}% up), left the wall {:.2f}s later", shot.popZ, wallFrac * 100.0f, shot.popJumpSec)
				: std::format("Popped it off the ground, jumped {:.2f}s after the touch", shot.popJumpSec);
			if (hg == Grade::Sloppy) why += " (too high up the wall: you're behind it, a ceiling shot fits better)";
			else if (jg == Grade::Sloppy) why += " (jumped late: jump right after the pop)";
			gradeSeg(findSeg(SegKind::Approach, 0), pg, why);
		}
		int before = shot.airTouches - 1;
		std::string why = std::format("{} touch{} in the air before the shot, {} of push each on average, ball close to your car {:.0f}% of the time",
			before, before == 1 ? "" : "es", Units::Speed(shot.airImpulse), shot.airCloseFrac * 100.0f);
		Grade cg = FromAirControl(shot.airTouches, shot.airImpulse);
		if (cg == Grade::Sloppy)
			why += shot.airTouches < kAdChainFor4 ? " (barely carried it)" : " (hitting it more than carrying it)";
		gradeSeg(findSeg(SegKind::AirDribble, 0), cg, why);
	}

	// ---- Flip reset: getting it, then using it ----
	if (shot.type == ShotType::FlipReset)
	{
		float toReset = (shot.resetTick - shot.resetTakeoff) / 120.0f;
		// The reset (Oct 24, coaching research): fly into the ball, don't let it fall onto you. When in its arc you
		// get it isn't graded: guides disagree, and Coop's best resets came off a falling ball.
		Grade rg = shot.resetClosing >= kFrClosingClean ? Grade::Clean : shot.resetClosing >= 0.0f ? Grade::Solid : Grade::Sloppy;
		gradeSeg(findSeg(SegKind::Reset, 0), rg,
			std::format("Got the reset {:.2f}s after leaving the {}, moving into the ball at {}{}{}", toReset,
				shot.resetAfterAirDribble ? "wall or ground, off an air dribble" : "wall or ground", Units::Speed(std::max(0.0f, shot.resetClosing)),
				shot.resetClosing < 0.0f ? " (it fell onto you: fly into it)" : shot.resetClosing < kFrClosingClean ? " (barely: fly into it more)" : "",
				shot.resets > 1 ? std::format(" ({} resets this play)", shot.resets) : std::string()));
		float held = shot.resetDodge >= 0 ? (shot.resetDodge - shot.resetTick) / 120.0f : 0.0f;
		Grade fg = shot.flipShot ? Grade::Clean : shot.resetDodge >= 0 ? Grade::Sloppy : Grade::Costly;
		std::string flipName = ShotGeometry::FlipName(shot.flipKind, false);
		std::string why = shot.flipShot ? std::format("Held the flip {:.2f}s, then hit the shot with a {}", held, flipName)
			: shot.resetDodge >= 0 ? std::format("Used a {} {:.2f}s after the reset, but not into the shot", flipName, held)
			: "Never used the flip you got back";
		why += shot.resetKeptMax > kFrLostBall ? std::format(" (the ball got {:.0f} away from you after the reset)", shot.resetKeptMax)
			: std::format(" (kept the ball within {:.0f} after the reset)", shot.resetKeptMax);
		gradeSeg(findSeg(SegKind::Flip, 0), fg, why);
	}

	// ---- Ceiling shot: the wall touch, the ceiling, the drop and the flip ----
	if (shot.type == ShotType::CeilingShot)
	{
		if (shot.ceilWallTouch >= 0)
		{
			Grade pathG = shot.ballHitCeiling ? Grade::Sloppy : shot.ballApexZ >= kCsApexClean ? Grade::Clean : shot.ballApexZ >= kCsApexSolid ? Grade::Solid : Grade::Sloppy;
			Grade speedG = shot.wallTouchCarSpeed >= kCsWallSpeedClean ? Grade::Clean : shot.wallTouchCarSpeed >= kCsWallSpeedSolid ? Grade::Solid : Grade::Sloppy;
			std::string why = std::format("Hit it up the wall at {}; it peaked {:.0f} high", Units::Speed(shot.wallTouchCarSpeed), shot.ballApexZ);
			if (shot.ballHitCeiling) why += " (it hit the ceiling and dropped back: keep it just under)";
			else if (pathG == Grade::Sloppy) why += " (not near the ceiling: hit it harder up the wall)";
			else if (speedG == Grade::Sloppy) why += " (come in faster: the wall touch sets the shot's speed)";
			gradeSeg(findSeg(SegKind::Approach, 0), Worse(pathG, speedG), why);
		}
		Grade leaveG = shot.leaveOffset <= kCsLeaveClean ? Grade::Clean : shot.leaveOffset <= kCsLeaveSolid ? Grade::Solid : Grade::Sloppy;
		gradeSeg(findSeg(SegKind::Ceiling, 0), leaveG, std::format("{:.2f}s on the ceiling; met the ball {:.0f} to the side of the line you left it on{}",
			shot.ceilSec, shot.leaveOffset, leaveG == Grade::Sloppy ? " (line up with the ball while you're up there)" : ""));
		Grade jumpG = !shot.ceilJumped ? Grade::Clean : shot.ballHitCeiling ? Grade::Solid : Grade::Sloppy;
		Grade flipG = shot.ceilFlipShot ? Grade::Clean : shot.ceilFlip >= 0 ? Grade::Sloppy : Grade::Costly;
		std::string flipName = shot.ceilFlip >= 0 ? ShotGeometry::FlipName(shot.flipKind, false) : std::string();
		std::string why = !shot.ceilJumped ? "Fell off the ceiling (flip kept)"
			: shot.ballHitCeiling ? "Jumped down off the ceiling (fine here: the ball was coming off the ceiling)"
			: "Jumped off the ceiling (fall off instead, so you keep your flip)";
		why += shot.ceilFlipShot ? std::format(", then shot with a {} {:.2f}s later", flipName, shot.dropSec)
			: shot.ceilFlip >= 0 ? std::format(", used a {} but not into the shot", flipName)
			: ", never used the flip";
		gradeSeg(findSeg(SegKind::Drop, 0), Worse(jumpG, flipG), why);
	}

	// ---- Contact: aim error, explained by where you hit the ball ----
	int contactIdx = findSeg(SegKind::Contact, shotTouch);
	// Pushed in from right at the goal line: the angle math means nothing that close, and it went in
	float toLine = 5120.0f - shot.ballCenter.y * shot.goalSign;
	if (wentIn && toLine < 500.0f)
		gradeSeg(contactIdx, Grade::Clean, std::format("Finished it from {:.0f} out", std::max(0.0f, toLine)));
	else
	{
		std::string why = std::format("Shot left your car {:.1f} deg off", shot.aimErrDeg);
		if (shot.contactErr > 15.0f)
			why += std::format(": hit {:.0f} {} and {:.0f} {} of the ideal spot", std::fabs(shot.contactUp),
				Side(shot.contactUp, "high", "low"), std::fabs(shot.contactLeft), Side(shot.contactLeft, "left", "right"));
		gradeSeg(contactIdx, FromAim(shot.aimErrDeg), why);
	}

	// ---- Result ----
	int resultIdx = findSeg(SegKind::Result, 0);
	const Cutoffs& C = For(shot.type);
	bool upper = shot.reachesGoalLine && shot.crossZ >= C.upperZ;
	bool fast = shot.shotSpeed >= C.fast;
	if (!wentIn)
	{
		bool close = shot.reachesGoalLine && shot.missDist <= C.closeMiss
			&& ((shot.type != ShotType::DoubleTap && shot.type != ShotType::AirDribble && shot.type != ShotType::FlipReset) || shot.missDist > 0.0f);
		// Air dribbles: carrying it a long way before losing it still counts as close
		bool longCarry = shot.type == ShotType::AirDribble && (shot.airTouches >= kAdLongTouches || shot.airSec >= kAdLongSec);
		close = close || longCarry;
		std::string why = !shot.reachesGoalLine ? "Never reached the goal line"
			: shot.missDist <= 0 ? "Looked on target but didn't go in"
			: std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist);
		// In a double-tap drill: set up the backboard but never got the second touch
		if (ref.doubleTapDrill && shot.type != ShotType::DoubleTap
			&& ShotGeometry::LastWallTap(fr, shot.touchTick + 1, static_cast<int>(fr.size()) - 2) >= 0)
			why = "Got it to the backboard but no second touch";
		gradeSeg(resultIdx, close ? Grade::Sloppy : Grade::Costly, why);
		g.overall = close ? 2 : 1;
		g.overallWhy = why;
	}
	else if (shot.type == ShotType::DoubleTap)
	{
		bool high = shot.reachesGoalLine && shot.crossZ >= kDtGoodZ;
		const float pace = DoubleTapPace(shot);
		bool great = high && pace >= kDtFivePace && shot.reboundDist >= kDtFiveOffWall;
		g.overall = high ? (great ? 5 : 4) : 3;
		std::string why = std::format("{} goal, {}, {} ({})", "Double tap",
			high ? "high in the net" : "low in the net", shot.shotSpeed >= kDtReadFast ? "hard" : "soft", Units::Speed(shot.shotSpeed));
		if (great) why += ", a quick play off the wall";
		else if (high && pace >= kDtFivePace) why += ", but hit right off the wall";
		else if (high && shot.reboundDist >= kDtFiveOffWall) why += std::format(", but a slow play ({:.1f}s from leaving the ground)", shot.tapPlaySec);
		gradeSeg(resultIdx, g.overall >= 5 ? Grade::Clean : g.overall == 4 ? Grade::Solid : Grade::Sloppy, why);
		g.overallWhy = why;
	}
	else if (shot.type == ShotType::FlipReset)
	{
		bool high = shot.reachesGoalLine && shot.crossZ >= kFlipReset.upperZ;
		bool fastEnough = shot.shotSpeed >= kFlipReset.fast;
		bool great = high && shot.crossZ >= kFrGreatZ && shot.shotSpeed >= kFrGreatSpeed;
		g.overall = (high && fastEnough) ? (great ? 5 : 4) : 3;
		std::string why = std::format("Flip reset goal, {}, {} ({}){}", high ? "high in the net" : "low in the net",
			fastEnough ? "with pace" : "slow", Units::Speed(shot.shotSpeed), shot.flipShot ? ", shot off the flip" : "");
		gradeSeg(resultIdx, g.overall >= 5 ? Grade::Clean : g.overall == 4 ? Grade::Solid : Grade::Sloppy, why);
		g.overallWhy = why;
	}
	else if (shot.type == ShotType::CeilingShot)
	{
		// 3 = a goal; 4 = off the flip you kept (fell off, didn't jump) with the ball kept off the ceiling;
		// 5 = a 4 that's also quick from the wall touch, hard, and high or toward a corner
		bool clean = shot.ceilFlipShot && !shot.ceilJumped && !shot.ballHitCeiling;
		bool placed = shot.reachesGoalLine && (shot.crossZ >= kCeilingShot.upperZ || std::fabs(shot.crossX) >= kCsFiveCornerX);
		bool quick = shot.ceilWallTouch < 0 || shot.ceilPlaySec <= kCsFivePlay;
		bool great = clean && placed && quick && shot.shotSpeed >= kCeilingShot.fast;
		g.overall = clean ? (great ? 5 : 4) : 3;
		std::string why = std::format("Ceiling shot goal, {} ({})", shot.shotSpeed >= kCeilingShot.fast ? "hard" : "soft", Units::Speed(shot.shotSpeed));
		if (great) why += ", off the flip, quick and well placed";
		else if (clean) why += ", off the flip you kept";
		else if (shot.ceilJumped && !shot.ballHitCeiling) why += " (jumped off the ceiling)";
		else if (shot.ballHitCeiling) why += " (the ball hit the ceiling first)";
		else why += " (not off the flip)";
		gradeSeg(resultIdx, g.overall >= 5 ? Grade::Clean : g.overall == 4 ? Grade::Solid : Grade::Sloppy, why);
		g.overallWhy = why;
	}
	else if (shot.type == ShotType::AirDribble)
	{
		bool high = shot.reachesGoalLine && shot.crossZ >= kAirDribble.upperZ;
		bool chain = shot.airTouches >= kAdChainFor4;
		bool corner = shot.reachesGoalLine && std::fabs(shot.crossX) >= kAdCornerX;
		bool soft = shot.airImpulse <= kAdSoftPush;
		g.overall = (high && chain) ? ((corner && soft) ? 5 : 4) : 3;
		std::string why = std::format("Air dribble goal, {}, {} touch{} in the air", high ? "high in the net" : "low in the net",
			shot.airTouches, shot.airTouches == 1 ? "" : "es");
		if (g.overall == 5) why += ", carried into the corner";
		else if (high && !chain) why += " (not much of a carry)";
		gradeSeg(resultIdx, g.overall >= 5 ? Grade::Clean : g.overall == 4 ? Grade::Solid : Grade::Sloppy, why);
		g.overallWhy = why;
	}
	else if (shot.type == ShotType::Dribble)
	{
		bool lifted = shot.flickLift >= kFlickLift && shot.flickJumpTick >= 0;
		bool hard = shot.shotSpeed >= kFlickSpeed;
		bool corner = shot.reachesGoalLine && std::fabs(shot.crossX) >= kFlickCornerX;
		bool slowDribble = shot.dribblePace < kDribbleSlowPace;
		g.overall = (lifted && hard) ? ((shot.shotSpeed >= kFlickRocket && corner) ? 5 : 4) : 3;
		if (slowDribble) g.overall = std::min(g.overall, 3);
		std::string why = std::format("Goal off a {}, {}, {} ({}){}{}", FlickWords(shot), lifted ? "good lift" : "not much lift",
			hard ? "hard" : "soft", Units::Speed(shot.shotSpeed), g.overall == 5 ? ", toward the corner" : "",
			slowDribble ? ", but a slow dribble" : "");
		gradeSeg(resultIdx, g.overall >= 5 ? Grade::Clean : g.overall == 4 ? Grade::Solid : Grade::Sloppy, why);
		g.overallWhy = why;
	}
	else if (shot.type == ShotType::Ground)
	{
		bool slow = shot.shotSpeed < kGroundSlow;
		bool high = shot.reachesGoalLine && shot.crossZ >= kGroundGoodZ;
		bool topShelf = shot.reachesGoalLine && shot.crossZ >= kGroundGreatZ;
		std::string why = std::format("Goal, {}, {}", topShelf ? "top shelf" : high ? "mid height" : "low in the net",
			std::format("{} ({})", slow ? "slow" : "fast", Units::Speed(shot.shotSpeed)));
		Grade res = slow ? Grade::Sloppy : topShelf ? Grade::Clean : high ? Grade::Solid : Grade::Sloppy;
		gradeSeg(resultIdx, res, why);
		g.overall = slow ? 3 : topShelf ? 5 : high ? 4 : 3;
		g.overallWhy = why;
	}
	else
	{
		std::vector<std::string> pros, cons;
		(upper ? pros : cons).push_back(upper ? "upper half of the net" : "low in the net");
		(fast ? pros : cons).push_back(std::format("{} ({})", fast ? "fast" : "slow", Units::Speed(shot.shotSpeed)));
		if (bounced) cons.push_back("bounced in");

		Grade res = bounced ? Grade::Sloppy : (upper && fast) ? Grade::Clean : (upper || fast) ? Grade::Solid : Grade::Sloppy;
		std::string why = "Goal";
		for (auto& p : pros) why += ", " + p;
		for (auto& c : cons) why += ", " + c;
		gradeSeg(resultIdx, res, why);

		int points = 1 /* not bounced */ + (upper ? 1 : 0) + (fast ? 1 : 0)
			+ ((!ref.valid || vsBest <= C.efficient) ? 1 : 0) + (shot.aimErrDeg <= C.aimPoint ? 1 : 0);
		if (bounced) g.overall = 3;
		else if (points < 4) g.overall = 3;
		else if (points == 5 && shot.aimErrDeg <= C.eliteAim && (!ref.valid || vsBest <= C.eliteTiming)
			&& (shot.cornerDist <= C.cornerDist || shot.shotSpeed >= C.rocket)) g.overall = 5;
		else g.overall = 4;

		g.overallWhy = why;
		if (g.overall == 5) g.overallWhy += shot.cornerDist <= C.cornerDist ? ", top corner" : ", rocket";
	}

	// Bounced in off the floor on the way: where it ended up doesn't count for much (Oct 12: every bounced goal
	// you've rated is a 3, all shot types). Crossbar-downs and post-ins aren't bounces.
	// (Double taps coming down from above the goal can't help a bounce, Coop Oct 24: only a side-angle bounce counts)
	bool bouncedGoal = wentIn && (shot.bouncedIn || bounced) && !(shot.type == ShotType::DoubleTap && shot.tapAngle < kDtSideAngle);
	if (bouncedGoal && g.overall > 3)
	{
		g.overall = 3;
		if (g.overallWhy.find("bounced") == std::string::npos) g.overallWhy += ", bounced in";
		for (SegmentGrade& sg : g.segs)
			if (sg.segIndex == resultIdx && (sg.grade == Grade::Clean || sg.grade == Grade::Solid)) { sg.grade = Grade::Sloppy; sg.why = g.overallWhy; }
	}

	// You marked this shot as a double-tap drill: a goal without the double tap (or triple, quad...) doesn't count
	// (A tap that bounced on the floor before your finish still counts as an attempt at one.)
	bool tappedFirst = segs.touchTicks.size() >= 2
		&& ShotGeometry::LastWallTap(fr, segs.touchTicks.front(), shot.touchTick - 1) >= 0;
	if (wentIn && shot.type != ShotType::DoubleTap && !tappedFirst && ref.doubleTapDrill)
	{
		g.overall = 1;
		g.overallWhy = "Scored without the double tap (you marked this shot as a double-tap drill)";
		for (SegmentGrade& sg : g.segs)
			if (sg.segIndex == resultIdx) { sg.grade = Grade::Costly; sg.why = g.overallWhy; }
	}

	// ---- Recovery: time to get back in control, landing speed, and alignment ----
	int recIdx = findSeg(SegKind::Recovery, 0);
	if (recIdx >= 0)
	{
		if (segs.recoveredTick < 0)
		{
			int afterTouch = static_cast<int>(fr.size()) - 1 - segs.lastTouch;
			gradeSeg(recIdx, afterTouch < 300 ? Grade::None : Grade::Costly,
				afterTouch < 300 ? "Rep ended before you landed" : "Never got back in control");
		}
		else
		{
			// Fitted to Coop's recovery ratings (Oct 11, 131 rated: 56% exact vs 34% for the old time-based formula).
			// What separated his good / eh / poor: speed kept into the landing, landing lined up with where you're
			// going, and using a flip to get moving again. How fast you got "in control" barely mattered.
			const int n = static_cast<int>(fr.size());
			auto speedAt = [&](int i) { const Frame& f = fr[i]; return std::sqrt(f.carVX * f.carVX + f.carVY * f.carVY + f.carVZ * f.carVZ); };
			int land = -1;
			for (int i = segs.lastTouch + 1; i < n; i++) if (ShotGeometry::OnSurface(fr[i])) { land = i; break; }
			if (land < 0) land = segs.recoveredTick;
			const Frame& lf = fr[land];
			float landSpeed = speedAt(land);
			float pitch = lf.carPitch * 3.14159265f / 32768.0f, yaw = lf.carYaw * 3.14159265f / 32768.0f;
			float align = landSpeed < 300.0f ? 1.0f
				: (std::cos(pitch) * std::cos(yaw) * lf.carVX + std::cos(pitch) * std::sin(yaw) * lf.carVY + std::sin(pitch) * lf.carVZ) / landSpeed;
			float kept = landSpeed / std::max(1.0f, speedAt(std::max(0, segs.lastTouch - 1)));
			bool flipped = false;
			for (int i = segs.lastTouch + 5; i < std::min(n, segs.lastTouch + 400); i++)
				if (fr[i].jump && !fr[i - 1].jump) { flipped = true; break; }
			bool poor = align < 0.3f || !flipped || landSpeed < 900.0f;
			bool good = !poor && kept >= 0.9f;
			std::string why = good ? std::format("Landed lined up and kept {:.0f}% of your speed", std::min(1.0f, kept) * 100.0f)
				: align < 0.3f ? "Landed sideways to where you were going"
				: !flipped ? "Didn't use a flip to get moving again after the touch"
				: landSpeed < 900.0f ? std::format("Landed slow ({})", Units::Speed(landSpeed))
				: std::format("Landed and kept {:.0f}% of your speed", std::min(1.0f, kept) * 100.0f);
			gradeSeg(recIdx, good ? Grade::Clean : poor ? Grade::Sloppy : Grade::Solid, why);
		}
	}

	// Sort by time, then point at the earliest real mistake (the one that set up the rest)
	std::sort(g.segs.begin(), g.segs.end(), [&](const SegmentGrade& a, const SegmentGrade& b) {
		return segs.segs[a.segIndex].start < segs.segs[b.segIndex].start;
	});
	for (int i = 0; i < static_cast<int>(g.segs.size()); i++)
	{
		if (segs.segs[g.segs[i].segIndex].kind == SegKind::Recovery) continue;
		if (IsMistake(g.segs[i].grade)) { g.biggestFix = i; break; }
	}
	// If the only problem is where the ball went, the fix is the touch that sent it there
	if (g.biggestFix >= 0 && segs.segs[g.segs[g.biggestFix].segIndex].kind == SegKind::Result)
	{
		for (int i = 0; i < static_cast<int>(g.segs.size()); i++)
		{
			SegKind k = segs.segs[g.segs[i].segIndex].kind;
			if (k == SegKind::Contact || k == SegKind::Flick) { g.biggestFix = i; break; }
		}
	}
	return g;
}

float Grader::DoubleTapPace(const ShotAnalysis& shot)
{
	auto ramp = [](float v, float lo, float hi) { return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); };
	const float readSec = shot.backboardTick >= 0 ? (shot.touchTick - shot.backboardTick) / 120.0f : 1.0f;
	const float play = ramp(-shot.tapPlaySec, -4.0f, -2.8f);     // the whole play: full marks at 2.8 s, none at 4 s
	const float power = ramp(shot.shotSpeed, 2000.0f, 2900.0f);  // how hard
	const float read = ramp(-readSec, -0.8f, -0.15f);           // how quick off the wall
	const float tight = ramp(shot.tapAngle, 35.0f, 75.0f);       // bonus: a tighter angle
	return 0.5f * play + 0.3f * power + 0.2f * read + 0.15f * tight;
}
