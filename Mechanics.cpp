// No pch on purpose: plain C++ (see Mechanics.h)
#include "Mechanics.h"

#include <algorithm>
#include <cmath>

namespace
{
	const std::vector<MechanicSpec>& Table()
	{
		static const std::vector<MechanicSpec> t = {
			{ ShotType::Ground, "Ground shot",
				"A ground shot: hit from the ground (or a short jump into a low ball), hard enough to count.",
				{ { SegKind::Approach, "Line-up", "you reach the ball in time" },
				  { SegKind::Contact, "Shot", "a hit toward the goal" } } },
			{ ShotType::Aerial, "Aerial",
				"An aerial flown like yours: boost in the air (or contact as high as yours), hit hard, no tip-ins.",
				{ { SegKind::Approach, "Line-up", "you leave the ground on time" },
				  { SegKind::Takeoff, "Takeoff", "off the ground and climbing" },
				  { SegKind::Flight, "Flight", "you reach the ball in the air" },
				  { SegKind::Contact, "Shot", "a hit toward the goal" } } },
			{ ShotType::DoubleTap, "Double tap",
				"A double tap off the same surface you used (backboard, ceiling or wall top), the ball read off the wall (no pinching it on the wall), scored on the second touch.",
				{ { SegKind::Setup, "Setup", "the first touch sends the ball into the backboard" },
				  { SegKind::Read, "Read", "you get to the rebound" },
				  { SegKind::Contact, "Shot", "the second touch goes toward the goal" } } },
			{ ShotType::Dribble, "Dribble",
				"A real dribble: the ball carried on your car, then flicked off it (if you flicked).",
				{ { SegKind::Approach, "Pickup", "the ball settles on your car" },
				  { SegKind::Dribble, "Carry", "you keep it on your car" },
				  { SegKind::Flick, "Flick", "you flick it off your car" },
				  { SegKind::Contact, "Shot", "the flick goes toward the goal" } } },
			{ ShotType::AirDribble, "Air dribble",
				"An air dribble: two or more touches in the air with no landing, the ball kept about as close as you kept it, then the shot.",
				{ { SegKind::Approach, "Setup", "you get off the ground or wall with the ball" },
				  { SegKind::AirDribble, "Carry", "touches in a row in the air, the ball kept close" },
				  { SegKind::Contact, "Shot", "the last touch goes toward the goal" } } },
			{ ShotType::FlipReset, "Flip reset",
				"A flip reset: get the reset off the ball, and if you shot off the flip, the line does too.",
				{ { SegKind::Approach, "Setup", "you get in the air with the ball" },
				  { SegKind::Reset, "Reset", "your wheels touch the ball and give you your flip back" },
				  { SegKind::Flip, "Flip", "you use the flip" },
				  { SegKind::Contact, "Shot", "the flip hits the ball toward the goal" } } },
		};
		return t;
	}
}

const MechanicSpec& Mechanics::Spec(ShotType t)
{
	for (const MechanicSpec& m : Table()) if (m.type == t) return m;
	return Table()[0];
}

float Mechanics::MinShotSpeed(ShotType t)
{
	switch (t)
	{
	case ShotType::Dribble: return 1200.0f;   // flicks off a slow carry can be soft and still be real
	default:                return 1400.0f;
	}
}

