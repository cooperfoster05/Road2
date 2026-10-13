// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "WhatIf.h"
#include "Coach.h"
#include "Grader.h"
#include "Mechanics.h"
#include "Segmenter.h"
#include "ShotGeometry.h"

#include "third_party/RocketSim/src/RocketSim.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <random>

using namespace RocketSim;

namespace
{
	constexpr float kU2R = 3.14159265f / 32768.0f;   // Unreal rotation units to radians
	constexpr float kR2U = 32768.0f / 3.14159265f;
	constexpr int   kNudges = 6;                      // slightly nudged starts each finding has to survive
	constexpr int   kNudgesNeeded = 5;

	// Puts the sim's car and ball exactly where the recording had them at tick s
	void LoadState(Car* car, Ball* ball, const std::vector<Frame>& fr, int s)
	{
		const Frame& f = fr[s];
		CarState cs;
		cs.pos = Vec(f.carX, f.carY, f.carZ);
		cs.vel = Vec(f.carVX, f.carVY, f.carVZ);
		cs.angVel = Vec(f.carAVX, f.carAVY, f.carAVZ);
		cs.rotMat = Angle(f.carYaw * kU2R, f.carPitch * kU2R, f.carRoll * kU2R).ToRotMat();
		cs.boost = f.boostAmount;
		cs.isOnGround = f.onGround;
		if (!f.onGround)
		{
			// How you got into the air decides whether (and for how long) you still have a flip
			int jumpTick = -1;
			bool reset = false;
			for (int i = s; i > 0; i--)
			{
				if (fr[i].onGround && ShotGeometry::OnSurface(fr[i])) break;
				if (ShotGeometry::IsFlipReset(fr[i - 1], fr[i])) { reset = true; break; }
				if (fr[i].jump && !fr[i - 1].jump && fr[i - 1].onGround) { jumpTick = i; break; }
			}
			if (reset)
			{
				// A flip from a reset: in the air without having jumped, and it doesn't run out
				cs.hasJumped = false;
				cs.airTimeSinceJump = 0;
				cs.airTime = 0.5f;
				cs.hasFlipped = !f.hasFlip;
				cs.hasDoubleJumped = false;
			}
			else
			{
				cs.hasJumped = jumpTick >= 0;
				cs.airTimeSinceJump = jumpTick >= 0 ? (s - jumpTick) / 120.0f : 0.0f;
				cs.airTime = cs.airTimeSinceJump;
				cs.hasFlipped = !f.hasFlip;
				cs.hasDoubleJumped = !f.hasFlip;
				if (jumpTick >= 0 && s - jumpTick < 24 && f.jump) { cs.isJumping = true; cs.jumpTime = (s - jumpTick) / 120.0f; }
			}
		}
		car->SetState(cs);
		BallState bs;
		bs.pos = Vec(f.ballX, f.ballY, f.ballZ);
		bs.vel = Vec(f.ballVX, f.ballVY, f.ballVZ);
		bs.angVel = Vec(f.ballAVX, f.ballAVY, f.ballAVZ);
		ball->SetState(bs);
	}

	CarControls ControlsOf(const Frame& f)
	{
		CarControls c;
		c.throttle = f.throttle; c.steer = f.steer; c.pitch = f.pitch; c.yaw = f.yaw; c.roll = f.roll;
		c.jump = f.jump; c.boost = f.boost; c.handbrake = f.handbrake;
		return c;
	}

	// Stage 2: the key moment an earlier change has to still produce, so the rest of your inputs can line up with it
	enum class KeyEvent { None, Reset, WallTap, Touch };

	bool KeyHappened(KeyEvent kind, const std::vector<Frame>& sim, const Frame& g)
	{
		const Frame& prev = sim.back();
		if (kind == KeyEvent::Reset)
		{
			// Wheels on the ball in open air (the sim reports it as touching the ground), or the flip coming back
			float dx = g.ballX - g.carX, dy = g.ballY - g.carY, dz = g.ballZ - g.carZ;
			bool wheelsOnBall = g.onGround && !ShotGeometry::OnSurface(g) && g.carZ > 250.0f && dx * dx + dy * dy + dz * dz < 220.0f * 220.0f;
			return wheelsOnBall || ShotGeometry::IsFlipReset(prev, g);
		}
		if (kind == KeyEvent::WallTap) return sim.size() >= 2 && ShotGeometry::IsWallTap(sim[sim.size() - 2], g);
		if (kind == KeyEvent::Touch) return g.touch;
		return false;
	}

	struct KeyPhase
	{
		KeyEvent kind = KeyEvent::None;
		int realTick = -1;                         // when it happened in your rep
		const std::vector<CarControls>* before = nullptr;   // inputs until it happens (with the early changes)
	};

	struct SimRun
	{
		std::vector<Frame> frames;   // the real rep up to the start, then the simulation
		bool wentIn = false, floorFirst = false;
		int  eventTick = -1;
		int  keyTick = -1;           // stage 2: when the key moment happened in the sim
		bool keyMissed = false;      // stage 2: it never happened (the change lost the reset, the tap, the touch)
		ShotAnalysis shot;
		RepGrade grade;
	};

