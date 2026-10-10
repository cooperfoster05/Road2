#include "pch.h"
#include "Segmenter.h"
#include "ShotGeometry.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr int kTakeoffTicks = 60;      // takeoff covers 0.5 s after the first jump
	constexpr int kContactBefore = 4;      // contact window: a few ticks either side of the touch
	constexpr int kContactAfter = 3;
	constexpr int kControlTicks = 30;      // 0.25 s in control counts as recovered
	constexpr float kAlignCos = 0.82f;     // about 35 degrees between nose and travel direction
}

const char* Segmenter::Name(SegKind k)
{
	switch (k)
	{
	case SegKind::Approach: return "Approach";
	case SegKind::Takeoff:  return "Takeoff";
	case SegKind::Flight:   return "Flight";
	case SegKind::Contact:  return "Contact";
	case SegKind::Result:   return "Result";
	case SegKind::Recovery: return "Recovery";
	case SegKind::Setup:    return "Setup touch";
	case SegKind::Read:     return "Backboard read";
	}
	return "";
}

bool Segmenter::InControl(const Frame& f)
{
	if (!f.onGround && !f.onWall) return false;
	float speed = std::sqrt(f.carVX * f.carVX + f.carVY * f.carVY + f.carVZ * f.carVZ);
	if (speed < 300.0f) return true;
	const float toRad = 3.14159265f / 32768.0f;
	float p = f.carPitch * toRad, y = f.carYaw * toRad;
	float fx = std::cos(p) * std::cos(y), fy = std::cos(p) * std::sin(y), fz = std::sin(p);
	float align = (fx * f.carVX + fy * f.carVY + fz * f.carVZ) / speed;
	return align >= kAlignCos;
}

RepSegments Segmenter::Split(const std::vector<Frame>& fr, int eventTick)
{
	RepSegments r;
	const int n = static_cast<int>(fr.size());
	if (n < 2) return r;
	const int last = n - 1;
	const int eventEnd = (eventTick > 0 && eventTick <= last) ? eventTick : last;

	for (int i = 0; i < n; i++)
		if (std::fabs(fr[i].ballVX) + std::fabs(fr[i].ballVY) + std::fabs(fr[i].ballVZ) > 1.0f) { r.startTick = i; break; }

	for (int i = 1; i < n; i++)
		if (fr[i].touch && !fr[i - 1].touch) r.touchTicks.push_back(i);
	r.whiff = r.touchTicks.empty();
	if (!r.whiff) r.lastTouch = r.touchTicks.back();

	auto add = [&](SegKind k, int a, int b, int touch = 0) {
		a = std::max(a, 0);
		b = std::min(b, last);
		if (b >= a) r.segs.push_back(Segment{ k, a, b, touch });
	};

	// First jump press from the ground within [a, b], or -1
	auto groundJump = [&](int a, int b) {
		for (int i = std::max(1, a); i <= std::min(b, last); i++)
			if (fr[i].jump && !fr[i - 1].jump && (fr[i].onGround || fr[i - 1].onGround)) return i;
		return -1;
	};

	// One stretch of play leading into a touch (or the end of the shot):
	// driving -> jump -> air, or just air if you were already flying
	auto addLeadIn = [&](int a, int b, int touch) {
		if (b < a) return;
		int j = groundJump(a, b);
		if (j >= 0)
		{
			add(SegKind::Approach, a, j - 1, touch);
			int takeoffEnd = std::min(j + kTakeoffTicks - 1, b);
			add(SegKind::Takeoff, j, takeoffEnd, touch);
			add(SegKind::Flight, takeoffEnd + 1, b, touch);
		}
		else
		{
			// No jump: call it driving if the car was mostly on the ground, otherwise air time
			int grounded = 0;
			for (int i = a; i <= b; i++) if (fr[i].onGround || fr[i].onWall) grounded++;
			add(grounded * 2 >= (b - a + 1) ? SegKind::Approach : SegKind::Flight, a, b, touch);
		}
	};

	// Overall first jump and the second jump that follows it (for takeoff grading)
	r.firstJump = groundJump(r.startTick, last);
	if (r.firstJump >= 0)
	{
		for (int i = r.firstJump + 1; i < n; i++)
		{
			if (fr[i].jump && !fr[i - 1].jump && !fr[i].onGround) { r.secondJump = i; break; }
			if (fr[i].onGround && i > r.firstJump + 10) break;
		}
	}

	if (r.whiff)
	{
		addLeadIn(0, eventEnd, 1);
		return r;
	}

	// Lead-in and contact for each touch
	int cursor = 0;
	for (size_t t = 0; t < r.touchTicks.size(); t++)
	{
		int k = r.touchTicks[t];
		int a = std::max(k - kContactBefore, cursor);
		addLeadIn(cursor, a - 1, static_cast<int>(t) + 1);
		int b = k + kContactAfter;
		add(SegKind::Contact, a, b, static_cast<int>(t) + 1);
		cursor = b + 1;
	}

	// Double tap: the touch that sent the ball to the backboard is the setup, and the stretch after the
	// backboard (before your final touch) is the read
	if (r.touchTicks.size() >= 2)
	{
		int bb = ShotGeometry::LastWallTap(fr, r.startTick, r.lastTouch - 1);
		bool floorBetween = false;
		if (bb >= 0)
			for (int i = bb + 1; i < r.lastTouch; i++) if (fr[i].ballZ < 110.0f && fr[i - 1].ballZ >= 110.0f) { floorBetween = true; break; }
		if (bb >= 0 && !floorBetween)
		{
			const int shotTouch = static_cast<int>(r.touchTicks.size());
			int setupTouch = 0;
			for (size_t t = 0; t < r.touchTicks.size(); t++) if (r.touchTicks[t] <= bb) setupTouch = static_cast<int>(t) + 1;
			if (setupTouch > 0 && setupTouch < shotTouch)
				for (Segment& sg : r.segs)
				{
					if (sg.kind == SegKind::Contact && sg.touch == setupTouch) sg.kind = SegKind::Setup;
					if (sg.touch == shotTouch && (sg.kind == SegKind::Flight || sg.kind == SegKind::Approach) && sg.end >= bb) sg.kind = SegKind::Read;
				}
		}
	}

	// After the last touch: result runs to the end of the shot, recovery until back in control
	int after = r.lastTouch + kContactAfter + 1;
	add(SegKind::Result, after, std::max(after, eventEnd));

	int run = 0;
	for (int i = after; i < n; i++)
	{
		run = InControl(fr[i]) ? run + 1 : 0;
		if (run >= kControlTicks) { r.recoveredTick = i - kControlTicks + 1; break; }
	}
	add(SegKind::Recovery, after, r.recoveredTick >= 0 ? r.recoveredTick : last);
	return r;
}

