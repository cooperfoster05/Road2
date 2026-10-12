// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "WhatIf.h"
#include "Grader.h"
#include "Segmenter.h"
#include "ShotGeometry.h"

#include "third_party/RocketSim/src/RocketSim.h"

#include <algorithm>
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

	struct SimRun
	{
		std::vector<Frame> frames;   // the real rep up to the start, then the simulation
		bool wentIn = false, floorFirst = false;
		int  eventTick = -1;
		ShotAnalysis shot;
		RepGrade grade;
	};

	// Runs the sim from tick s with the given controls (indexed by tick) and grades the result.
	// nudge > 0 moves the start by a couple of units (a real attempt never starts from exactly the same spot).
	SimRun Run(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const std::vector<CarControls>& ctl,
		int maxTick, const ShotReference& ref, int nudge)
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

		int lastTouch = -1, stopAt = -1;
		bool bounced = false;
		for (int i = s; i < maxTick; i++)
		{
			CarControls c = i < static_cast<int>(ctl.size()) ? ctl[i] : CarControls();
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
			r.frames.push_back(g);

			// After the shot ends, keep a half second for the recovery part of the breakdown
			if (stopAt >= 0) { if (i + 1 >= stopAt) break; continue; }
			auto end = [&]() { r.eventTick = i + 1; stopAt = i + 1 + 60; };
			if (std::fabs(bs.pos.y) > 5215.0f && std::fabs(bs.pos.x) < 893.0f && bs.pos.z < 643.0f)
			{
				r.wentIn = true;
				r.floorFirst = bounced;
				end();
				continue;
			}
			if (lastTouch >= 0 && bs.pos.z < 100.0f && bs.vel.z > 0.0f && i + 1 - lastTouch > 10 && !ShotGeometry::BallWithCar(g))
			{
				// Off the floor and away from you: over, unless it's rolling in (the recorder waits for those too)
				bounced = true;
				bool rollingIn = std::fabs(bs.pos.x) < 1100.0f && std::fabs(bs.vel.y) > 300.0f &&
					((bs.vel.y > 0 && bs.pos.y > 2500.0f) || (bs.vel.y < 0 && bs.pos.y < -2500.0f));
				if (!rollingIn) { end(); continue; }
			}
			if (lastTouch >= 0 && i + 1 - lastTouch > 480) end();
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
		if (r.wentIn) q += 5000.0f - std::min(5000.0f, r.shot.cornerDist) + r.shot.shotSpeed * 0.5f;
		else q -= r.shot.reachesGoalLine ? std::min(5000.0f, r.shot.missDist) : 5000.0f;
		return q;
	}

	// One "what if": a few small changes to your own inputs, all optional. Zero / kKeep = do what you did.
	constexpr int kKeep = 9999;
	struct Edit
	{
		int   flipShift = 0;       // flip shots: the flip (and everything after it) this many ticks later (- = earlier)
		int   flipAngle = kKeep;   // flip shots: stick direction for the flip, degrees from straight forward (+ = right)
		float aimPitch = 0;        // extra stick back (+, nose up) / forward (-) just before the flip or touch
		float aimYaw = 0;          // extra stick right (+) / left (-)
		int   aimTicks = 0;        // over how long
		int   boost = 0;           // 0 yours, 1 none on the way in, 2 held the whole way, 3 let go for the last 0.2 s
		int   roll = 0;            // 0 yours, -1 air roll left, +1 right (on the way in)
		int   flipInto = 0;        // no-flip shots: flip this many ticks before the touch (0 = don't)
		int   flipIntoAngle = 0;

		bool Same(const Edit& o) const
		{
			return flipShift == o.flipShift && flipAngle == o.flipAngle && std::fabs(aimPitch - o.aimPitch) < 0.01f &&
				std::fabs(aimYaw - o.aimYaw) < 0.01f && (aimTicks == o.aimTicks || (aimPitch == 0 && aimYaw == 0)) &&
				boost == o.boost && roll == o.roll && flipInto == o.flipInto && (flipInto == 0 || flipIntoAngle == o.flipIntoAngle);
		}
		bool Empty() const { return Same(Edit{}); }
		int Parts() const
		{
			return (flipShift != 0) + (flipAngle != kKeep) + (aimPitch != 0 || aimYaw != 0) + (boost != 0) + (roll != 0) + (flipInto != 0);
		}
		// How big a change it is: smaller wins ties
		float Cost() const
		{
			float c = std::abs(flipShift) / 8.0f + (flipAngle != kKeep ? 1.5f : 0.0f) + (std::fabs(aimPitch) + std::fabs(aimYaw)) * 1.5f +
				(boost ? 1.0f : 0.0f) + (roll ? 1.5f : 0.0f) + (flipInto ? 2.0f : 0.0f);
			return c;
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

	struct Plan { int s = -1, k = -1, flip = -1, closer = -1; };

	Plan MakePlan(const std::vector<Frame>& fr, const ShotAnalysis& shot)
	{
		Plan p;
		if (!shot.valid) return p;
		p.k = shot.touchTick;
		p.flip = shot.flipTick >= 0 ? shot.flipTick : FindFlip(fr, p.k - 60, p.k);
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
		return p;
	}
}

int WhatIf::StartTick(const std::vector<Frame>& frames)
{
	return MakePlan(frames, ShotGeometry::Analyze(frames)).s;
}

const char* WhatIf::StatusText(WhatIfStatus s)
{
	switch (s)
	{
	case WhatIfStatus::NotNeeded:     return "Already a 5: nothing to improve on here.";
	case WhatIfStatus::NoShot:        return "No shot touch to work from.";
	case WhatIfStatus::CantSimulate:  return "The simulator can't follow this one closely enough to say (replaying your own inputs didn't give your result).";
	case WhatIfStatus::NothingBetter: return "No small change at the shot would have saved it.";
	case WhatIfStatus::Cancelled:     return "Stopped.";
	default:                          return "";
	}
}

WhatIfResult WhatIf::Search(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int depth)
{
	auto t0 = std::chrono::steady_clock::now();
	auto elapsed = [&]() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(); };
	auto stop = [&]() { return (cancel && cancel->load()) || elapsed() > maxSeconds; };

	WhatIfResult out;
	out.depth = depth;
	const int n = static_cast<int>(fr.size());
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	Plan plan = MakePlan(fr, real);
	if (plan.s < 0) { out.status = WhatIfStatus::NoShot; return out; }
	const int k = plan.k, F = plan.flip;

	RepSegments realSegs = Segmenter::Split(fr, eventTick >= 0 ? eventTick : n - 1);
	RepGrade realGrade = Grader::GradeRep(fr, realSegs, real, realIn, floorBeforeGoal, ref);
	out.realGrade = realGrade.overall;
	if (realGrade.overall >= 5) { out.status = WhatIfStatus::NotNeeded; return out; }

	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	Car* car = arena->AddCar(Team::BLUE, Body(carBody));

	std::vector<CarControls> realCtl(n);
	for (int i = 0; i < n; i++) realCtl[i] = ControlsOf(fr[i]);
	const int maxTick = std::min(n + 600, k + 720);
	auto matches = [&](const SimRun& r) { return r.wentIn == realIn && std::abs(r.grade.overall - realGrade.overall) <= 1; };

	// Gate: your own inputs replayed in the sim (from the exact start and from 6 slightly nudged ones) have to give
	// your result at least 3 times out of 7. A shot that only sometimes reproduces was a knife-edge one, and that's
	// fine: a what-if then has to work almost every time, which yours didn't. If the sim can't follow you from the
	// first start, try one closer to the shot.
	int s = -1, baseMatches = 0;
	SimRun base;
	for (int start : { plan.s, plan.closer })
	{
		if (start < 0 || stop()) continue;
		SimRun b0 = Run(arena.get(), car, fr, start, realCtl, maxTick, ref, 0);
		int m = matches(b0) ? 1 : 0;
		out.tries++;
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun b = Run(arena.get(), car, fr, start, realCtl, maxTick, ref, j);
			m += matches(b) ? 1 : 0;
			out.tries++;
		}
		out.note += std::format("Start {} before the touch: your inputs gave your result {} of 7 times.\n", Secs(k - start), m);
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
	const int end = F >= 0 ? F : k;   // the flip, or the touch: aim and roll changes lead up to it

	// ---- Turn an edit into inputs ----
	auto stickFor = [](int deg, float& pitch, float& yaw) {
		float r = deg * 3.14159265f / 180.0f;
		pitch = -std::cos(r);   // stick forward = front flip
		yaw = std::sin(r);      // + = to your right
	};
	auto apply = [&](const Edit& e, std::vector<CarControls>& c) -> bool {
		c = realCtl;
		int flipAt = end;
		if (F >= 0)
		{
			flipAt = F + e.flipShift;
			if (flipAt <= s + 1 || flipAt >= n - 1) return false;
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
			if (at <= s + 1 || !fr[at].hasFlip || ShotGeometry::OnSurface(fr[at]) || c[at - 1].jump) return false;
			float p, y;
			stickFor(e.flipIntoAngle, p, y);
			for (int i = at; i < std::min(n, at + 30); i++) { c[i].pitch = p; c[i].yaw = y; c[i].roll = 0; c[i].jump = false; }
			c[at].jump = true;
			if (at + 1 < n) c[at + 1].jump = true;
			flipAt = at;
		}
		if (e.aimPitch != 0 || e.aimYaw != 0)
			for (int i = std::max(s, flipAt - e.aimTicks); i < flipAt; i++)
			{
				c[i].pitch = std::clamp(c[i].pitch + e.aimPitch, -1.0f, 1.0f);
				c[i].yaw = std::clamp(c[i].yaw + e.aimYaw, -1.0f, 1.0f);
			}
		if (e.roll != 0)
			for (int i = s; i < flipAt; i++) c[i].roll = static_cast<float>(e.roll);
		if (e.boost == 1) for (int i = s; i < k; i++) c[i].boost = false;
		if (e.boost == 2) for (int i = s; i < k; i++) c[i].boost = true;
		if (e.boost == 3) for (int i = std::max(s, k - 24); i < k; i++) c[i].boost = false;
		return true;
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
	auto describe = [&](const Edit& e, std::string& cue, std::string& detail) {
		std::vector<std::string> parts, lines;
		if (e.flipAngle != kKeep)
		{
			float p, y;
			stickFor(e.flipAngle, p, y);
			Frame probe; probe.pitch = p; probe.yaw = y;
			bool right = false;
			FlipKind kind = ShotGeometry::ClassifyFlip(probe, right);
			parts.push_back(std::format("use a {} instead", flipLabel(kind, right)));
			lines.push_back(std::format("Flip with the stick {} degrees {} of straight ahead: a {} instead of your {}.",
				std::abs(e.flipAngle), e.flipAngle >= 0 ? "right" : "left", flipLabel(kind, right), flipLabel(realKind, realRight)));
		}
		if (e.flipShift != 0)
		{
			parts.push_back(std::format("{} {}{}", flick ? "flick" : "flip", std::abs(e.flipShift) <= 8 ? "a little " : "", e.flipShift < 0 ? "earlier" : "later"));
			lines.push_back(std::format("The {} {} {}.", flipWord, Secs(std::abs(e.flipShift)), e.flipShift < 0 ? "earlier" : "later"));
		}
		if (e.flipInto > 0)
		{
			const char* dir = e.flipIntoAngle == 0 ? "front flip" : (e.flipIntoAngle > 0 ? "diagonal flip to the right" : "diagonal flip to the left");
			parts.push_back(std::format("flip into the touch ({})", dir));
			lines.push_back(std::format("A {} {} before the touch.", dir, Secs(e.flipInto)));
		}
		if (e.aimPitch != 0 || e.aimYaw != 0)
		{
			float mag = std::max(std::fabs(e.aimPitch), std::fabs(e.aimYaw));
			std::string dir;
			if (std::fabs(e.aimPitch) >= 0.15f) dir = e.aimPitch > 0 ? "higher" : "lower";
			if (std::fabs(e.aimYaw) >= 0.15f) dir += std::string(dir.empty() ? "" : " and ") + (e.aimYaw > 0 ? "more to the right" : "more to the left");
			if (dir.empty()) dir = "slightly differently";
			parts.push_back(std::format("aim your nose {}{}{}", mag < 0.5f ? "a bit " : "", dir, F >= 0 ? " before the flip" : ""));
			std::string stick;
			if (e.aimPitch != 0) stick = std::format("{:.0f}% more stick {}", std::fabs(e.aimPitch) * 100.0f, e.aimPitch > 0 ? "back (nose up)" : "forward (nose down)");
			if (e.aimYaw != 0) stick += std::format("{}{:.0f}% more stick {}", stick.empty() ? "" : " and ", std::fabs(e.aimYaw) * 100.0f, e.aimYaw > 0 ? "right" : "left");
			stick[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stick[0])));
			lines.push_back(std::format("{} for the last {} before the {}.", stick, Secs(std::min(e.aimTicks, end - s)), F >= 0 ? flipWord : "touch"));
		}
		if (e.roll != 0)
		{
			parts.push_back(std::format("air roll {} on the way in", e.roll < 0 ? "left" : "right"));
			lines.push_back(std::format("Air roll {} held for the {} before the {}.", e.roll < 0 ? "left" : "right", Secs(end - s), F >= 0 ? flipWord : "touch"));
		}
		if (e.boost == 1) { parts.push_back("stop boosting on the way in"); lines.push_back(std::format("No boost for the last {} before the touch.", Secs(k - s))); }
		if (e.boost == 2) { parts.push_back("keep boosting into the touch"); lines.push_back(std::format("Boost held for the last {} before the touch.", Secs(k - s))); }
		if (e.boost == 3) { parts.push_back("let go of boost just before the touch"); lines.push_back("No boost for the last 0.20 s before the touch."); }
		cue.clear();
		for (size_t i = 0; i < parts.size(); i++)
			cue += (i == 0 ? "" : (i + 1 == parts.size() ? ", and " : ", ")) + parts[i];
		if (!cue.empty()) cue[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cue[0])));
		detail.clear();
		for (const std::string& l : lines) detail += l + "\n";
	};

	// ---- The starting set: one change at a time (all small, all things you could actually do) ----
	std::vector<Edit> seeds;
	bool airborne = !ShotGeometry::OnSurface(fr[std::max(s, end - 30)]) || !ShotGeometry::OnSurface(fr[end]);
	if (F >= 0)
	{
		for (int dt : { -18, -12, -8, -4, 4, 8, 12, 18 }) { Edit e; e.flipShift = dt; seeds.push_back(e); }
		for (int deg : { 0, 45, -45, 90, -90, 135, -135, 180 })
			for (int dt : { 0, -6, 6 })
			{
				float p, y;
				stickFor(deg, p, y);
				Frame probe; probe.pitch = p; probe.yaw = y;
				bool right = false;
				FlipKind kind = ShotGeometry::ClassifyFlip(probe, right);
				if (kind == realKind && (right == realRight || kind == FlipKind::Front || kind == FlipKind::Back) && dt == 0) continue;
				Edit e; e.flipAngle = deg; e.flipShift = dt; seeds.push_back(e);
			}
	}
	else
	{
		for (int lead : { 4, 8, 12, 18 })
			for (int deg : { 0, 45, -45 }) { Edit e; e.flipInto = lead; e.flipIntoAngle = deg; seeds.push_back(e); }
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
				seeds.push_back(e);
			}
	{
		bool anyBoost = false, anyCoast = false, lateBoost = false;
		for (int i = s; i < k; i++) { anyBoost |= realCtl[i].boost; anyCoast |= !realCtl[i].boost; }
		for (int i = std::max(s, k - 24); i < k; i++) lateBoost |= realCtl[i].boost;
		if (anyBoost) { Edit e; e.boost = 1; seeds.push_back(e); }
		if (anyCoast && fr[s].boostAmount > 5.0f) { Edit e; e.boost = 2; seeds.push_back(e); }
		if (lateBoost && k - 24 > s) { Edit e; e.boost = 3; seeds.push_back(e); }
	}
	if (airborne) for (int r : { -1, 1 }) { Edit e; e.roll = r; seeds.push_back(e); }

	// ---- Try them, then keep refining the most promising ones (two changes together, finer amounts) ----
	struct Tried { Edit e; SimRun run; float q; };
	std::vector<Tried> tried;
	auto seen = [&](const Edit& e) { for (const Tried& t : tried) if (t.e.Same(e)) return true; return false; };
	auto tryEdit = [&](const Edit& e) {
		if (e.Empty() || seen(e) || e.Parts() > (depth >= 2 ? 3 : 2)) return;
		std::vector<CarControls> c;
		if (!apply(e, c)) return;
		SimRun r = Run(arena.get(), car, fr, s, c, maxTick, ref, 0);
		out.tries++;
		float q = Quality(r) - e.Cost() * 40.0f;
		r.frames.clear();   // keep memory down; the winner is re-run
		r.frames.shrink_to_fit();
		tried.push_back({ e, std::move(r), q });
	};
	for (const Edit& e : seeds) { if (stop()) break; tryEdit(e); }

	const int budget = depth >= 2 ? 1200 : 260;   // simulator runs (each about 2-4 ms)
	std::mt19937 rng(12345u + static_cast<unsigned>(n));
	auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	auto mutate = [&](Edit e) {
		int changes = pick(1, 2);
		for (int c = 0; c < changes; c++)
		{
			int what = pick(0, 5);
			if (what == 0 && F >= 0) e.flipShift = std::clamp(e.flipShift + pick(-6, 6), -30, 30);
			else if (what == 1 && F >= 0) e.flipAngle = (e.flipAngle == kKeep ? (pick(0, 7) - 3) * 45 : e.flipAngle) + pick(-30, 30);
			else if (what == 1 && F < 0) { e.flipInto = pick(3, 20); e.flipIntoAngle = pick(-60, 60); }
			else if (what == 2 && airborne) { e.aimPitch = std::clamp(e.aimPitch + pick(-4, 4) * 0.1f, -0.8f, 0.8f); e.aimTicks = pick(1, 4) * 12; }
			else if (what == 3 && airborne) { e.aimYaw = std::clamp(e.aimYaw + pick(-4, 4) * 0.1f, -0.8f, 0.8f); e.aimTicks = pick(1, 4) * 12; }
			else if (what == 4) e.boost = pick(0, 3);
			else if (what == 5 && airborne) e.roll = pick(-1, 1);
			if (e.flipAngle != kKeep) { while (e.flipAngle > 180) e.flipAngle -= 360; while (e.flipAngle < -180) e.flipAngle += 360; }
		}
		return e;
	};
	int guard = 0;
	while (out.tries < budget && !stop() && guard++ < budget * 4)
	{
		// Parents: the best few so far (by result, minus how big a change they are)
		std::vector<int> order(tried.size());
		for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
		std::partial_sort(order.begin(), order.begin() + std::min<size_t>(6, order.size()), order.end(),
			[&](int a, int b) { return tried[a].q > tried[b].q; });
		Edit parent = order.empty() ? Edit{} : tried[order[pick(0, static_cast<int>(std::min<size_t>(6, order.size())) - 1)]].e;
		// Sometimes cross two good ones: the changes from one, plus one change from the other
		if (order.size() >= 2 && pick(0, 3) == 0)
		{
			const Edit& other = tried[order[pick(0, static_cast<int>(std::min<size_t>(6, order.size())) - 1)]].e;
			if (other.boost) parent.boost = other.boost;
			if (other.roll) parent.roll = other.roll;
			if (other.aimPitch != 0 || other.aimYaw != 0) { parent.aimPitch = other.aimPitch; parent.aimYaw = other.aimYaw; parent.aimTicks = other.aimTicks; }
			tryEdit(parent);
			continue;
		}
		tryEdit(mutate(parent));
	}

	// ---- Pick: a higher grade than both your shot and the sim's replay of it (a miss has to go in),
	// then the smaller change, then the nicer shot. It has to keep working from nudged starts, or it's luck. ----
	const int toBeat = std::max(base.grade.overall, realGrade.overall);
	std::vector<int> better;
	for (int i = 0; i < static_cast<int>(tried.size()); i++)
		if (tried[i].run.grade.overall > toBeat && (tried[i].run.wentIn || realIn)) better.push_back(i);
	std::sort(better.begin(), better.end(), [&](int a, int b) {
		const Tried& x = tried[a]; const Tried& y = tried[b];
		if (x.run.grade.overall != y.run.grade.overall) return x.run.grade.overall > y.run.grade.overall;
		if (std::fabs(x.e.Cost() - y.e.Cost()) > 0.01f) return x.e.Cost() < y.e.Cost();
		return x.q > y.q;
	});
	int checked = 0;
	for (int bi : better)
	{
		if (checked++ >= (depth >= 2 ? 16 : 8) || stop()) break;
		const Tried& t = tried[bi];
		std::vector<CarControls> c;
		apply(t.e, c);
		int held = 0;
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun r = Run(arena.get(), car, fr, s, c, maxTick, ref, 100 + j);
			out.tries++;
			held += r.grade.overall > toBeat && (r.wentIn || realIn);
			if (j - held > kNudges - kNudgesNeeded) break;
		}
		if (held < kNudgesNeeded) continue;
		SimRun win = Run(arena.get(), car, fr, s, c, maxTick, ref, 0);
		std::string lines;
		describe(t.e, out.cue, lines);
		out.status = WhatIfStatus::Found;
		out.grade = win.grade.overall;
		out.wentIn = win.wentIn;
		out.gradeWhy = win.grade.overallWhy;
		out.held = held;
		out.heldOf = kNudges;
		out.eventTick = win.eventTick;
		out.detail = std::format("{}Would have been a {} ({}).\nSimulated from {} before your touch with your own inputs, changing only what is listed above. "
			"It worked from {} of {} slightly different starts; your own inputs gave your result {} of 7 times.\nYour own inputs in the sim: a {} (your real shot: {}).",
			lines, out.grade, out.gradeWhy, Secs(k - s), held, kNudges, baseMatches, base.grade.overall, realGrade.overall);
		out.frames = std::move(win.frames);
		break;
	}
	if (out.status != WhatIfStatus::Found)
		out.status = (cancel && cancel->load()) ? WhatIfStatus::Cancelled : WhatIfStatus::NothingBetter;
	out.note += std::format("{} tries in {:.0f} ms.", out.tries, elapsed() * 1000.0f);
	out.ms = elapsed() * 1000.0f;
	return out;
}