	// Runs the sim from tick s and grades the result. Without a key phase, ctl (indexed by tick) drives the car.
	// With one, key.before drives it until the key moment happens; from then on ctl does, shifted so your inputs
	// after that moment line up with when it happened in the sim.
	// nudge > 0 moves the start by a couple of units (a real attempt never starts from exactly the same spot).
	SimRun Run(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const std::vector<CarControls>& ctl,
		int maxTick, const ShotReference& ref, int nudge, const KeyPhase* key = nullptr)
	{
		SimRun r;
		r.frames.assign(fr.begin(), fr.begin() + s + 1);
		r.frames.reserve(maxTick + 2);
		LoadState(car, arena->ball, fr, s);
		if (nudge > 0)
		{
			std::mt19937 rng(static_cast<unsigned>(nudge) * 7919u);
			std::uniform_real_distribution<float> u(-1.0f, 1.0f);
			CarState cs = car->GetState();
			cs.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			cs.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
			cs.angVel += Vec(u(rng), u(rng), u(rng)) * 0.03f;
			car->SetState(cs);
			BallState bs = arena->ball->GetState();
			bs.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			bs.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
			arena->ball->SetState(bs);
		}

		bool phase2 = key == nullptr || key->kind == KeyEvent::None;
		int shift = 0;
		int lastTouch = -1, stopAt = -1;
		bool bounced = false;
		auto at = [](const std::vector<CarControls>& v, int i) { return i >= 0 && i < static_cast<int>(v.size()) ? v[i] : CarControls(); };
		for (int i = s; i < maxTick; i++)
		{
			CarControls c = phase2 ? at(ctl, i - shift) : at(*key->before, i);
			Frame& cur = r.frames.back();
			cur.throttle = c.throttle; cur.steer = c.steer; cur.pitch = c.pitch; cur.yaw = c.yaw; cur.roll = c.roll;
			cur.jump = c.jump; cur.boost = c.boost; cur.handbrake = c.handbrake;
			car->controls = c;
			uint64_t before = arena->tickCount;
			arena->Step(1);

			CarState cs = car->GetState();
			BallState bs = arena->ball->GetState();
			Frame g;
			g.tick = i + 1;
			g.t = (i + 1) / 120.0f;
			g.carX = cs.pos.x; g.carY = cs.pos.y; g.carZ = cs.pos.z;
			Angle a = Angle::FromRotMat(cs.rotMat);
			g.carYaw = static_cast<int>(std::lround(a.yaw * kR2U));
			g.carPitch = static_cast<int>(std::lround(a.pitch * kR2U));
			g.carRoll = static_cast<int>(std::lround(a.roll * kR2U));
			g.carVX = cs.vel.x; g.carVY = cs.vel.y; g.carVZ = cs.vel.z;
			g.carAVX = cs.angVel.x; g.carAVY = cs.angVel.y; g.carAVZ = cs.angVel.z;
			g.boostAmount = cs.boost;
			g.onGround = cs.isOnGround;
			g.onWall = cs.isOnGround && cs.rotMat.up.z < 0.7f;
			g.hasFlip = cs.HasFlipOrJump();
			g.ballX = bs.pos.x; g.ballY = bs.pos.y; g.ballZ = bs.pos.z;
			g.ballVX = bs.vel.x; g.ballVY = bs.vel.y; g.ballVZ = bs.vel.z;
			g.ballAVX = bs.angVel.x; g.ballAVY = bs.angVel.y; g.ballAVZ = bs.angVel.z;
			if (cs.ballHitInfo.isValid && cs.ballHitInfo.tickCountWhenHit >= before)
			{
				g.touch = true;
				Vec p = cs.ballHitInfo.ballPos + cs.ballHitInfo.relativePosOnBall;
				g.touchX = p.x; g.touchY = p.y; g.touchZ = p.z;
				lastTouch = i + 1;
			}
			if (!phase2)
			{
				if (i + 1 > s + 1 && KeyHappened(key->kind, r.frames, g))
				{
					phase2 = true;
					r.keyTick = i + 1;
					shift = r.keyTick - key->realTick;
				}
				else if (i + 1 > key->realTick + 45)
				{
					// The change lost the key moment: no point playing the rest out
					r.keyMissed = true;
					r.frames.push_back(g);
					break;
				}
			}
			r.frames.push_back(g);

			// Like the recorder: a goal ends it (even after a floor bounce); a floor bounce away from you ends the shot,
			// but the ball is followed while it's still rolling or bouncing toward the net (up to 6 s more)
			float dirY = bs.vel.y >= 0.0f ? 1.0f : -1.0f;
			float toLine = 5120.0f - bs.pos.y * dirY;
			float tLine = std::fabs(bs.vel.y) > 1.0f ? toLine / std::fabs(bs.vel.y) : 1e9f;
			bool rollingIn = std::fabs(bs.vel.y) > 300.0f && toLine > 0.0f && tLine < 6.0f && std::fabs(bs.pos.x + bs.vel.x * tLine) < 950.0f;
			if (!r.wentIn && std::fabs(bs.pos.y) > 5215.0f && std::fabs(bs.pos.x) < 893.0f && bs.pos.z < 643.0f)
			{
				r.wentIn = true;
				r.floorFirst = bounced;
				r.eventTick = i + 1;
				stopAt = i + 1 + 60;
				continue;
			}
			if (stopAt >= 0)
			{
				if (i + 1 >= stopAt && (r.wentIn || !rollingIn || i + 1 >= r.eventTick + 900)) break;
				continue;
			}
			if (lastTouch >= 0 && bs.pos.z < 100.0f && bs.vel.z > 0.0f && i + 1 - lastTouch > 10 && !ShotGeometry::BallWithCar(g))
			{
				bounced = true;
				r.eventTick = i + 1;
				stopAt = i + 1 + 60;
				continue;
			}
			if (lastTouch >= 0 && i + 1 - lastTouch > 480) { r.eventTick = i + 1; stopAt = i + 1 + 60; }
		}
		if (r.eventTick < 0) r.eventTick = static_cast<int>(r.frames.size()) - 1;
		r.shot = ShotGeometry::Analyze(r.frames);
		RepSegments segs = Segmenter::Split(r.frames, r.eventTick);
		r.grade = Grader::GradeRep(r.frames, segs, r.shot, r.wentIn, r.floorFirst, ref);
		return r;
	}

	// The dodge used for the shot when Analyze doesn't name one: a jump press in the air that used up your flip
	int FindFlip(const std::vector<Frame>& fr, int from, int to)
	{
		int found = -1;
		for (int i = std::max(1, from); i <= to && i + 1 < static_cast<int>(fr.size()); i++)
			if (fr[i].jump && !fr[i - 1].jump && !ShotGeometry::OnSurface(fr[i]) && fr[i].hasFlip && !fr[i + 1].hasFlip &&
				std::fabs(fr[i].pitch) + std::fabs(fr[i].yaw) + std::fabs(fr[i].roll) > 0.5f)
				found = i;
		return found;
	}

	// Higher is better: the grade first, then how good the shot was
	float Quality(const SimRun& r)
	{
		float q = r.grade.overall * 10000.0f;
		if (r.wentIn) q += 5000.0f - std::min(5000.0f, r.shot.cornerDist) + r.shot.shotSpeed * 0.5f - (r.shot.bouncedIn ? 3000.0f : 0.0f);
		else q -= r.shot.reachesGoalLine ? std::min(5000.0f, r.shot.missDist) : 5000.0f;
		if (r.keyMissed) q -= 20000.0f;
		return q;
	}

	// One "what if": a few small changes to your own inputs, all optional. Zero / kKeep = do what you did.
	constexpr int kKeep = 9999;
	struct Edit
	{
		// At the shot
		int   flipShift = 0;       // flip shots: the flip (and everything after it) this many ticks later (- = earlier)
		int   flipAngle = kKeep;   // flip shots: stick direction for the flip, degrees from straight forward (+ = right)
		float aimPitch = 0;        // extra stick back (+, nose up) / forward (-) just before the flip or touch
		float aimYaw = 0;          // extra stick right (+) / left (-)
		int   aimTicks = 0;        // over how long
		int   boost = 0;           // 0 yours, 1 none on the way in, 2 held the whole way, 3 let go for the last 0.2 s
		int   roll = 0;            // 0 yours, -1 air roll left, +1 right (on the way in)
		int   flipInto = 0;        // no-flip air shots: flip this many ticks before the touch (0 = don't)
		int   flipIntoAngle = 0;
		float steer = 0;           // on the ground: steer more right (+) / left (-) just before the touch
		int   steerTicks = 0;
		int   jumpInto = 0;        // on the ground: jump this many ticks before the touch (0 = don't)
		int   jumpFlipAngle = kKeep;   // ...and flip into it right after (kKeep = just the jump)

		// Stage 2: on the way to the key moment (the reset, the setup touch, the first air dribble touch)
		int   kBoost = 0;          // 0 yours, 1 none, 2 held, 3 feathered (on and off), 4 let go for the last 0.25 s
		float kAimPitch = 0, kAimYaw = 0;
		int   kAimTicks = 0;
		int   kRoll = 0;
		float kSteer = 0;          // on the ground on the way in