std::string ShotEndTracker::Update(const Frame& f, bool hadPrev, float prevBallZ, int framesSoFar, int& eventTick)
{
	constexpr float kFloorZ = 110.0f;          // ball center this low = touching the floor
	constexpr float kGroundShotMaxZ = 350.0f;  // same cutoff ShotGeometry uses to call a touch a ground shot
	constexpr int   kAwayTicks = 45;           // 0.375 s not heading toward the goal = the shot is done
	constexpr float kTowardSpeed = 150.0f;

	bool carOnFloor = f.onGround && f.carZ < 100.0f;
	if (f.carZ > 120.0f) carLeftFloor = true;

	if (f.touch)
	{
		if (f.tick - lastTouchFrame > 12)
		{
			touches++;
			groundTouch = carOnFloor || f.ballZ < kGroundShotMaxZ;
			touchTick = f.tick;
			goalSign = 0;
			awayRun = 0;
		}
		lastTouchFrame = f.tick;
	}

	// Dribbling: you touched it within the last few ticks, or it's sitting just above your car.
	// While that lasts the shot isn't over; when it ends (a flick, or it rolls off) we judge from there.
	{
		float dx = f.ballX - f.carX, dy = f.ballY - f.carY, above = f.ballZ - f.carZ;
		bool onCar = dx * dx + dy * dy < 160.0f * 160.0f && above > 40.0f && above < 260.0f;
		bool nowCarrying = touches > 0 && (f.tick - lastTouchFrame <= 20 || onCar);
		if (carrying && !nowCarrying)
		{
			touchTick = lastTouchFrame;   // judge where it goes from the moment it left your car
			goalSign = 0;
			awayRun = 0;
		}
		carrying = nowCarrying;
	}

	// A few ticks after a ground touch, the ball's direction says which goal it was hit at
	if (groundTouch && touches > 0 && goalSign == 0 && f.tick >= touchTick + 4)
		goalSign = std::fabs(f.ballVY) > 200.0f ? (f.ballVY > 0 ? 1 : -1) : (f.ballY >= 0 ? 1 : -1);

	// Before any touch: only a whiff in the air ends it (ball lands while you're up in the air going for it).
	// A bouncing ball with you still on or near the floor is normal in ground packs.
	// Off the backboard (a double tap, or a pack that throws it there): the play goes on until the ball lands
	if (f.touch) offBackboard = false;
	if (framesSeen >= 2 && ShotGeometry::IsWallTap(prev2, f)) offBackboard = true;
	prev2 = prev1;
	prev1 = f;
	framesSeen++;

	bool aerialPlay = offBackboard || (touches > 0 ? !groundTouch : (carLeftFloor && f.carZ > 250.0f));
	bool floorHit = hadPrev && framesSoFar > 30 && f.ballZ < kFloorZ && prevBallZ >= kFloorZ;
	if (aerialPlay && floorHit && !carrying)
	{
		eventTick = f.tick;
		return "floor";
	}

	if (touches > 0 && groundTouch && goalSign != 0 && !offBackboard && !carrying)
	{
		awayRun = f.ballVY * goalSign < kTowardSpeed ? awayRun + 1 : 0;
		if (awayRun >= kAwayTicks)
		{
			eventTick = f.tick - kAwayTicks + 1;
			return "miss";
		}
	}
	return "";
}
