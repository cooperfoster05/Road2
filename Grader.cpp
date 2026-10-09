#include "pch.h"
#include "Grader.h"
#include "Units.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
	// ---- Thresholds (tuned on Coop's rated reps, Oct 9) ----
	constexpr float kCloseMiss = 150.0f;       // missed by this much or less = "Close"
	constexpr float kUpperHalfZ = 321.0f;      // ball center height at the line; above = upper half of the net
	constexpr float kFastShot = 2500.0f;       // shot speed for a "fast" shot
	constexpr float kEfficientSec = 0.15f;     // touch within this of your fastest goal on the shot
	constexpr float kCleanAim = 2.5f;          // aim error for a clean touch (degrees)
	// A 5 = everything a 4 has, and elite at something
	constexpr float kEliteAim = 1.6f;
	constexpr float kEliteTiming = 0.10f;
	constexpr float kCornerDist = 200.0f;
	constexpr float kRocketShot = 3000.0f;

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
	bool bounced = bouncedIn || (wentIn && shot.dipsToFloor);
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
	bool upper = shot.reachesGoalLine && shot.crossZ >= kUpperHalfZ;
	bool fast = shot.shotSpeed >= kFastShot;
	if (!wentIn)
	{
		bool close = shot.reachesGoalLine && shot.missDist <= kCloseMiss;
		std::string why = !shot.reachesGoalLine ? "Never reached the goal line"
			: shot.missDist <= 0 ? "Looked on target but didn't go in"
			: std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist);
		gradeSeg(resultIdx, close ? Grade::Sloppy : Grade::Costly, why);
		g.overall = close ? 2 : 1;
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
			+ ((!ref.valid || vsBest <= kEfficientSec) ? 1 : 0) + (shot.aimErrDeg <= kCleanAim ? 1 : 0);
		if (bounced) g.overall = 3;
		else if (points < 4) g.overall = 3;
		else if (points == 5 && shot.aimErrDeg <= kEliteAim && (!ref.valid || vsBest <= kEliteTiming)
			&& (shot.cornerDist <= kCornerDist || shot.shotSpeed >= kRocketShot)) g.overall = 5;
		else g.overall = 4;

		g.overallWhy = why;
		if (g.overall == 5) g.overallWhy += shot.cornerDist <= kCornerDist ? ", top corner" : ", rocket";
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