		bool Same(const Edit& o) const
		{
			auto eq = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
			return flipShift == o.flipShift && flipAngle == o.flipAngle && eq(aimPitch, o.aimPitch) && eq(aimYaw, o.aimYaw) &&
				(aimTicks == o.aimTicks || (aimPitch == 0 && aimYaw == 0)) && boost == o.boost && roll == o.roll &&
				flipInto == o.flipInto && (flipInto == 0 || flipIntoAngle == o.flipIntoAngle) &&
				eq(steer, o.steer) && (steerTicks == o.steerTicks || steer == 0) &&
				jumpInto == o.jumpInto && (jumpInto == 0 || jumpFlipAngle == o.jumpFlipAngle) &&
				kBoost == o.kBoost && eq(kAimPitch, o.kAimPitch) && eq(kAimYaw, o.kAimYaw) &&
				(kAimTicks == o.kAimTicks || (kAimPitch == 0 && kAimYaw == 0)) && kRoll == o.kRoll && eq(kSteer, o.kSteer);
		}
		bool Empty() const { return Same(Edit{}); }
		int ShotParts() const
		{
			return (flipShift != 0) + (flipAngle != kKeep) + (aimPitch != 0 || aimYaw != 0) + (boost != 0) + (roll != 0) +
				(flipInto != 0) + (steer != 0) + (jumpInto != 0);
		}
		int KeyParts() const { return (kBoost != 0) + (kAimPitch != 0 || kAimYaw != 0) + (kRoll != 0) + (kSteer != 0); }
		int Parts() const { return ShotParts() + KeyParts(); }
		// How big a change it is: smaller wins ties
		float Cost() const
		{
			return std::abs(flipShift) / 8.0f + (flipAngle != kKeep ? 1.5f : 0.0f) + (std::fabs(aimPitch) + std::fabs(aimYaw)) * 1.5f +
				(boost ? 1.0f : 0.0f) + (roll ? 1.5f : 0.0f) + (flipInto ? 2.0f : 0.0f) + std::fabs(steer) * 2.0f +
				(jumpInto ? (jumpFlipAngle != kKeep ? 2.5f : 1.5f) : 0.0f) +
				(kBoost ? 1.0f : 0.0f) + (std::fabs(kAimPitch) + std::fabs(kAimYaw)) * 1.5f + (kRoll ? 1.5f : 0.0f) + std::fabs(kSteer) * 2.0f;
		}
	};

	const CarConfig& Body(int carBody)
	{
		switch (carBody)
		{
		case 1: return CAR_CONFIG_DOMINUS;
		case 2: return CAR_CONFIG_PLANK;
		case 3: return CAR_CONFIG_BREAKOUT;
		case 4: return CAR_CONFIG_HYBRID;
		case 5: return CAR_CONFIG_MERC;
		default: return CAR_CONFIG_OCTANE;
		}
	}

	std::string Secs(int ticks) { return std::format("{:.2f} s", ticks / 120.0f); }

	struct Plan
	{
		int s = -1, k = -1, flip = -1, closer = -1;
		// Stage 2
		KeyEvent key = KeyEvent::None;
		int keyReal = -1;        // when the key moment happened in your rep
		int keyFrom = -1;        // where the changes before it start
		SegKind seg = SegKind::Contact;   // the breakdown line the button sits on
		std::string what;        // "the reset", "the setup touch", "the first touch"
	};

	Plan MakePlan(const std::vector<Frame>& fr, const ShotAnalysis& shot, WhatIfPoint point)
	{
		Plan p;
		if (!shot.valid) return p;
		p.k = shot.touchTick;
		p.flip = shot.flipTick >= 0 ? shot.flipTick : FindFlip(fr, p.k - 60, p.k);
		if (point == WhatIfPoint::KeyMoment)
		{
			// Start before the moment the shot was set up, change things on the way to it, and replay the rest
			// lined up with when it happens
			if (shot.type == ShotType::FlipReset && shot.resetTick >= 0)
			{
				p.key = KeyEvent::Reset; p.keyReal = shot.resetTick; p.seg = SegKind::Reset; p.what = "the reset";
				p.s = std::max(shot.resetTakeoff + 4, shot.resetTick - 60);
			}
			else if (shot.type == ShotType::DoubleTap && shot.backboardTick >= 0 && shot.setupTouchTick >= 0)
			{
				p.key = KeyEvent::WallTap; p.keyReal = shot.backboardTick; p.seg = SegKind::Setup; p.what = "the setup touch";
				p.s = shot.setupTouchTick - 30;
			}
			else if (shot.type == ShotType::AirDribble && shot.airFirstTouch >= 0)
			{
				p.key = KeyEvent::Touch; p.keyReal = shot.airFirstTouch; p.seg = SegKind::AirDribble; p.what = "the first touch";
				p.s = shot.airFirstTouch - 40;
			}
			else return Plan{};
			p.keyFrom = p.s;
			if (p.s < 2 || p.keyReal - p.s < 8) return Plan{};
			p.closer = p.keyReal - 24 > p.s + 4 ? p.keyReal - 24 : -1;
			return p;
		}
		p.s = p.flip >= 0 ? p.flip - 24 : p.k - 36;
		if (shot.dribble && shot.flickJumpTick >= 0) p.s = std::min(p.s, shot.flickJumpTick - 2);
		if (shot.flipReset && shot.resetTick >= 0) p.s = std::max(p.s, shot.resetTick + 3);
		if (p.flip < 0)
		{
			// Don't start in the middle of a flip
			for (int i = std::max(1, p.s - 78); i < p.s && i + 1 < static_cast<int>(fr.size()); i++)
				if (fr[i].jump && !fr[i - 1].jump && fr[i].hasFlip && !fr[i + 1].hasFlip && !fr[i + 1].onGround) { p.s = p.k - 12; break; }
		}
		if (p.s < 2 || (p.flip >= 0 && p.s >= p.flip)) p.s = -1;
		// A closer start for when the sim can't follow you from the first one (less room to change things, but more reps qualify)
		p.closer = p.flip >= 0 ? p.flip - 10 : p.k - 16;
		if (p.s < 0 || p.closer <= p.s + 4) p.closer = -1;
		// The breakdown line: the flick or flip that made the shot, or the contact / read
		p.seg = shot.dribble ? SegKind::Flick : shot.type == ShotType::FlipReset ? SegKind::Flip : SegKind::Contact;
		return p;
	}
}

int WhatIf::StartTick(const std::vector<Frame>& frames, WhatIfPoint point)
{
	return MakePlan(frames, ShotGeometry::Analyze(frames), point).s;
}

bool WhatIf::KeyMoment(const std::vector<Frame>& frames, SegKind& seg, std::string& what)
{
	Plan p = MakePlan(frames, ShotGeometry::Analyze(frames), WhatIfPoint::KeyMoment);
	if (p.s < 0) return false;
	seg = p.seg;
	what = p.what;
	return true;
}

const char* WhatIf::StatusText(WhatIfStatus s)
{
	switch (s)
	{
	case WhatIfStatus::NotNeeded:     return "Already a 5: nothing to improve on here.";
	case WhatIfStatus::NoShot:        return "Nothing to work from here.";
	case WhatIfStatus::CantSimulate:  return "The simulator can't follow this one closely enough to say (replaying your own inputs didn't give your result).";
	case WhatIfStatus::NothingBetter: return "No small change here would have saved it.";
	case WhatIfStatus::Cancelled:     return "Stopped.";
	default:                          return "";
	}
}