bool Mechanics::SameMechanic(const ShotAnalysis& yours, const std::vector<Frame>& yourFrames,
	const ShotAnalysis& line, const std::vector<Frame>& lineFrames, int start, std::string* why)
{
	auto no = [&](const char* reason) { if (why) *why = reason; return false; };
	if (!yours.valid) return true;            // nothing to match
	if (!line.valid) return no("never touched the ball");
	if (line.type != yours.type) return no("a different kind of shot");
	if (line.reachesGoalLine && line.shotSpeed < MinShotSpeed(yours.type)) return no("a tip, not a shot");

	switch (yours.type)
	{
	case ShotType::Ground:
	{
		if (lineFrames.empty()) return true;
		// Played like a ground shot (Oct 22, from Coop's 507 ground shots). (1) No hovering: the engine could jump,
		// boost and glide a foot off the floor for a long time to meet the ball exactly where it wanted. Coop is never
		// airborne under 120 high for more than about 0.3 s before the touch (99% of his reps), so a line gets 0.35 s
		// (or a tenth of a second more than he did). (2) Straight in: only 2 of his 507 ground shots touched the
		// ceiling on the way and none a side wall, so a line that banks it in off the ceiling or a wall isn't the shot
		// he went for (unless his did too).
		auto lowAir = [](const std::vector<Frame>& f, int touch) {
			const int n = static_cast<int>(f.size());
			if (n == 0) return 0;
			touch = std::clamp(touch, 0, n - 1);
			int lg = touch;
			while (lg > 0 && !f[lg].onGround) lg--;
			int low = 0;
			for (int i = lg + 1; i <= touch; i++) low += f[i].carZ < 120.0f ? 1 : 0;
			return low;
		};
		auto banked = [](const std::vector<Frame>& f, int touch) {
			for (int i = std::max(0, touch); i < static_cast<int>(f.size()); i++)
			{
				if (std::fabs(f[i].ballY) > 5120.0f) break;
				if (f[i].ballZ > 1930.0f || std::fabs(f[i].ballX) > 3980.0f) return true;
			}
			return false;
		};
		if (lowAir(lineFrames, line.touchTick) > std::max(42, lowAir(yourFrames, yours.touchTick) + 12)) return no("hovered low over the ground to reach it");
		if (banked(lineFrames, line.touchTick) && !banked(yourFrames, yours.touchTick)) return no("banked it off the ceiling or a wall");
		return true;
	}
	case ShotType::Aerial:
	{
		if (lineFrames.empty()) return true;
		// Flown like yours: boost in the air before the hit, or contact as high as you made it
		const int yn = static_cast<int>(yourFrames.size()), ln = static_cast<int>(lineFrames.size());
		int yourAir = 0, lineAir = 0;
		for (int i = std::max(0, start); i < std::min(yn, yours.touchTick); i++) yourAir += (!yourFrames[i].onGround && yourFrames[i].boost) ? 1 : 0;
		for (int i = std::max(0, start); i < std::min(ln, line.touchTick); i++) lineAir += (!lineFrames[i].onGround && lineFrames[i].boost) ? 1 : 0;
		float yourZ = yn ? yourFrames[std::clamp(yours.touchTick, 0, yn - 1)].carZ : 0.0f;
		float lineZ = lineFrames[std::clamp(line.touchTick, 0, ln - 1)].carZ;
		if (lineAir >= std::min(12, yourAir / 2) || lineZ >= 0.7f * yourZ) return true;
		return no("a jump-and-tip, not an aerial");
	}
	case ShotType::DoubleTap:
		if (!line.doubleTap) return no("no double tap");
		if (yours.tapSurface != 0 && line.tapSurface != yours.tapSurface) return no("off a different surface");
		// A read of the rebound, not a pinch: the ball has to come off the wall before the second touch (at least
		// about half a ball, more if you let yours come further off). Hitting it while it's still on the wall
		// smothers it into the goal frame and drops it in on the line.
		if (line.reboundDist < std::min(150.0f, std::max(100.0f, 0.75f * yours.reboundDist))) return no("pinched it on the wall, not a rebound");
		return true;
	case ShotType::Dribble:
	{
		if (!line.dribble) return no("no carry");
		float need = std::max(0.4f, 0.5f * yours.dribbleSec);
		if (line.dribbleSec < need) return no("barely carried it");
		if (yours.flickJumpTick >= 0 && line.flickJumpTick < 0) return no("no flick");
		return true;
	}
	case ShotType::AirDribble:
		if (!line.airDribble || line.airTouches < 2) return no("not an air dribble");
		// Kept about as close as you kept it (your own air dribbles sit around a third of the air time within 230)
		if (line.airCloseFrac < std::min(0.5f, 0.75f * yours.airCloseFrac)) return no("the ball wasn't kept close");
		return true;
	case ShotType::FlipReset:
		if (!line.flipReset) return no("no reset");
		if (yours.flipShot && (line.resetDodge < 0 || !line.flipShot)) return no("the flip wasn't used on the shot");
		return true;
	}
	return true;
}

int Mechanics::StepsPassed(const MechanicSpec& spec, const ShotAnalysis& a, bool wentIn)
{
	if (!a.valid) return 0;
	int n = 0;
	for (const MechanicStep& st : spec.steps)
	{
		bool ok = true;
		switch (st.seg)
		{
		case SegKind::Setup:      ok = a.doubleTap; break;
		case SegKind::Read:       ok = a.doubleTap && a.touchTick > a.backboardTick; break;
		case SegKind::Dribble:    ok = a.dribble && a.dribbleSec >= 0.4f; break;
		case SegKind::Flick:      ok = a.dribble && a.flickJumpTick >= 0; break;
		case SegKind::AirDribble: ok = a.airDribble && a.airTouches >= 2; break;
		case SegKind::Reset:      ok = a.flipReset; break;
		case SegKind::Flip:       ok = a.flipReset && a.resetDodge >= 0; break;
		case SegKind::Contact:    ok = wentIn; break;
		default:                  ok = true; break;   // getting there: passed if a later step happened
		}
		if (!ok) break;
		n++;
	}
	return n;
}

int Mechanics::MechanicStart(const ShotAnalysis& a, int frameCount)
{
	if (!a.valid) return 1;
	int t = a.startTick;
	switch (a.type)
	{
	case ShotType::DoubleTap:  if (a.setupTouchTick >= 0) t = a.setupTouchTick - 60; break;
	case ShotType::AirDribble: if (a.airStart >= 0) t = a.airStart - 30; break;
	case ShotType::FlipReset:  if (a.resetTakeoff >= 0) t = a.resetTakeoff - 30; break;
	case ShotType::Dribble:    if (a.dribbleStart >= 0) t = a.dribbleStart - 30; break;
	default: break;
	}
	t = std::max(t, a.startTick);
	return std::clamp(t, 1, std::max(1, frameCount - 2));
}
