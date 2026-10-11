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
	// Air dribbles: closeMiss, and upperZ = high enough in the net for a 4 (details below)
	constexpr Cutoffs kAirDribble{ 150.0f, 480.0f, 2400.0f, 0.20f, 3.5f, 1.6f, 0.05f, 200.0f, 3500.0f };
	const Cutoffs& For(ShotType t)
	{
		return t == ShotType::Ground ? kGround : t == ShotType::DoubleTap ? kDoubleTap : t == ShotType::Dribble ? kDribble
			: t == ShotType::AirDribble ? kAirDribble : kAerial;
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
	constexpr float kAdLongSec = 4.5f;
	Grade FromAirControl(int touches, float push)
	{
		if (touches >= 5 && push <= 450.0f) return Grade::Clean;
		if (touches >= 3 && push <= 750.0f) return Grade::Solid;
		return Grade::Sloppy;
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
	// A 5 (none in the data yet): high, hard and accurate
	constexpr float kDtGreatZ = 550.0f, kDtGreatSpeed = 2800.0f, kDtGreatAim = 2.0f;

	// First-pass setup and read grades for double taps (refine once there are more rated double taps)
	Grade FromBackboardSpot(float z, float absX)
	{
		if (z >= 500.0f && z <= 1300.0f && absX <= 1600.0f) return Grade::Clean;
		if (z >= 300.0f && z <= 1700.0f && absX <= 2400.0f) return Grade::Solid;
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
	bool bounced = shot.type == ShotType::Aerial && (bouncedIn || (wentIn && shot.dipsToFloor));
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
			gradeSeg(setupIdx, FromBackboardSpot(shot.backboardZ, ax),
				std::format("Sent it to the backboard {:.0f} high, {:.0f} from the middle", shot.backboardZ, ax));
		else
			gradeSeg(setupIdx, Grade::Solid, std::format("Sent it off {} ({:.0f} high)", surface, shot.backboardZ));
		int readIdx = findSeg(SegKind::Read, shotTouch);
		float readSec = (shot.touchTick - shot.backboardTick) / 120.0f;
		gradeSeg(readIdx, FromReadTime(readSec), std::format("Hit it {:.2f}s after it came off {}", readSec, surface));
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
			std::string why = std::format("Flick left your car {:.1f} deg off", shot.aimErrDeg);
			if (!shot.missDir.empty()) why += std::format(" (went {})", shot.missDir);
			gradeSeg(flickIdx, FromAim(shot.aimErrDeg), why);
		}
		else
		{
			bool lifted = shot.flickLift >= kFlickLift, hard = shot.shotSpeed >= kFlickSpeed;
			Grade fg = lifted && hard ? (shot.shotSpeed >= kFlickRocket ? Grade::Clean : Grade::Solid) : Grade::Sloppy;
			gradeSeg(flickIdx, fg, std::format("Flick sent it up at {} and off at {}{}", Units::Speed(shot.flickLift),
				Units::Speed(shot.shotSpeed), !lifted ? " (not much lift)" : !hard ? " (not much power)" : ""));
		}
	}

	// ---- Air dribble: how well you carried it ----
	if (shot.type == ShotType::AirDribble)
	{
		int before = shot.airTouches - 1;
		std::string why = std::format("{} touch{} in the air before the shot, {} of push each on average, ball close to your car {:.0f}% of the time",
			before, before == 1 ? "" : "es", Units::Speed(shot.airImpulse), shot.airCloseFrac * 100.0f);
		Grade cg = FromAirControl(shot.airTouches, shot.airImpulse);
		if (cg == Grade::Sloppy)
			why += shot.airTouches < kAdChainFor4 ? " (barely carried it)" : " (hitting it more than carrying it)";
		gradeSeg(findSeg(SegKind::AirDribble, 0), cg, why);
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
			&& ((shot.type != ShotType::DoubleTap && shot.type != ShotType::AirDribble) || shot.missDist > 0.0f);
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
		bool great = shot.reachesGoalLine && shot.crossZ >= kDtGreatZ && shot.shotSpeed >= kDtGreatSpeed && shot.aimErrDeg <= kDtGreatAim;
		g.overall = high ? (great ? 5 : 4) : 3;
		std::string why = std::format("{} goal, {}, {} ({})", "Double tap",
			high ? "high in the net" : "low in the net", shot.shotSpeed >= kDtReadFast ? "hard" : "soft", Units::Speed(shot.shotSpeed));
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
		std::string why = std::format("Goal off the flick, {}, {} ({}){}{}", lifted ? "good lift" : "not much lift",
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
		const Segment& rs = segs.segs[recIdx];
		if (segs.recoveredTick < 0)
		{
			int afterTouch = static_cast<int>(fr.size()) - 1 - segs.lastTouch;
			gradeSeg(recIdx, afterTouch < 300 ? Grade::None : Grade::Costly,
				afterTouch < 300 ? "Rep ended before you landed" : "Never got back in control");
		}
		else
		{
			float t = (segs.recoveredTick - rs.start) / 120.0f;
			auto speedAt = [&](int i) { const Frame& f = fr[i]; return std::sqrt(f.carVX * f.carVX + f.carVY * f.carVY + f.carVZ * f.carVZ); };
			float kept = speedAt(segs.recoveredTick) / std::max(1.0f, speedAt(std::max(0, segs.lastTouch - 1)));
			float score = 0.40f * 1.0f                                              // aligned by definition once recovered
				+ 0.35f * std::min(1.0f, kept)
				+ 0.25f * std::max(0.0f, std::min(1.0f, 1.0f - (t - 0.5f) / 2.0f));
			Grade rg = score >= 0.80f ? Grade::Clean : score >= 0.65f ? Grade::Solid : score >= 0.50f ? Grade::Sloppy : Grade::Costly;
			gradeSeg(recIdx, rg, std::format("Back in control {:.2f}s after the touch, kept {:.0f}% of your speed", t, std::min(1.0f, kept) * 100));
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