WhatIfResult WhatIf::Search(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int depth, WhatIfPoint point,
	const std::vector<int>* fixChecks)
{
	auto t0 = std::chrono::steady_clock::now();
	auto elapsed = [&]() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(); };
	// The search gets about two thirds of the time; the rest is kept for checking its best finds hold up
	auto cancelled = [&]() { return cancel && cancel->load(); };
	auto stop = [&]() { return cancelled() || elapsed() > maxSeconds * 0.65f; };
	auto hardStop = [&]() { return cancelled() || elapsed() > maxSeconds; };

	WhatIfResult out;
	out.depth = depth;
	out.point = point;
	const int n = static_cast<int>(fr.size());
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	Plan plan = MakePlan(fr, real, point);
	if (plan.s < 0) { out.status = WhatIfStatus::NoShot; return out; }
	const int k = plan.k, F = plan.flip;
	const bool keyMode = plan.key != KeyEvent::None;
	out.what = plan.what;

	RepSegments realSegs = Segmenter::Split(fr, eventTick >= 0 ? eventTick : n - 1);
	RepGrade realGrade = Grader::GradeRep(fr, realSegs, real, realIn, floorBeforeGoal, ref);
	out.realGrade = realGrade.overall;
	if (realGrade.overall >= 5) { out.status = WhatIfStatus::NotNeeded; return out; }

	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	Car* car = arena->AddCar(Team::BLUE, Body(carBody));

	std::vector<CarControls> realCtl(n);
	for (int i = 0; i < n; i++) realCtl[i] = ControlsOf(fr[i]);
	const int maxTick = std::min(n + 600, k + 720);
	auto matches = [&](const SimRun& r) { return !r.keyMissed && r.wentIn == realIn && std::abs(r.grade.overall - realGrade.overall) <= 1; };
	KeyPhase realKey{ plan.key, plan.keyReal, &realCtl };
	const KeyPhase* baseKey = keyMode ? &realKey : nullptr;

	// Gate: your own inputs replayed in the sim (from the exact start and from 6 slightly nudged ones) have to give
	// your result at least 3 times out of 7. A shot that only sometimes reproduces was a knife-edge one, and that's
	// fine: a what-if then has to work almost every time, which yours didn't. If the sim can't follow you from the
	// first start, try one closer.
	int s = -1, baseMatches = 0;
	SimRun base;
	for (int start : { plan.s, plan.closer })
	{
		if (start < 0 || hardStop()) continue;
		SimRun b0 = Run(arena.get(), car, fr, start, realCtl, maxTick, ref, 0, baseKey);
		int m = matches(b0) ? 1 : 0;
		out.tries++;
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun b = Run(arena.get(), car, fr, start, realCtl, maxTick, ref, j, baseKey);
			m += matches(b) ? 1 : 0;
			out.tries++;
		}
		out.note += std::format("Start {} before {}: your inputs gave your result {} of 7 times.\n",
			Secs((keyMode ? plan.keyReal : k) - start), keyMode ? plan.what : "the touch", m);
		if (m >= 3) { s = start; baseMatches = m; base = std::move(b0); break; }
	}
	if (s < 0)
	{
		out.status = WhatIfStatus::CantSimulate;
		out.ms = elapsed() * 1000.0f;
		return out;
	}
	out.startTick = s;
	out.simGrade = base.grade.overall;
	const int end = F >= 0 ? F : k;               // the flip, or the touch: shot aim and roll changes lead up to it
	const int shotLo = keyMode ? plan.keyReal + 2 : s;   // shot changes never reach back past the key moment
	const int keyEnd = plan.keyReal;

	auto onGroundNear = [&](int tick) { tick = std::clamp(tick, 0, n - 1); return ShotGeometry::OnSurface(fr[tick]); };
	const bool groundShot = onGroundNear(k - 1) || onGroundNear(k - 12);
	const bool airborne = !onGroundNear(std::max(shotLo, end - 30)) || !onGroundNear(end);
	const bool keyAirborne = keyMode && (!onGroundNear(s) || !onGroundNear(keyEnd - 1));
	const bool keyGround = keyMode && onGroundNear(s);

	// ---- Turn an edit into inputs ----
	auto stickFor = [](int deg, float& pitch, float& yaw) {
		float r = deg * 3.14159265f / 180.0f;
		pitch = -std::cos(r);   // stick forward = front flip
		yaw = std::sin(r);      // + = to your right
	};
	// Changes at the shot (on the replay of your own inputs)
	auto applyShot = [&](const Edit& e, std::vector<CarControls>& c) -> bool {
		c = realCtl;
		int flipAt = end;
		if (F >= 0)
		{
			flipAt = F + e.flipShift;
			if (flipAt <= shotLo + 1 || flipAt >= n - 1) return false;
			if (e.flipShift != 0)
			{
				// Everything from the flip on happens later (or earlier); a later flip holds the inputs from just before it
				for (int i = std::min(F, flipAt); i < n; i++)
				{
					if (i >= flipAt) { int src = i - e.flipShift; c[i] = src < n ? realCtl[src] : CarControls(); }
					else c[i] = realCtl[F - 1];
				}
			}
			if (c[flipAt - 1].jump) return false;   // still holding the first jump there: no new press, no flip
			if (e.flipAngle != kKeep)
			{
				float p, y;
				stickFor(e.flipAngle, p, y);
				for (int i = flipAt; i < std::min(n, flipAt + 30); i++) { c[i].pitch = p; c[i].yaw = y; c[i].roll = 0; }
			}
		}
		else if (e.flipInto > 0)
		{
			int at = k - e.flipInto;
			if (at <= shotLo + 1 || !fr[at].hasFlip || ShotGeometry::OnSurface(fr[at]) || c[at - 1].jump) return false;
			float p, y;
			stickFor(e.flipIntoAngle, p, y);
			for (int i = at; i < std::min(n, at + 30); i++) { c[i].pitch = p; c[i].yaw = y; c[i].roll = 0; c[i].jump = false; }
			c[at].jump = true;
			if (at + 1 < n) c[at + 1].jump = true;
			flipAt = at;
		}
		if (e.jumpInto > 0)
		{
			// From the ground: jump into it (held), or a quick jump and a flip into it
			int at = k - e.jumpInto;
			if (at <= shotLo + 1 || !ShotGeometry::OnSurface(fr[at]) || c[at - 1].jump) return false;
			for (int i = at; i < std::min(n, k + 30); i++) c[i].jump = false;
			int hold = e.jumpFlipAngle == kKeep ? std::min(24, e.jumpInto + 6) : 8;
			for (int i = at; i < std::min(n, at + hold); i++) c[i].jump = true;
			if (e.jumpFlipAngle != kKeep)
			{
				int flipTick = at + hold + 3;
				if (flipTick >= n) return false;
				float p, y;
				stickFor(e.jumpFlipAngle, p, y);
				for (int i = flipTick; i < std::min(n, flipTick + 30); i++) { c[i].pitch = p; c[i].yaw = y; c[i].roll = 0; }
				c[flipTick].jump = true;
				if (flipTick + 1 < n) c[flipTick + 1].jump = true;
			}
			else
				for (int i = at; i < std::min(n, at + hold); i++) { c[i].pitch = 0; c[i].roll = 0; }
		}
		if (e.steer != 0)
			for (int i = std::max(shotLo, k - e.steerTicks); i < k; i++) c[i].steer = std::clamp(c[i].steer + e.steer, -1.0f, 1.0f);
		if (e.aimPitch != 0 || e.aimYaw != 0)
			for (int i = std::max(shotLo, flipAt - e.aimTicks); i < flipAt; i++)
			{
				c[i].pitch = std::clamp(c[i].pitch + e.aimPitch, -1.0f, 1.0f);
				c[i].yaw = std::clamp(c[i].yaw + e.aimYaw, -1.0f, 1.0f);
			}
		if (e.roll != 0)
			for (int i = shotLo; i < flipAt; i++) c[i].roll = static_cast<float>(e.roll);
		if (e.boost == 1) for (int i = shotLo; i < k; i++) c[i].boost = false;
		if (e.boost == 2) for (int i = shotLo; i < k; i++) c[i].boost = true;
		if (e.boost == 3) for (int i = std::max(shotLo, k - 24); i < k; i++) c[i].boost = false;
		return true;
	};
	// Changes on the way to the key moment (stage 2)
	auto applyKey = [&](const Edit& e, std::vector<CarControls>& c) {
		c = realCtl;
		if (!keyMode) return;
		if (e.kBoost == 1) for (int i = s; i < keyEnd; i++) c[i].boost = false;
		if (e.kBoost == 2) for (int i = s; i < keyEnd; i++) c[i].boost = true;
		if (e.kBoost == 3) for (int i = s; i < keyEnd; i++) c[i].boost = ((i - s) / 6) % 2 == 0;   // feathered: on and off every 0.05 s
		if (e.kBoost == 4) for (int i = std::max(s, keyEnd - 30); i < keyEnd; i++) c[i].boost = false;
		if (e.kAimPitch != 0 || e.kAimYaw != 0)
			for (int i = std::max(s, keyEnd - e.kAimTicks); i < keyEnd; i++)
			{
				c[i].pitch = std::clamp(c[i].pitch + e.kAimPitch, -1.0f, 1.0f);
				c[i].yaw = std::clamp(c[i].yaw + e.kAimYaw, -1.0f, 1.0f);
			}
		if (e.kRoll != 0) for (int i = s; i < keyEnd; i++) c[i].roll = static_cast<float>(e.kRoll);
		if (e.kSteer != 0)
			for (int i = s; i < keyEnd; i++)
				if (ShotGeometry::OnSurface(fr[i])) c[i].steer = std::clamp(c[i].steer + e.kSteer, -1.0f, 1.0f);
	};
	struct Inputs { std::vector<CarControls> shot, before; };
	auto build = [&](const Edit& e, Inputs& in) -> bool {
		if (!applyShot(e, in.shot)) return false;
		applyKey(e, in.before);
		return true;
	};
	auto runEdit = [&](const Inputs& in, int nudge) {
		KeyPhase kp{ plan.key, plan.keyReal, &in.before };
		return Run(arena.get(), car, fr, s, in.shot, maxTick, ref, nudge, keyMode ? &kp : nullptr);
	};

	// ---- What each change looks like in words ----
	const bool flick = real.dribble;
	const char* flipWord = flick ? "flick" : "flip";
	bool realRight = false;
	FlipKind realKind = F >= 0 ? ShotGeometry::ClassifyFlip(fr[F], realRight) : FlipKind::None;
	auto flipLabel = [&](FlipKind kind, bool right) {
		std::string side = (kind == FlipKind::Front || kind == FlipKind::Back) ? "" : (right ? " (right)" : " (left)");
		return ShotGeometry::FlipName(kind, flick) + side;
	};
	auto kindOf = [&](int deg, bool& right) {
		float p, y;
		stickFor(deg, p, y);
		Frame probe; probe.pitch = p; probe.yaw = y;
		return ShotGeometry::ClassifyFlip(probe, right);
	};
	auto stickWords = [](float pitch, float yaw) {
		std::string s;
		if (pitch != 0) s = std::format("{:.0f}% more stick {}", std::fabs(pitch) * 100.0f, pitch > 0 ? "back (nose up)" : "forward (nose down)");
		if (yaw != 0) s += std::format("{}{:.0f}% more stick {}", s.empty() ? "" : " and ", std::fabs(yaw) * 100.0f, yaw > 0 ? "right" : "left");
		if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
		return s;
	};
	auto noseWords = [](float pitch, float yaw) {
		float mag = std::max(std::fabs(pitch), std::fabs(yaw));
		std::string dir;
		if (std::fabs(pitch) >= 0.05f && std::fabs(pitch) >= std::fabs(yaw) * 0.4f) dir = pitch > 0 ? "higher" : "lower";
		if (std::fabs(yaw) >= 0.05f && std::fabs(yaw) >= std::fabs(pitch) * 0.4f)
			dir += std::string(dir.empty() ? "" : " and ") + (yaw > 0 ? "more to the right" : "more to the left");
		return std::string(mag < 0.25f ? "a touch " : mag < 0.5f ? "a bit " : "") + dir;
	};
	auto describe = [&](const Edit& e, std::string& cue, std::string& detail) {
		std::vector<std::string> keyParts, parts, lines;
		// Before the key moment
		if (e.kBoost == 1) { keyParts.push_back("no boost"); lines.push_back(std::format("No boost from {} before {}.", Secs(keyEnd - s), plan.what)); }
		if (e.kBoost == 2) { keyParts.push_back("hold boost"); lines.push_back(std::format("Boost held from {} before {}.", Secs(keyEnd - s), plan.what)); }
		if (e.kBoost == 3) { keyParts.push_back("feather your boost"); lines.push_back(std::format("Boost tapped on and off (every 0.05 s) from {} before {}.", Secs(keyEnd - s), plan.what)); }
		if (e.kBoost == 4) { keyParts.push_back("let go of boost just before it"); lines.push_back(std::format("No boost for the last 0.25 s before {}.", plan.what)); }
		if (e.kAimPitch != 0 || e.kAimYaw != 0)
		{
			keyParts.push_back("nose " + noseWords(e.kAimPitch, e.kAimYaw));
			lines.push_back(std::format("{} for the last {} before {}.", stickWords(e.kAimPitch, e.kAimYaw), Secs(std::min(e.kAimTicks, keyEnd - s)), plan.what));
		}
		if (e.kRoll != 0) { keyParts.push_back(std::format("air roll {}", e.kRoll < 0 ? "left" : "right")); lines.push_back(std::format("Air roll {} held from {} before {}.", e.kRoll < 0 ? "left" : "right", Secs(keyEnd - s), plan.what)); }
		if (e.kSteer != 0) { keyParts.push_back(std::format("steer a bit more {}", e.kSteer > 0 ? "right" : "left")); lines.push_back(std::format("{:.0f}% more steering {} on the ground before {}.", std::fabs(e.kSteer) * 100.0f, e.kSteer > 0 ? "right" : "left", plan.what)); }

		// At the shot
		if (e.flipAngle != kKeep)
		{
			bool right = false;
			FlipKind kind = kindOf(e.flipAngle, right);
			parts.push_back(std::format("use a {} instead", flipLabel(kind, right)));
			lines.push_back(std::format("Flip with the stick {} degrees {} of straight ahead: a {} instead of your {}.",
				std::abs(e.flipAngle), e.flipAngle >= 0 ? "right" : "left", flipLabel(kind, right), flipLabel(realKind, realRight)));
		}
		if (e.flipShift != 0)
		{
			parts.push_back(std::format("{} {}{}", flipWord, std::abs(e.flipShift) <= 8 ? "a little " : "", e.flipShift < 0 ? "earlier" : "later"));
			lines.push_back(std::format("The {} {} {}.", flipWord, Secs(std::abs(e.flipShift)), e.flipShift < 0 ? "earlier" : "later"));
		}
		if (e.flipInto > 0)
		{
			const char* dir = e.flipIntoAngle == 0 ? "front flip" : (e.flipIntoAngle > 0 ? "diagonal flip to the right" : "diagonal flip to the left");
			parts.push_back(std::format("flip into the touch ({})", dir));
			lines.push_back(std::format("A {} {} before the touch.", dir, Secs(e.flipInto)));
		}
		if (e.jumpInto > 0)
		{
			if (e.jumpFlipAngle == kKeep)
			{
				parts.push_back("jump into it");
				lines.push_back(std::format("Jump {} before the touch and hold it.", Secs(e.jumpInto)));
			}
			else
			{
				const char* dir = std::abs(e.jumpFlipAngle) < 25 ? "front flip" : (e.jumpFlipAngle > 0 ? "diagonal flip to the right" : "diagonal flip to the left");
				parts.push_back(std::format("jump and {} into it", dir));
				lines.push_back(std::format("Jump {} before the touch, then a {} right after.", Secs(e.jumpInto), dir));
			}
		}
		if (e.steer != 0)
		{
			parts.push_back(std::format("steer {}more {}", std::fabs(e.steer) < 0.25f ? "a touch " : std::fabs(e.steer) < 0.5f ? "a bit " : "", e.steer > 0 ? "right" : "left"));
			lines.push_back(std::format("{:.0f}% more steering {} for the last {} before the touch.", std::fabs(e.steer) * 100.0f, e.steer > 0 ? "right" : "left", Secs(std::min(e.steerTicks, k - shotLo))));
		}
		if (e.aimPitch != 0 || e.aimYaw != 0)
		{
			parts.push_back(std::format("aim your nose {}{}", noseWords(e.aimPitch, e.aimYaw), F >= 0 ? " before the flip" : ""));
			lines.push_back(std::format("{} for the last {} before the {}.", stickWords(e.aimPitch, e.aimYaw), Secs(std::min(e.aimTicks, end - shotLo)), F >= 0 ? flipWord : "touch"));
		}
		if (e.roll != 0)
		{
			parts.push_back(std::format("air roll {} on the way in", e.roll < 0 ? "left" : "right"));
			lines.push_back(std::format("Air roll {} held for the {} before the {}.", e.roll < 0 ? "left" : "right", Secs(end - shotLo), F >= 0 ? flipWord : "touch"));
		}
		if (e.boost == 1) { parts.push_back("stop boosting on the way in"); lines.push_back(std::format("No boost for the last {} before the touch.", Secs(k - shotLo))); }
		if (e.boost == 2) { parts.push_back("keep boosting into the touch"); lines.push_back(std::format("Boost held for the last {} before the touch.", Secs(k - shotLo))); }
		if (e.boost == 3) { parts.push_back("let go of boost just before the touch"); lines.push_back("No boost for the last 0.20 s before the touch."); }

		auto join = [](const std::vector<std::string>& v) {
			std::string s;
			for (size_t i = 0; i < v.size(); i++) s += (i == 0 ? "" : (i + 1 == v.size() ? " and " : ", ")) + v[i];
			return s;
		};
		cue.clear();
		if (!keyParts.empty()) cue = std::format("Into {}: {}", plan.what, join(keyParts));
		if (!parts.empty()) cue += (cue.empty() ? "" : "; then ") + join(parts);
		if (keyParts.empty() && keyMode && !parts.empty()) cue = "Same way into " + plan.what + ", then " + join(parts);
		if (!cue.empty()) cue[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cue[0])));
		detail.clear();
		for (const std::string& l : lines) detail += l + "\n";
	};

	// ---- The starting set: one change at a time (all small, all things you could actually do) ----
	std::vector<Edit> shotSeeds, keySeeds;
	if (keyMode)
	{
		for (int b : { 1, 2, 3, 4 }) { Edit e; e.kBoost = b; keySeeds.push_back(e); }
		if (keyAirborne)
		{
			for (int d = 0; d < 4; d++)
				for (float amt : { 0.3f, 0.6f })
					for (int ticks : { 24, 60 })
					{
						Edit e; e.kAimTicks = ticks;
						if (d == 0) e.kAimPitch = amt;
						if (d == 1) e.kAimPitch = -amt;
						if (d == 2) e.kAimYaw = -amt;
						if (d == 3) e.kAimYaw = amt;
						keySeeds.push_back(e);
					}
			for (int r : { -1, 1 }) { Edit e; e.kRoll = r; keySeeds.push_back(e); }
		}
		if (keyGround)
			for (float st : { -0.6f, -0.3f, 0.3f, 0.6f }) { Edit e; e.kSteer = st; keySeeds.push_back(e); }
	}
	{
		if (F >= 0)
		{
			for (int dt : { -18, -12, -8, -4, 4, 8, 12, 18 }) { Edit e; e.flipShift = dt; shotSeeds.push_back(e); }
			for (int deg : { 0, 45, -45, 90, -90, 135, -135, 180 })
				for (int dt : { 0, -6, 6 })
				{
					bool right = false;
					FlipKind kind = kindOf(deg, right);
					if (kind == realKind && (right == realRight || kind == FlipKind::Front || kind == FlipKind::Back) && dt == 0) continue;
					Edit e; e.flipAngle = deg; e.flipShift = dt; shotSeeds.push_back(e);
				}
		}
		else if (!groundShot)
		{
			for (int lead : { 4, 8, 12, 18 })
				for (int deg : { 0, 45, -45 }) { Edit e; e.flipInto = lead; e.flipIntoAngle = deg; shotSeeds.push_back(e); }
		}
		if (groundShot)
		{
			for (float st : { -0.6f, -0.3f, 0.3f, 0.6f })
				for (int ticks : { 24, 48 }) { Edit e; e.steer = st; e.steerTicks = ticks; shotSeeds.push_back(e); }
			if (F < 0)
			{
				for (int lead : { 6, 12, 18, 24 }) { Edit e; e.jumpInto = lead; shotSeeds.push_back(e); }
				for (int lead : { 18, 24, 30 })
					for (int deg : { 0, 45, -45 }) { Edit e; e.jumpInto = lead; e.jumpFlipAngle = deg; shotSeeds.push_back(e); }
			}
		}
		if (airborne)
			for (int d = 0; d < 4; d++)
				for (float amt : { 0.4f, 0.8f })
				{
					Edit e; e.aimTicks = 30;
					if (d == 0) e.aimPitch = amt;
					if (d == 1) e.aimPitch = -amt;
					if (d == 2) e.aimYaw = -amt;
					if (d == 3) e.aimYaw = amt;
					shotSeeds.push_back(e);
				}
		bool anyBoost = false, anyCoast = false, lateBoost = false;
		for (int i = shotLo; i < k; i++) { anyBoost |= realCtl[i].boost; anyCoast |= !realCtl[i].boost; }
		for (int i = std::max(shotLo, k - 24); i < k; i++) lateBoost |= realCtl[i].boost;
		if (anyBoost) { Edit e; e.boost = 1; shotSeeds.push_back(e); }
		if (anyCoast && fr[std::min(shotLo, n - 1)].boostAmount > 5.0f) { Edit e; e.boost = 2; shotSeeds.push_back(e); }
		if (lateBoost && k - 24 > shotLo) { Edit e; e.boost = 3; shotSeeds.push_back(e); }
		if (airborne) for (int r : { -1, 1 }) { Edit e; e.roll = r; shotSeeds.push_back(e); }
	}
	const std::vector<Edit>& seeds = keyMode ? keySeeds : shotSeeds;

	// ---- Try them, then keep refining the most promising ones (two changes together, finer amounts) ----
	struct Tried { Edit e; SimRun run; float q; };
	std::vector<Tried> tried;
	const int maxParts = depth >= 2 ? 3 : 2;
	auto seen = [&](const Edit& e) { for (const Tried& t : tried) if (t.e.Same(e)) return true; return false; };
	auto tryEdit = [&](const Edit& e) {
		if (e.Empty() || seen(e) || e.Parts() > maxParts) return;
		if (keyMode && e.KeyParts() == 0) return;   // stage 2 is about what you did before the key moment
		Inputs in;
		if (!build(e, in)) return;
		SimRun r = runEdit(in, 0);
		out.tries++;
		float q = Quality(r) - e.Cost() * 40.0f;
		r.frames.clear();   // keep memory down; the winner is re-run
		r.frames.shrink_to_fit();
		tried.push_back({ e, std::move(r), q });
	};
	for (const Edit& e : seeds) { if (stop()) break; tryEdit(e); }
	if (keyMode)
	{
		// A better way into the key moment often only pays off with a different shot after it (your real shot
		// inputs were made for the real reset / tap / touch). So: the most promising ways in, each with every
		// single shot change.
		std::vector<int> order;
		for (int i = 0; i < static_cast<int>(tried.size()); i++) if (!tried[i].run.keyMissed) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		int topKeys = depth >= 2 ? 16 : 5;
		std::vector<Edit> ways;
		for (int i = 0; i < static_cast<int>(order.size()) && static_cast<int>(ways.size()) < topKeys; i++) ways.push_back(tried[order[i]].e);
		for (const Edit& w : ways)
			for (const Edit& se : shotSeeds)
			{
				if (stop()) break;
				Edit e = w;
				e.flipShift = se.flipShift; e.flipAngle = se.flipAngle; e.aimPitch = se.aimPitch; e.aimYaw = se.aimYaw; e.aimTicks = se.aimTicks;
				e.boost = se.boost; e.roll = se.roll; e.flipInto = se.flipInto; e.flipIntoAngle = se.flipIntoAngle;
				e.steer = se.steer; e.steerTicks = se.steerTicks; e.jumpInto = se.jumpInto; e.jumpFlipAngle = se.jumpFlipAngle;
				tryEdit(e);
			}
	}

	const int budget = depth >= 2 ? 3000 : (keyMode ? 420 : 260);   // simulator runs (each about 2-4 ms)
	const int shiftMax = depth >= 2 ? 42 : 30;    // how far the flip can move (ticks)
	const float aimMax = depth >= 2 ? 1.0f : 0.8f;
	std::mt19937 rng(12345u + static_cast<unsigned>(n));
	auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	auto mutateShot = [&](Edit& e) {
		int what = pick(0, 7);
		if (what == 0 && F >= 0) e.flipShift = std::clamp(e.flipShift + pick(-6, 6), -shiftMax, shiftMax);
		else if (what == 1 && F >= 0) e.flipAngle = (e.flipAngle == kKeep ? (pick(0, 7) - 3) * 45 : e.flipAngle) + pick(-30, 30);
		else if (what == 1 && F < 0 && !groundShot) { e.flipInto = pick(3, 20); e.flipIntoAngle = pick(-60, 60); }
		else if (what == 2 && airborne) { e.aimPitch = std::clamp(e.aimPitch + pick(-4, 4) * 0.1f, -aimMax, aimMax); e.aimTicks = pick(1, depth >= 2 ? 6 : 4) * 12; }
		else if (what == 3 && airborne) { e.aimYaw = std::clamp(e.aimYaw + pick(-4, 4) * 0.1f, -aimMax, aimMax); e.aimTicks = pick(1, depth >= 2 ? 6 : 4) * 12; }
		else if (what == 4) e.boost = pick(0, 3);
		else if (what == 5 && airborne) e.roll = pick(-1, 1);
		else if (what == 6 && groundShot) { e.steer = std::clamp(e.steer + pick(-3, 3) * 0.1f, -aimMax, aimMax); e.steerTicks = pick(1, 5) * 12; }
		else if (what == 7 && groundShot && F < 0)
		{
			e.jumpInto = pick(0, 1) ? 0 : pick(4, 36);
			e.jumpFlipAngle = pick(0, 1) ? kKeep : pick(-60, 60);
		}
		if (e.flipAngle != kKeep) { while (e.flipAngle > 180) e.flipAngle -= 360; while (e.flipAngle < -180) e.flipAngle += 360; }
	};
	auto mutateKey = [&](Edit& e) {
		int what = pick(0, 3);
		if (what == 0) e.kBoost = pick(0, 4);
		else if (what == 1 && keyAirborne)
		{
			if (pick(0, 1)) e.kAimPitch = std::clamp(e.kAimPitch + pick(-3, 3) * 0.1f, -aimMax, aimMax);
			else e.kAimYaw = std::clamp(e.kAimYaw + pick(-3, 3) * 0.1f, -aimMax, aimMax);
			e.kAimTicks = pick(1, 6) * 12;
		}
		else if (what == 2 && keyAirborne) e.kRoll = pick(-1, 1);
		else if (what == 3 && keyGround) e.kSteer = std::clamp(e.kSteer + pick(-3, 3) * 0.1f, -0.8f, 0.8f);
	};
	auto mutate = [&](Edit e) {
		int changes = pick(1, 2);
		for (int c = 0; c < changes; c++)
		{
			// Stage 2 mostly changes the way in, sometimes the shot after it too
			if (keyMode && pick(0, 1) != 0) mutateKey(e);
			else mutateShot(e);
		}
		return e;
	};
	int guard = 0;
	while (out.tries < budget && !stop() && guard++ < budget * 4)
	{
		// Parents: the best few so far (by result, minus how big a change they are)
		std::vector<int> order(tried.size());
		for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
		size_t top = std::min<size_t>(6, order.size());
		std::partial_sort(order.begin(), order.begin() + top, order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		Edit parent = order.empty() ? Edit{} : tried[order[pick(0, static_cast<int>(top) - 1)]].e;
		// Sometimes cross two good ones: the changes from one, plus one change from the other
		if (order.size() >= 2 && pick(0, 3) == 0)
		{
			const Edit& other = tried[order[pick(0, static_cast<int>(top) - 1)]].e;
			if (other.boost) parent.boost = other.boost;
			if (other.roll) parent.roll = other.roll;
			if (other.aimPitch != 0 || other.aimYaw != 0) { parent.aimPitch = other.aimPitch; parent.aimYaw = other.aimYaw; parent.aimTicks = other.aimTicks; }
			if (other.steer != 0) { parent.steer = other.steer; parent.steerTicks = other.steerTicks; }
			if (other.kBoost) parent.kBoost = other.kBoost;
			if (other.kRoll) parent.kRoll = other.kRoll;
			if (other.kAimPitch != 0 || other.kAimYaw != 0) { parent.kAimPitch = other.kAimPitch; parent.kAimYaw = other.kAimYaw; parent.kAimTicks = other.kAimTicks; }
			tryEdit(parent);
			continue;
		}
		tryEdit(mutate(parent));
	}

	// ---- Pick: a higher grade than both your shot and the sim's replay of it (a miss has to go in),
	// then the smaller change, then the nicer shot. It has to keep working from nudged starts, or it's luck. ----
	const int toBeat = std::max(base.grade.overall, realGrade.overall);
	// ...and it has to be the mechanic you attempted, done the same way (the rulebook's lock): no "it scores if you skip the reset"
	static const std::vector<Frame> kNoFrames;
	auto good = [&](const SimRun& r) { return !r.keyMissed && r.grade.overall > toBeat && (r.wentIn || realIn) && Mechanics::SameMechanic(real, fr, r.shot, kNoFrames, s); };
	std::vector<int> better;
	for (int i = 0; i < static_cast<int>(tried.size()); i++) if (good(tried[i].run)) better.push_back(i);
	std::sort(better.begin(), better.end(), [&](int a, int b) {
		const Tried& x = tried[a]; const Tried& y = tried[b];
		if (x.run.grade.overall != y.run.grade.overall) return x.run.grade.overall > y.run.grade.overall;
		if (std::fabs(x.e.Cost() - y.e.Cost()) > 0.01f) return x.e.Cost() < y.e.Cost();
		return x.q > y.q;
	});
	int checked = 0;
	for (int bi : better)
	{
		if (checked >= (depth >= 2 ? 80 : 12) || cancelled() || (hardStop() && checked >= 3)) break;
		checked++;
		const Tried& t = tried[bi];
		Inputs in;
		build(t.e, in);
		int held = 0;
		for (int j = 1; j <= kNudges; j++)
		{
			// Slightly different start, and your timing a tick or two off (nobody hits the exact tick every time)
			static const int kTiming[kNudges] = { -2, 2, -1, 1, 0, 0 };
			Edit ej = t.e;
			int d = kTiming[j - 1];
			if (ej.flipShift != 0 || ej.flipAngle != kKeep) ej.flipShift += d;
			if (ej.flipInto > 0) ej.flipInto = std::max(2, ej.flipInto + d);
			if (ej.jumpInto > 0) ej.jumpInto = std::max(2, ej.jumpInto + d);
			Inputs inj;
			if (!build(ej, inj)) inj = in;
			SimRun r = runEdit(inj, 100 + j);
			out.tries++;
			held += good(r);
			if (j - held > kNudges - kNudgesNeeded) break;
		}
		if (held < kNudgesNeeded) continue;
		SimRun win = runEdit(in, 0);
		if (!good(win)) continue;   // the line it shows has to pass too, not just its nudged copies
		std::string lines;
		describe(t.e, out.cue, lines);
		out.status = WhatIfStatus::Found;
		out.grade = win.grade.overall;
		out.wentIn = win.wentIn;
		out.gradeWhy = win.grade.overallWhy;
		out.held = held;
		out.heldOf = kNudges;
		out.eventTick = win.eventTick;
		out.detail = std::format("{}Would have been a {} ({}).\nSimulated from {} before {} with your own inputs, changing only what is listed above{}. "
			"It worked from {} of {} slightly different starts (with your timing a tick or two off); your own inputs gave your result {} of 7 times.\nYour own inputs in the sim: a {} (your real shot: {}).",
			lines, out.grade, out.gradeWhy, Secs((keyMode ? plan.keyReal : k) - s), keyMode ? plan.what : "your touch",
			keyMode ? std::format(" (the rest of your inputs replayed in step with when {} happened)", plan.what) : "",
			held, kNudges, baseMatches, base.grade.overall, realGrade.overall);
		out.frames = std::move(win.frames);
		break;
	}
	// Stage 2 with no goal found: a way into the key moment that fixes what the coach flagged is still worth showing
	if (out.status != WhatIfStatus::Found && keyMode && fixChecks && !fixChecks->empty() && !hardStop())
	{
		Coach::Values realV = Coach::Measure(fr, real);
		std::vector<int> ways;
		for (int i = 0; i < static_cast<int>(tried.size()); i++)
			if (tried[i].e.ShotParts() == 0 && !tried[i].run.keyMissed) ways.push_back(i);
		std::sort(ways.begin(), ways.end(), [&](int a, int b) {
			if (std::fabs(tried[a].e.Cost() - tried[b].e.Cost()) > 0.01f) return tried[a].e.Cost() < tried[b].e.Cost();
			return tried[a].q > tried[b].q; });
		auto fixesAll = [&](const SimRun& r, Coach::Values& v) {
			if (r.keyMissed || !Mechanics::SameMechanic(real, fr, r.shot, kNoFrames, s)) return false;
			v = Coach::Measure(r.frames, r.shot);
			for (int c : *fixChecks) if (v[c] < 0 || Coach::Fires(c, v[c])) return false;
			return true;
		};
		int looked = 0;
		for (int wi : ways)
		{
			if (looked++ >= (depth >= 2 ? 60 : 30) || hardStop()) break;
			Inputs in;
			build(tried[wi].e, in);
			SimRun r = runEdit(in, 0);
			out.tries++;
			Coach::Values v;
			if (!fixesAll(r, v)) continue;
			int held = 0;
			for (int j = 1; j <= kNudges; j++)
			{
				SimRun rj = runEdit(in, 200 + j);
				out.tries++;
				Coach::Values vj;
				held += fixesAll(rj, vj);
				if (j - held > kNudges - 4) break;
			}
			if (held < 4) continue;
			std::string lines;
			describe(tried[wi].e, out.cue, lines);
			out.status = WhatIfStatus::CleanerWayIn;
			out.grade = r.grade.overall;
			out.wentIn = r.wentIn;
			out.gradeWhy = r.grade.overallWhy;
			out.held = held;
			out.heldOf = kNudges;
			out.eventTick = r.eventTick;
			for (int c : *fixChecks)
				out.fixes += std::format("{}{} -> {}", out.fixes.empty() ? "" : "\n", Coach::DetailOf(c, realV[c]), Coach::DetailOf(c, v[c]));
			out.detail = std::format("{}{}\nWith your own shot inputs after it, it's still a {} ({}): the shot itself needs work too (try the What if? on it).\n"
				"It kept the fix from {} of {} slightly different starts.",
				lines, out.fixes, out.grade, out.gradeWhy, held, kNudges);
			out.frames = std::move(r.frames);
			break;
		}
	}
	if (out.status != WhatIfStatus::Found && out.status != WhatIfStatus::CleanerWayIn)
		out.status = (cancel && cancel->load()) ? WhatIfStatus::Cancelled : WhatIfStatus::NothingBetter;
	int lost = 0;
	for (const Tried& t : tried) lost += t.run.keyMissed;
	if (keyMode) out.note += std::format("{} of the changes lost {}.\n", lost, plan.what);
	out.note += std::format("{} tries in {:.0f} ms.", out.tries, elapsed() * 1000.0f);
	out.ms = elapsed() * 1000.0f;
	return out;
}
