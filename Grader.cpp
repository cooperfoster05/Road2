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
	const Cutoffs& For(ShotType t) { return t == ShotType::Ground ? kGround : t == ShotType::DoubleTap ? kDoubleTap : kAerial; }

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
		float ax = std::fabs(shot.backboardX);
		gradeSeg(setupIdx, FromBackboardSpot(shot.backboardZ, ax),
			std::format("Sent it to the backboard {:.0f} high, {:.0f} from the middle", shot.backboardZ, ax));
		int readIdx = findSeg(SegKind::Read, shotTouch);
		float readSec = (shot.touchTick - shot.backboardTick) / 120.0f;
		gradeSeg(readIdx, FromReadTime(readSec), std::format("Hit it {:.2f}s after it came off the backboard", readSec));
	}

	// ---- Contact: aim error, explained by where you hit the ball ----
	int contactIdx = findSeg(SegKind::Contact, shotTouch);
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
			&& (shot.type != ShotType::DoubleTap || shot.missDist > 0.0f);
		std::string why = !shot.reachesGoalLine ? "Never reached the goal line"
			: shot.missDist <= 0 ? "Looked on target but didn't go in"
			: std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist);
		// In a double-tap drill: set up the backboard but never got the second touch
		if (ref.DoubleTapDrill() && shot.type != ShotType::DoubleTap
			&& ShotGeometry::LastBackboard(fr, shot.touchTick + 1, static_cast<int>(fr.size()) - 2) >= 0)
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
			if (segs.segs[g.segs[i].segIndex].kind == SegKind::Contact) { g.biggestFix = i; break; }
	}
	return g;
}
