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

	struct Candidate
	{
		std::string cue, detail;
		int cost = 1;   // how big a change it is (smaller wins ties)
		std::vector<CarControls> ctl;
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

	struct Plan { int s = -1, k = -1, flip = -1; };

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
	case WhatIfStatus::CantSimulate:  return "Can't simulate this one closely enough to say: replaying your own inputs didn't give the same result.";
	case WhatIfStatus::NothingBetter: return "No small change at the shot does better. What decided it happened earlier.";
	case WhatIfStatus::Cancelled:     return "Stopped.";
	default:                          return "";
	}
}

WhatIfResult WhatIf::Search(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds)
{
	auto t0 = std::chrono::steady_clock::now();
	auto elapsed = [&]() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(); };
	auto stop = [&]() { return (cancel && cancel->load()) || elapsed() > maxSeconds; };

	WhatIfResult out;
	const int n = static_cast<int>(fr.size());
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	Plan plan = MakePlan(fr, real);
	if (plan.s < 0) { out.status = WhatIfStatus::NoShot; return out; }
	const int s = plan.s, k = plan.k, F = plan.flip;
	out.startTick = s;

	RepSegments realSegs = Segmenter::Split(fr, eventTick >= 0 ? eventTick : n - 1);
	RepGrade realGrade = Grader::GradeRep(fr, realSegs, real, realIn, floorBeforeGoal, ref);
	out.realGrade = realGrade.overall;
	if (realGrade.overall >= 5) { out.status = WhatIfStatus::NotNeeded; return out; }

	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	Car* car = arena->AddCar(Team::BLUE, Body(carBody));

	std::vector<CarControls> realCtl(n);
	for (int i = 0; i < n; i++) realCtl[i] = ControlsOf(fr[i]);
	const int maxTick = std::min(n + 600, k + 720);

	// Gate: your own inputs replayed in the sim have to give the same result (and about the same grade),
	// from the exact start and from slightly nudged ones. Otherwise the sim can't be trusted on this rep.
	SimRun base = Run(arena.get(), car, fr, s, realCtl, maxTick, ref, 0);
	out.simGrade = base.grade.overall;
	out.tries = 1;
	int same = 0;
	for (int j = 1; j <= kNudges; j++)
	{
		SimRun b = Run(arena.get(), car, fr, s, realCtl, maxTick, ref, j);
		same += b.wentIn == realIn && std::abs(b.grade.overall - realGrade.overall) <= 1;
		out.tries++;
	}
	if (base.wentIn != realIn || std::abs(base.grade.overall - realGrade.overall) > 1 || same < kNudgesNeeded)
	{
		out.status = WhatIfStatus::CantSimulate;
		out.ms = elapsed() * 1000.0f;
		return out;
	}

	// ---- The changes to try (all small, all things you could actually do) ----
	std::vector<Candidate> cands;
	const bool flick = real.dribble;
	const char* flipWord = flick ? "flick" : "flip";
	auto shifted = [&](int at, int dt) {
		// Everything from the flip on happens dt ticks later (or earlier); a later flip holds the inputs from just before it
		std::vector<CarControls> c = realCtl;
		for (int i = std::min(at, at + dt); i < n; i++)
		{
			if (i >= at + dt) { int src = i - dt; c[i] = src < n ? realCtl[src] : CarControls(); }
			else c[i] = realCtl[at - 1];
		}
		return c;
	};
	auto stickFor = [](int deg, float& pitch, float& yaw) {
		float r = deg * 3.14159265f / 180.0f;
		pitch = -std::cos(r);   // stick forward = front flip
		yaw = std::sin(r);      // + = to your right
	};

	if (F >= 0)
	{
		// Same flip, different timing
		for (int dt : { -18, -12, -8, -4, 4, 8, 12, 18 })
		{
			if (F + dt <= s + 1 || F + dt >= n - 1) continue;
			std::vector<CarControls> c = shifted(F, dt);
			if (c[F + dt - 1].jump) continue;   // still holding the first jump there: no new press, no flip
			bool little = std::abs(dt) <= 8;
			cands.push_back({ std::format("{} {}{}", flick ? "Flick" : "Flip", little ? "a little " : "", dt < 0 ? "earlier" : "later"),
				std::format("The same {}, {} {}.", flipWord, Secs(std::abs(dt)), dt < 0 ? "earlier" : "later"),
				1 + std::abs(dt) / 8, std::move(c) });
		}
		// A different flip direction (and a touch earlier or later)
		bool realRight = false;
		FlipKind realKind = ShotGeometry::ClassifyFlip(fr[F], realRight);
		for (int deg : { 0, 45, -45, 90, -90, 135, -135, 180 })
			for (int dt : { 0, -6, 6 })
			{
				float p, y;
				stickFor(deg, p, y);
				Frame probe; probe.pitch = p; probe.yaw = y;
				bool right = false;
				FlipKind kind = ShotGeometry::ClassifyFlip(probe, right);
				if (kind == realKind && (right == realRight || kind == FlipKind::Front || kind == FlipKind::Back)) continue;
				int at = F + dt;
				if (at <= s + 1 || at >= n - 1) continue;
				std::vector<CarControls> c = dt ? shifted(F, dt) : realCtl;
				if (c[at - 1].jump) continue;
				for (int i = at; i < std::min(n, at + 30); i++) { c[i].pitch = p; c[i].yaw = y; c[i].roll = 0; }
				std::string side = (kind == FlipKind::Front || kind == FlipKind::Back) ? "" : (right ? " (right)" : " (left)");
				std::string name = ShotGeometry::FlipName(kind, flick) + side;
				std::string when = dt == 0 ? "" : (dt < 0 ? ", a little earlier" : ", a little later");
				cands.push_back({ std::format("Use a {} instead{}", name, when),
					std::format("A {} instead of your {}{}{}.", name, ShotGeometry::FlipName(realKind, flick),
						(realKind == FlipKind::Front || realKind == FlipKind::Back) ? "" : (realRight ? " (right)" : " (left)"),
						dt == 0 ? ", at the same moment" : std::format(", {} {}", Secs(std::abs(dt)), dt < 0 ? "earlier" : "later")),
					2 + (dt ? 1 : 0), std::move(c) });
			}
	}
	else
	{
		// No flip used: flip into the touch
		for (int lead : { 4, 8, 12, 18 })
			for (int deg : { 0, 45, -45 })
			{
				int at = k - lead;
				if (at <= s + 1 || !fr[at].hasFlip || ShotGeometry::OnSurface(fr[at])) continue;
				std::vector<CarControls> c = realCtl;
				if (c[at - 1].jump) continue;
				float p, y;
				stickFor(deg, p, y);
				for (int i = at; i < std::min(n, at + 30); i++) { c[i].pitch = p; c[i].yaw = y; c[i].roll = 0; c[i].jump = false; }
				c[at].jump = true;
				if (at + 1 < n) c[at + 1].jump = true;
				const char* dir = deg == 0 ? "front flip" : (deg > 0 ? "diagonal flip to the right" : "diagonal flip to the left");
				cands.push_back({ std::format("Flip into the touch ({})", dir),
					std::format("A {} {} before the touch.", dir, Secs(lead)), 3, std::move(c) });
			}
		// Aim: nose a little up, down, left or right over the last moments before the hit (in the air only)
		int aimFrom = std::max(s, k - 30);
		bool airborne = !ShotGeometry::OnSurface(fr[aimFrom]) && !ShotGeometry::OnSurface(fr[k]);
		if (airborne)
		{
			const char* names[4] = { "higher", "lower", "more to the left", "more to the right" };
			for (int d = 0; d < 4; d++)
				for (float amt : { 0.4f, 0.8f })
				{
					std::vector<CarControls> c = realCtl;
					for (int i = aimFrom; i < k; i++)
					{
						if (d == 0) c[i].pitch = std::clamp(c[i].pitch + amt, -1.0f, 1.0f);
						if (d == 1) c[i].pitch = std::clamp(c[i].pitch - amt, -1.0f, 1.0f);
						if (d == 2) c[i].yaw = std::clamp(c[i].yaw - amt, -1.0f, 1.0f);
						if (d == 3) c[i].yaw = std::clamp(c[i].yaw + amt, -1.0f, 1.0f);
					}
					cands.push_back({ std::format("Aim your nose {}{}", amt < 0.5f ? "a bit " : "", names[d]),
						std::format("About {:.0f}% more stick {} for the last {} before the touch.", amt * 100.0f,
							d == 0 ? "back (nose up)" : d == 1 ? "forward (nose down)" : d == 2 ? "left" : "right", Secs(k - aimFrom)),
						amt < 0.5f ? 1 : 2, std::move(c) });
				}
		}
	}

	// Boost
	{
		std::vector<CarControls> off = realCtl, on = realCtl, late = realCtl;
		bool anyBoost = false, anyCoast = false;
		for (int i = s; i < k; i++) { anyBoost |= realCtl[i].boost; anyCoast |= !realCtl[i].boost; off[i].boost = false; on[i].boost = true; }
		for (int i = std::max(s, k - 24); i < k; i++) late[i].boost = false;
		if (anyBoost) cands.push_back({ "Stop boosting on the way in", std::format("No boost for the last {} before the touch.", Secs(k - s)), 1, std::move(off) });
		if (anyCoast && fr[s].boostAmount > 5.0f) cands.push_back({ "Keep boosting into the touch", std::format("Boost held for the last {} before the touch.", Secs(k - s)), 1, std::move(on) });
		bool lateBoost = false;
		for (int i = std::max(s, k - 24); i < k; i++) lateBoost |= realCtl[i].boost;
		if (lateBoost && k - 24 > s) cands.push_back({ "Let go of boost just before the touch", "No boost for the last 0.20 s before the touch.", 1, std::move(late) });
	}
	// Air roll on the way in (up to the flip, or the touch)
	{
		int end = F >= 0 ? F : k;
		if (!ShotGeometry::OnSurface(fr[s]) || !ShotGeometry::OnSurface(fr[end]))
			for (float roll : { -1.0f, 1.0f })
			{
				std::vector<CarControls> c = realCtl;
				for (int i = s; i < end; i++) c[i].roll = roll;
				cands.push_back({ roll < 0 ? "Air roll left on the way in" : "Air roll right on the way in",
					std::format("Air roll {} held for the {} before the {}.", roll < 0 ? "left" : "right", Secs(end - s), F >= 0 ? flipWord : "touch"),
					2, std::move(c) });
			}
	}

	// ---- Try them all ----
	// Better means a higher grade than both your real shot and the sim's replay of it; a miss has to go in.
	const int floor = std::max(base.grade.overall, realGrade.overall);
	struct Scored { int idx; SimRun run; float q; };
	std::vector<Scored> better;
	for (int ci = 0; ci < static_cast<int>(cands.size()); ci++)
	{
		if (stop()) break;
		SimRun r = Run(arena.get(), car, fr, s, cands[ci].ctl, maxTick, ref, 0);
		out.tries++;
		if (r.grade.overall > floor && (r.wentIn || realIn))
		{
			float q = Quality(r);
			better.push_back({ ci, std::move(r), q });
		}
	}
	std::sort(better.begin(), better.end(), [&](const Scored& a, const Scored& b) {
		if (a.run.grade.overall != b.run.grade.overall) return a.run.grade.overall > b.run.grade.overall;
		if (cands[a.idx].cost != cands[b.idx].cost) return cands[a.idx].cost < cands[b.idx].cost;
		return a.q > b.q;
	});

	// The winner has to keep working from slightly nudged starts, or it's luck, not advice
	int checked = 0;
	for (Scored& b : better)
	{
		if (checked++ >= 6 || stop()) break;
		int held = 0;
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun r = Run(arena.get(), car, fr, s, cands[b.idx].ctl, maxTick, ref, 100 + j);
			out.tries++;
			held += r.grade.overall > floor && (r.wentIn || realIn);
		}
		if (held < kNudgesNeeded) continue;
		const Candidate& c = cands[b.idx];
		out.status = WhatIfStatus::Found;
		out.cue = c.cue;
		out.grade = b.run.grade.overall;
		out.wentIn = b.run.wentIn;
		out.gradeWhy = b.run.grade.overallWhy;
		out.held = held;
		out.heldOf = kNudges;
		out.eventTick = b.run.eventTick;
		out.detail = std::format("{}\nWould have been a {} ({}).\nSimulated from {} before your touch with your own inputs, changing only this. "
			"It worked from {} of {} slightly different starts.\nYour own inputs in the sim: a {} (your real shot: {}).",
			c.detail, out.grade, out.gradeWhy, Secs(k - s), held, kNudges, base.grade.overall, realGrade.overall);
		out.frames = std::move(b.run.frames);
		break;
	}
	if (out.status != WhatIfStatus::Found)
		out.status = (cancel && cancel->load()) ? WhatIfStatus::Cancelled : WhatIfStatus::NothingBetter;
	out.ms = elapsed() * 1000.0f;
	return out;
}
