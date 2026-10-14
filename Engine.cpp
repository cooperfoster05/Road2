// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "Engine.h"
#include "Grader.h"
#include "Mechanics.h"
#include "Segmenter.h"
#include "ShotGeometry.h"
#include "Units.h"

#include "third_party/RocketSim/src/RocketSim.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>

using namespace RocketSim;

namespace
{
	constexpr float kU2R = 3.14159265f / 32768.0f;
	constexpr float kR2U = 32768.0f / 3.14159265f;
	constexpr int   kNudges = 6;
	constexpr float kBallR = 91.25f;

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
			}
			else
			{
				cs.hasJumped = jumpTick >= 0;
				cs.airTimeSinceJump = jumpTick >= 0 ? (s - jumpTick) / 120.0f : 0.0f;
				cs.airTime = cs.airTimeSinceJump;
				if (jumpTick >= 0 && s - jumpTick < 24 && f.jump) { cs.isJumping = true; cs.jumpTime = (s - jumpTick) / 120.0f; }
			}
			// The flip: a flip used (and how long ago, which matters: the game locks your pitch for a while after
			// one) is not the same as a double jump used. Counting every used flip as "just flipped" locked the sim's
			// pitch for the first second of every line started in the air after a double jump.
			cs.hasFlipped = false;
			cs.hasDoubleJumped = false;
			if (!f.hasFlip)
			{
				int second = -1;
				for (int i = s; i > 0; i--)
				{
					if (fr[i].onGround && ShotGeometry::OnSurface(fr[i])) break;
					if (i == jumpTick) break;
					if (fr[i].jump && !fr[i - 1].jump && !fr[i - 1].onGround) { second = i; break; }
				}
				const Frame* j2 = second >= 0 ? &fr[second] : nullptr;
				const bool dodge = j2 && std::fabs(j2->pitch) + std::fabs(j2->yaw) + std::fabs(j2->roll) >= 0.5f;
				if (dodge)
				{
					cs.hasFlipped = true;
					cs.flipTime = (s - second) / 120.0f;
					cs.isFlipping = cs.flipTime < 0.65f;
					float dx = -j2->pitch, dy = j2->yaw + j2->roll;
					float dl = std::sqrt(dx * dx + dy * dy);
					if (dl > 1e-3f && (std::fabs(dy) >= 0.1f || std::fabs(dx) >= 0.1f)) { dx /= dl; dy /= dl; }
					else { dx = 0; dy = 0; }
					cs.flipRelTorque = Vec(-dy, dx, 0);
				}
				else
					cs.hasDoubleJumped = true;   // a double jump, or the flip ran out: either way no flip left, no pitch lock
			}
		}
		car->SetState(cs);
		BallState bs;
		bs.pos = Vec(f.ballX, f.ballY, f.ballZ);
		bs.vel = Vec(f.ballVX, f.ballVY, f.ballVZ);
		bs.angVel = Vec(f.ballAVX, f.ballAVY, f.ballAVZ);
		ball->SetState(bs);
	}

	// Every run starts with every boost pad up: pads a previous run picked up must not change the next one
	void ResetPads(Arena* arena)
	{
		for (BoostPad* p : arena->GetBoostPads()) p->SetState(BoostPadState());
	}

	// The physics keeps a little wheel/suspension state that setting the car's state doesn't reset, so a run that
	// starts on the ground or a wall could depend on the run before it. One tick parked high in the air clears it, so
	// the same inputs from the same start always play out exactly the same.
	void ClearCarInternals(Arena* arena, Car* car)
	{
		CarState cs;
		cs.pos = Vec(0, 0, 1400);
		cs.isOnGround = false;
		car->SetState(cs);
		car->controls = CarControls();
		BallState bs;                       // the ball too (its contacts with walls and the car are remembered)
		bs.pos = Vec(2500, 0, 1400);
		arena->ball->SetState(bs);
		arena->Step(1);
	}

	// ---- The bot: get the car's center to a point at a time, nose along the hit direction ----
	namespace Bot {
	inline float Clamp1(float v) { return std::max(-1.0f, std::min(1.0f, v)); }
	
	// Turn the car toward a forward direction (and an up direction if given). Rates from the probe:
	// pitch +1 = nose up (rate -w.right), yaw +1 = nose right (rate +w.up), roll +1 = top to the right (rate -w.fw).
	inline void Orient(const CarState& s, Vec fwd, Vec up, bool useUp, CarControls& c, float kp = 3.0f, float kd = 0.35f)
	{
		const RotMat& R = s.rotMat;
		float tf = fwd.Dot(R.forward), tr = fwd.Dot(R.right), tu = fwd.Dot(R.up);
		float ePitch = std::atan2(tu, tf), eYaw = std::atan2(tr, tf);
		float pitchRate = -s.angVel.Dot(R.right), yawRate = s.angVel.Dot(R.up), rollRate = -s.angVel.Dot(R.forward);
		c.pitch = Clamp1(kp * ePitch - kd * pitchRate);
		c.yaw = Clamp1(kp * eYaw - kd * yawRate);
		if (useUp)
		{
			float ur = up.Dot(R.right), uu = up.Dot(R.up);
			float eRoll = std::atan2(ur, uu);
			c.roll = Clamp1(1.5f * eRoll - 0.25f * rollRate);
		}
		else c.roll = Clamp1(-0.25f * rollRate);
	}
	
	struct Plan
	{
		Vec target;        // where the car's center should be
		Vec hitDir;        // the direction to send the ball (car nose at contact)
		int arriveTick;    // when
		int jumpTick;      // when to leave the ground (-1 = stay on the ground)
		bool dodge = false;   // dodge into the ball at the end
		int dodgeLead = 8;    // ticks before arrival
		bool doubleJump = true;
		bool power = false;   // drive through it: keep boosting into the hit for a harder shot
	};
	
	struct State { int jumpStarted = -1; bool doubleJumped = false; bool dodged = false; int touched = -1; bool noJump = false; };
	
	inline CarControls Drive(const CarState& s, const Plan& p, int tick, State& st)
	{
		CarControls c;
		const float g = -650.0f;
		int tau = p.arriveTick - tick;
		float T = std::max(1, tau) / 120.0f;
		if (st.touched >= 0 && tick >= st.touched) return c;   // after the touch: hands off
	
		// Back on the ground after a jump (or after starting in the air): drive again
		if (s.isOnGround && st.jumpStarted >= 0 && tick - st.jumpStarted > 24) { st.jumpStarted = -1; st.doubleJumped = false; st.dodged = false; st.noJump = false; }
		bool airborne = !s.isOnGround || st.jumpStarted >= 0;
		if (!airborne && (p.jumpTick < 0 || tick < p.jumpTick))
		{
			// Ground: get behind the contact point first if we're on the wrong side of it, then come in along the
			// hit direction, timed to get there on the tick
			Vec hd = p.hitDir; hd.z = 0;
			float hl = hd.Length();
			hd = hl > 0.1f ? hd / hl : Vec(1, 0, 0);
			Vec P = s.pos; P.z = 0;
			Vec C = p.target; C.z = 0;
			if (p.jumpTick >= 0)
			{
				// Jumping before the hit: be short of the contact point at takeoff by how far we'll carry in the air
				float carry = std::max(400.0f, s.vel.Length()) * (p.arriveTick - p.jumpTick) / 120.0f;
				C = C - hd * std::min(carry, 2500.0f);
			}
			Vec rel = P - C;
			float along = rel.Dot(hd);                 // + = in front of the contact point (the wrong side)
			Vec side(-hd.y, hd.x, 0);
			float lat = rel.Dot(side);
			const float runIn = 450.0f;
			Vec aimPt;
			float pathLen;
			if (along > -runIn * 0.4f && (P - C).Length() > 150.0f)
			{
				Vec W = C - hd * runIn + side * ((lat >= 0 ? 1.0f : -1.0f) * 250.0f);
				aimPt = W;
				pathLen = (W - P).Length() + (C - W).Length();
			}
			else
			{
				aimPt = C + hd * (along * 0.45f);   // a point on the run-in line between us and the contact point
				if (tau < 30) aimPt = C;
				pathLen = (C - P).Length();
			}
			Vec to = aimPt - P;
			Vec dir = to.Length() > 1 ? to / to.Length() : hd;
			const RotMat& R = s.rotMat;
			float ang = std::atan2(dir.Dot(R.right), dir.Dot(R.forward));
			c.steer = Clamp1(3.5f * ang);
			float speed = s.vel.Dot(R.forward);
			int arrive = p.jumpTick >= 0 ? p.jumpTick : p.arriveTick;
			float timeLeft = std::max(0.02f, (arrive - tick) / 120.0f);
			float need = pathLen / timeLeft;
			if (std::fabs(ang) > 1.3f && speed > 700) c.handbrake = true;
			if (std::fabs(ang) > 0.6f) c.throttle = speed < 1200.0f ? 1.0f : 0.3f;   // still turning around: keep moving
			else if (need > speed + 50) { c.throttle = 1; c.boost = std::fabs(ang) < 0.35f && need > speed + 250 && speed < 2250; }
			else if (need < speed - 100) c.throttle = speed - need > 400 ? -1.0f : 0.0f;
			else c.throttle = Clamp1((need - speed) / 300.0f + 0.2f);
			if (p.power && tau < 48 && std::fabs(ang) < 0.3f) { c.throttle = 1; c.boost = speed < 2280.0f; }
			// Ground dodge into the ball
			if (p.dodge && p.jumpTick < 0 && tau <= p.dodgeLead && tau > p.dodgeLead - 2) { c.jump = true; st.jumpStarted = tick; }
			return c;
		}
		if (st.jumpStarted < 0)
		{
			// Leaving the ground now (our jump), or already in the air without one (falling, off a bounce)
			st.jumpStarted = tick;
			st.noJump = !s.isOnGround;
			if (st.noJump) st.doubleJumped = true;
		}
		int sinceJump = st.noJump ? 1000 : tick - st.jumpStarted;
	
		// Ground dodge in progress (jump, then flip toward the hit direction)
		if (p.dodge && p.jumpTick < 0)
		{
			if (sinceJump < 4) { c.jump = true; return c; }
			if (!st.dodged && sinceJump >= 6)
			{
				const RotMat& R = s.rotMat;
				Vec d = p.hitDir; d.z = 0;
				float f = d.Dot(R.forward), r = d.Dot(R.right), m = std::sqrt(f * f + r * r) + 1e-3f;
				c.pitch = -f / m; c.yaw = r / m; c.jump = true; st.dodged = true;
				return c;
			}
			return c;
		}
	
		// Air: where we'd end up with no boost, and what's left to make up
		Vec pf = s.pos + s.vel * T + Vec(0, 0, 0.5f * g * T * T);
		Vec delta = p.target - pf;
		float miss = delta.Length();
		Vec want = miss > 1 ? delta / miss : p.hitDir;
		float needAcc = 2.0f * miss / (T * T);
	
		// Jump sequence: hold the first jump (up to 0.2 s), release, then a neutral-stick second jump
		if (sinceJump < 12) { c.jump = true; Orient(s, want, Vec(0, 0, 1), false, c); return c; }
		if (p.doubleJump && !st.doubleJumped)
		{
			if (sinceJump == 12) { Orient(s, want, Vec(0, 0, 1), false, c); return c; }
			c.jump = true; c.pitch = c.yaw = c.roll = 0; st.doubleJumped = true;
			return c;
		}
	
		// Late: point the nose where the ball should go; otherwise point along what's left to make up
		bool late = tau < 18 || miss < 40;
		Vec fwd = late ? p.hitDir : want;
		Orient(s, fwd, Vec(0, 0, 1), false, c);
		float align = s.rotMat.forward.Dot(want);
		c.boost = !late && align > 0.8f && needAcc > 250.0f;
		if (late && needAcc > 500.0f && align > 0.7f) c.boost = true;
		if (p.power && tau < 36 && s.rotMat.forward.Dot(p.hitDir) > 0.7f) c.boost = true;
		c.throttle = 1;
		// Aerial dodge into the ball
		if (p.dodge && !st.dodged && tau <= p.dodgeLead && s.HasFlipOrJump())
		{
			const RotMat& R = s.rotMat;
			Vec d = p.hitDir;
			float f = d.Dot(R.forward), r = d.Dot(R.right), m = std::sqrt(f * f + r * r) + 1e-3f;
			c.pitch = -f / m; c.yaw = r / m; c.roll = 0; c.jump = true; c.boost = false; st.dodged = true;
		}
		return c;
	}
	}
	

	struct SimRun
	{
		std::vector<Frame> frames;
		bool wentIn = false, floorFirst = false;
		int  eventTick = -1;
		int  touchTick = -1;     // the bot's first touch
		int  takeoff = -1;
		float boostUsed = 0;
		std::vector<CarControls> ctl;   // the bot's inputs, by tick (so its line can be refined later)
		ShotAnalysis shot;
		RepGrade grade;
	};

	// Plays the plan out from tick s: the bot drives until it touches the ball, then lets go
	// fixedCtl: play these inputs (indexed by tick) instead of the bot
	// resets: ticks where a step-search line's save points were (the car's leftover wheel state is cleared there,
	// exactly as the search did), so replaying its inputs reproduces it exactly
	SimRun Run(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const Bot::Plan& plan, int maxTick,
		const ShotReference& ref, int nudge, const std::vector<CarControls>* fixedCtl = nullptr, const std::vector<int>* resets = nullptr)
	{
		SimRun r;
		r.frames.assign(fr.begin(), fr.begin() + s + 1);
		r.frames.reserve(maxTick + 2);
		ClearCarInternals(arena, car);
		LoadState(car, arena->ball, fr, s);
		ResetPads(arena);
		if (nudge > 0)
		{
			std::mt19937 rng(static_cast<unsigned>(nudge) * 7919u);
			std::uniform_real_distribution<float> u(-1.0f, 1.0f);
			CarState cs = car->GetState();
			cs.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			cs.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
			car->SetState(cs);
			BallState bs = arena->ball->GetState();
			bs.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			bs.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
			arena->ball->SetState(bs);
		}
		Bot::State st;
		{
			CarState cs = car->GetState();
			if (!cs.isOnGround) { st.jumpStarted = s; st.noJump = true; st.doubleJumped = true; }
		}
		float boost0 = car->GetState().boost;
		int lastTouch = -1, stopAt = -1;
		bool bounced = false;
		for (int i = s; i < maxTick; i++)
		{
			if (resets && i > s && std::find(resets->begin(), resets->end(), i) != resets->end())
			{
				CarState cs = car->GetState();
				BallState bs = arena->ball->GetState();
				std::vector<BoostPadState> pads;
				for (BoostPad* p : arena->GetBoostPads()) pads.push_back(p->GetState());
				ClearCarInternals(arena, car);
				car->SetState(cs);
				arena->ball->SetState(bs);
				for (size_t k = 0; k < pads.size(); k++) arena->GetBoostPads()[k]->SetState(pads[k]);
			}
			CarState before = car->GetState();
			CarControls c = fixedCtl ? (i < static_cast<int>(fixedCtl->size()) ? (*fixedCtl)[i] : CarControls()) : Bot::Drive(before, plan, i, st);
			Frame& cur = r.frames.back();
			cur.throttle = c.throttle; cur.steer = c.steer; cur.pitch = c.pitch; cur.yaw = c.yaw; cur.roll = c.roll;
			cur.jump = c.jump; cur.boost = c.boost; cur.handbrake = c.handbrake;
			if (r.takeoff < 0 && c.jump && before.isOnGround) r.takeoff = i;
			if (!fixedCtl) { if (static_cast<int>(r.ctl.size()) <= i) r.ctl.resize(i + 1); r.ctl[i] = c; }
			car->controls = c;
			uint64_t t0 = arena->tickCount;
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
			if (cs.ballHitInfo.isValid && cs.ballHitInfo.tickCountWhenHit >= t0)
			{
				g.touch = true;
				Vec p = cs.ballHitInfo.ballPos + cs.ballHitInfo.relativePosOnBall;
				g.touchX = p.x; g.touchY = p.y; g.touchZ = p.z;
				lastTouch = i + 1;
				if (r.touchTick < 0) r.touchTick = i + 1;
				if (st.touched < 0) st.touched = i + 1 + 4;   // a few more ticks of the hit, then hands off
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
			if (lastTouch < 0 && i > plan.arriveTick + 60) { r.eventTick = i + 1; break; }   // missed it entirely
			if (lastTouch >= 0 && i + 1 - lastTouch > 480) { r.eventTick = i + 1; stopAt = i + 1 + 60; }
		}
		if (r.eventTick < 0) r.eventTick = static_cast<int>(r.frames.size()) - 1;
		r.boostUsed = boost0 - r.frames.back().boostAmount;
		r.shot = ShotGeometry::Analyze(r.frames);
		RepSegments segs = Segmenter::Split(r.frames, r.eventTick);
		r.grade = Grader::GradeRep(r.frames, segs, r.shot, r.wentIn, r.floorFirst, ref);
		return r;
	}

	// Higher is better: the grade first, then how close the shot came to the next grade up. The partial credit
	// steers the search toward 4s and 5s (high in the net, hard, early, on line) instead of settling for a 3.
	float Quality(const SimRun& r, const ShotReference& ref)
	{
		if (r.touchTick < 0) return -100000.0f;
		const ShotAnalysis& a = r.shot;
		float q = r.grade.overall * 10000.0f;
		if (!r.wentIn) return q - (a.reachesGoalLine ? std::min(5000.0f, a.missDist) : 5000.0f);
		auto ramp = [](float v, float lo, float hi) { return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); };
		float credit = 0;
		if (a.type == ShotType::Ground)
		{
			credit += 2.0f * ramp(a.crossZ, 200.0f, 520.0f);        // a 4 at 360, a 5 at 500
			credit += ramp(a.shotSpeed, 1500.0f, 2100.0f);
		}
		else
		{
			float touchSec = (a.touchTick - a.startTick) / 120.0f;
			float vsBest = ref.valid ? touchSec - ref.touchSec : 0.0f;
			credit += ramp(a.crossZ, 150.0f, 420.0f);                  // upper half of the net at 350
			credit += ramp(a.shotSpeed, 1800.0f, 2900.0f);             // fast at 2,800
			credit += ref.valid ? ramp(-vsBest, -0.6f, 0.0f) : 1.0f;   // early: within 0.2 s of your best (0.05 s for a 5)
			credit += ramp(-a.aimErrDeg, -8.0f, -1.6f);               // on line: 3.5 deg for a 4, 1.6 for a 5
			credit += ramp(-a.cornerDist, -900.0f, -200.0f);           // toward a top corner (or a rocket) for a 5
		}
		q += 2000.0f * credit + a.shotSpeed * 0.2f;
		if (a.bouncedIn) q -= 4000.0f;
		return q;
	}

	std::string Secs(int ticks) { return std::format("{:.2f} s", ticks / 120.0f); }

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

	// ======================= The step search (multi-step mechanics) =======================
	// A chess engine searches move by move; for a mechanic the "moves" are its steps (the segments: setup, reset,
	// carry, shot...). Each step tries many versions in the sim from a save point, keeps the best few that pass the
	// step's check, and the next step starts from their save points, so nothing before it is ever re-simulated.
	// Survivors of the last step are replayed whole from the real start (exactly, from their recorded inputs) and
	// graded, so the lines it shows never depend on the save points being perfect.

	// A bot plan plus the choices that made it (so the search can re-aim it)
	struct PlanSpec
	{
		Bot::Plan plan;
		Vec aimPt;
		int ti = 0, jl = 0;
		float reach = 150.0f;
		bool dodge = false, dj = true, power = false;
		bool lift = false;   // aimed by the push the ball needs to reach liftT (a spot on the goal line), not at a point
		Vec liftT;
	};

	// The push a ball needs to drop into T (a spot on the goal line) from where it'll be at ti, flying flat at least
	// 1300: returned as a point a little way along that push from the ball, the form plans take their aim in
	Vec LiftAim(const std::vector<Vec>& path, int ti, const Vec& T)
	{
		const int n = static_cast<int>(path.size());
		ti = std::clamp(ti, 1, n - 2);
		Vec B = path[ti], vB = (path[ti + 1] - path[ti - 1]) * 60.0f;
		Vec d = T - B;
		float flat = std::sqrt(d.x * d.x + d.y * d.y);
		float vflat = std::max(1300.0f, std::sqrt(vB.x * vB.x + vB.y * vB.y));
		float t = std::clamp(flat / vflat, 0.2f, 2.0f);
		Vec want(d.x / t, d.y / t, d.z / t + 325.0f * t);
		Vec push = want - vB;
		float pl = push.Length();
		Vec nrm = pl > 80.0f ? push / pl : want / std::max(1.0f, want.Length());
		return B + nrm * 300.0f;
	}

	// The exact game state at the end of a step
	// A closed-loop skill that drives the car every tick from the car and ball state (e.g. carrying the ball in the air)
	using Controller = std::function<CarControls(int tick, const CarState& car, const BallState& ball)>;

	struct SavePoint
	{
		CarState car;
		BallState ball;
		std::vector<BoostPadState> pads; // the boost pads (which are up)
		std::vector<int> bounds;         // ticks where later steps started (save points), for exact replays
		int tick = 0;                    // the tick this state is at
		std::vector<CarControls> ctl;    // the line's inputs by tick, from the search start up to here
		std::vector<Frame> frames;       // the line's frames from the search start (frames[0] = the start)
		int start = 0;                   // the search start tick (frames[i] is tick start + i)
		int touches = 0, firstTouch = -1, lastTouch = -1;
		float score = 0;                 // the step's own score (higher is better), for keeping the best few
		std::string note;                // what this branch did, for the cue later
		PlanSpec spec;                   // the plan that played the last step (if a plan did)
		bool viaPlan = false;
		int parent = -1;                 // which save point of the previous step it started from
		// How every step so far was played, so the whole line can be re-played closed-loop from a nudged start
		struct ChainStep { bool viaPlan = false; PlanSpec spec; int follow = -1; std::shared_ptr<std::vector<CarControls>> inputs; int ctrl = -1;
			std::shared_ptr<Controller> tuned; };   // a skill whose settings were tuned for this spot (kept to play it again)
		std::vector<ChainStep> chain;
	};

	inline Frame SimFrame(int tick, const CarState& cs, const BallState& bs, bool hit, const Vec& hitPt)
	{
		Frame g;
		g.tick = tick; g.t = tick / 120.0f;
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
		if (hit) { g.touch = true; g.touchX = hitPt.x; g.touchY = hitPt.y; g.touchZ = hitPt.z; }
		return g;
	}

	// A save point at the search start, straight from your recording
	SavePoint StartPoint(Arena* arena, Car* car, const std::vector<Frame>& fr, int s)
	{
		SavePoint sp;
		ClearCarInternals(arena, car);
		LoadState(car, arena->ball, fr, s);
		ResetPads(arena);
		for (BoostPad* p : arena->GetBoostPads()) sp.pads.push_back(p->GetState());
		sp.car = car->GetState();
		sp.ball = arena->ball->GetState();
		sp.tick = s;
		sp.start = s;
		sp.frames.push_back(fr[s]);
		return sp;
	}

	// 1 = the step passed (stop here), -1 = it failed (drop this branch), 0 = keep going
	using StepEnd = std::function<int(const SavePoint& at, int stepStart)>;

	// Plays a plan from a save point until the step ends (or maxTicks run out, which counts as a fail)
	// After the step's touch, a step can keep flying the car somewhere instead of letting go (e.g. following the ball
	// to the backboard for the second touch of a double tap). Returns false = let go.
	using AfterTouch = std::function<bool(int tick, const CarState& car, const BallState& ball, Bot::Plan& follow)>;


	// Air dribble carry: keep the car under and behind the ball (relative to the goal), matching its speed, so the
	// nose meets it softly and keeps pushing it toward the goal. back/down: where the car's center sits from the
	// ball's; k: how hard it closes the gap.
	Controller CarrySkill(float gs, float back, float down, float k, Vec goalAim = Vec(0, 0, 450.0f))
	{
		return [gs, back, down, k, goalAim](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec goal(goalAim.x, gs * 5120.0f, goalAim.z);
			Vec gd = goal - ball.pos; gd.z *= 0.3f;
			gd = gd / std::max(1.0f, gd.Length());
			Vec aimPt = ball.pos - gd * back + Vec(0, 0, -down);
			Vec vDes = ball.vel + (aimPt - car.pos) * k;
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);   // plus a little to hold against gravity
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos; toBall = toBall / std::max(1.0f, toBall.Length());
			Vec fwd = need > 250.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// Air dribble carry aimed at a spot in the net: every tick, work out the velocity the ball needs to drop into
	// (target.x, the goal line, target.z) from where it is (flying flat at least minFlat), and sit the car on the
	// side of the ball that pushes it that way. Soft touches keep nudging the ball onto that arc, so the carry ends
	// with the ball heading high into the net instead of sinking into the bottom of it.
	Controller CarryAim(float gs, float tx, float tz, float back, float k, float minFlat = 1300.0f)
	{
		return [gs, tx, tz, back, k, minFlat](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec T(tx, gs * 5120.0f, tz);
			Vec d = T - ball.pos;
			float flat = std::sqrt(d.x * d.x + d.y * d.y);
			float vflat = std::max(minFlat, std::sqrt(ball.vel.x * ball.vel.x + ball.vel.y * ball.vel.y));
			float t = std::clamp(flat / vflat, 0.25f, 2.0f);
			Vec want(d.x / t, d.y / t, d.z / t + 325.0f * t);
			Vec push = want - ball.vel;
			float pl = push.Length();
			Vec dir = pl > 60.0f ? push / pl : want / std::max(1.0f, want.Length());
			if (dir.z < -0.3f) { dir.z = -0.3f; dir = dir / std::max(1e-3f, dir.Length()); }   // never come down on top of it
			Vec aimPt = ball.pos - dir * back;
			Vec vDes = ball.vel + (aimPt - car.pos) * k;
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos; toBall = toBall / std::max(1.0f, toBall.Length());
			Vec fwd = need > 250.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// A carry with knobs, for tuning from a save point: where it aims the ball (gx, gz on the goal line), where the
	// car sits under it (back, down), how hard it closes the gap (k), and how much lower it tucks when the ball is
	// heading into the net too low (lift: extra 'down' per unit the predicted crossing sits under 515)
	constexpr int kCarryKnobs = 8;   // the carry uses the first 6, the reset all 8
	using Knobs = std::array<float, kCarryKnobs>;
	Controller ParamCarry(float gs, Knobs P)
	{
		return [gs, P](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec goal(P[0], gs * 5120.0f, P[1]);
			Vec gd = goal - ball.pos; gd.z *= 0.3f;
			gd = gd / std::max(1.0f, gd.Length());
			float down = P[3];
			if (ball.vel.y * gs > 200.0f)
			{
				float t = std::clamp((gs * 5120.0f - ball.pos.y) / ball.vel.y, 0.0f, 1.5f);
				float cz = ball.pos.z + ball.vel.z * t - 325.0f * t * t;
				down = std::clamp(down + P[5] * (515.0f - cz), -60.0f, 160.0f);
			}
			Vec aimPt = ball.pos - gd * P[2] + Vec(0, 0, -down);
			Vec vDes = ball.vel + (aimPt - car.pos) * P[4];
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos; toBall = toBall / std::max(1.0f, toBall.Length());
			Vec fwd = need > 250.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// A good spot to hand an air dribble to the finish (measured on Coop's 165 air dribbles, 1,000 to 2,800 out:
	// from spots like this the tuned finish goes in high 65% of the time, from the rest 21%): the ball moving 1,675+,
	// above 1,130, within 300 of the car, the car keeping pace with it (not running into it)
	float HandoffScore(const SavePoint& at, float gs, bool* goodOut = nullptr)
	{
		const BallState& b = at.ball;
		const CarState& c = at.car;
		const float speed = b.vel.Length(), dist = (b.pos - c.pos).Length();
		const float relVF = (b.vel - c.vel).Dot(c.rotMat.forward);
		const bool good = speed > 1675.0f && b.pos.z > 1130.0f && dist < 300.0f && relVF > -68.0f;
		if (goodOut) *goodOut = good;
		return (good ? 1000.0f : 0.0f) + std::clamp((speed - 1675.0f) / 3.0f, -200.0f, 100.0f) + std::clamp((b.pos.z - 1130.0f) / 3.0f, -200.0f, 100.0f)
			- std::max(0.0f, dist - 250.0f) + std::clamp(relVF + 68.0f, -150.0f, 50.0f) + std::clamp(b.vel.y * gs - 1200.0f, -300.0f, 300.0f) / 3.0f;
	}

	// Flip reset: get the wheels onto the ball. Flies at a spot just off the ball nose first, then close in turns its
	// wheels to the ball (nose toward the goal) and settles onto it. Knobs: how far the contact leans toward the goal
	// (a) and sideways (b), position gain (k), how close it flies in nose first (sw), closing speed at contact (uc),
	// and how far ahead it reads the ball (lead, s).
	Controller ResetSkill(float gs, Knobs P)
	{
		// A little memory: whether the flip came back on the ball during this run (a new run is spotted by the tick
		// going backwards or jumping, so the same skill can be played again from any save point)
		struct Mem { int lastTick = -1000; bool prevFlip = true; bool done = false; };
		auto mem = std::make_shared<Mem>();
		return [gs, P, mem](int tick, const CarState& car, const BallState& ball) {
			CarControls c;
			if (tick <= mem->lastTick || tick > mem->lastTick + 1) { mem->done = false; mem->prevFlip = car.HasFlipOrJump(); }
			mem->lastTick = tick;
			const bool flip = car.HasFlipOrJump();
			if (flip && !mem->prevFlip && (ball.pos - car.pos).Length() < 220.0f) mem->done = true;
			mem->prevFlip = flip;
			// Reset done: let go of the ball, nose toward it, flip saved for the shot
			if (mem->done)
			{
				Vec toBall = ball.pos - car.pos;
				Bot::Orient(car, toBall / std::max(1.0f, toBall.Length()), Vec(0, 0, 1), false, c, 3.0f, 0.4f);
				return c;
			}
			const float lead = std::clamp(P[5], 0.0f, 0.4f);
			Vec bp = ball.pos + ball.vel * lead + Vec(0, 0, -325.0f * lead * lead);
			Vec bv = ball.vel + Vec(0, 0, -650.0f * lead);
			Vec gdir(0, gs, 0), side(1, 0, 0);
			Vec u = Vec(0, 0, 1) + gdir * P[0] + side * P[1];   // from the car to the ball at contact
			u = u / std::max(1e-3f, u.Length());
			// Until the ball nears the top of its arc (its climb slows below P[7]), hang P[6] under it; then go for it
			const bool wait = ball.vel.z > P[7];
			Vec target = bp - u * (112.0f + (wait ? P[6] : 0.0f));   // the car's center: ball radius plus the car's half height
			Vec vDes = bv + (target - car.pos) * P[2] + (wait ? Vec() : u * P[4]);
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.12f);
			float need = dv.Length();
			float dist = (target - car.pos).Length();
			if (wait || dist > P[3])
			{
				Vec fwd = need > 150.0f ? dv / need : (target - car.pos) / std::max(1.0f, dist);
				Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
				c.boost = need > 150.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			}
			else
			{
				Vec up = u * -1.0f;
				Vec fwd = gdir - up * gdir.Dot(up);
				fwd = fwd / std::max(1e-3f, fwd.Length());
				Bot::Orient(car, fwd, up, true, c, 4.0f, 0.45f);
				c.boost = car.rotMat.forward.Dot(dv) > 300.0f;
			}
			c.throttle = 0;
			// A reset only gives the flip back if it's gone: with one left, spend it as a plain double jump (stick
			// centered, so it doesn't flip) while climbing to the ball
			if (!car.isOnGround && car.HasFlipOrJump() && !car.isJumping && (target.z - car.pos.z) > 40.0f)
			{
				c.jump = true;
				c.pitch = 0; c.yaw = 0; c.roll = 0;
			}
			return c;
		};
	}

	// Flip shot (after a reset): stay lined up behind the ball along the shot (aimed at gx, gz on the goal line),
	// then flip into it once close. Knobs: aim (gx, gz), where it sits behind the ball (back) and how hard it closes
	// (k), the distance it flips from (trig), how far ahead it reads the ball (lead), flip direction mix (side).
	Controller FlipShot(float gs, Knobs P)
	{
		struct Mem { int lastTick = -1000; int dodgeAt = -1; };
		auto mem = std::make_shared<Mem>();
		return [gs, P, mem](int tick, const CarState& car, const BallState& ball) {
			CarControls c;
			if (tick <= mem->lastTick || tick > mem->lastTick + 1) mem->dodgeAt = -1;
			mem->lastTick = tick;
			if (mem->dodgeAt >= 0)
			{
				// Mid-flip: keep the stick where the flip went, nothing else to do
				c.pitch = -1.0f; c.yaw = Bot::Clamp1(P[6]);
				return c;
			}
			const float lead = std::clamp(P[5], 0.0f, 0.4f);
			Vec bp = ball.pos + ball.vel * lead + Vec(0, 0, -325.0f * lead * lead);
			Vec aim(P[0], gs * 5120.0f, P[1]);
			Vec d = aim - bp; d = d / std::max(1.0f, d.Length());
			Vec target = bp - d * P[2];
			Vec vDes = ball.vel + (target - car.pos) * P[3];
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.12f);
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos;
			float dist = toBall.Length();
			toBall = toBall / std::max(1.0f, dist);
			Vec fwd = need > 200.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			if (dist < P[4] && car.rotMat.forward.Dot(toBall) > 0.7f && car.HasFlipOrJump() && !car.isOnGround)
			{
				c.jump = true; c.pitch = -1.0f; c.yaw = Bot::Clamp1(P[6]); c.roll = 0; c.boost = false;
				mem->dodgeAt = tick;
			}
			return c;
		};
	}

	SavePoint RunFrom(Arena* arena, Car* car, const SavePoint& sp, const Bot::Plan& plan, int maxTicks, const StepEnd& ended, int& result,
		const AfterTouch* afterTouch = nullptr, const std::vector<CarControls>* fixed = nullptr, const Controller* ctrl = nullptr)
	{
		SavePoint r = sp;
		if (sp.tick != sp.start) r.bounds.push_back(sp.tick);
		ClearCarInternals(arena, car);
		car->SetState(sp.car);
		arena->ball->SetState(sp.ball);
		{
			const auto& pads = arena->GetBoostPads();
			for (size_t i = 0; i < pads.size(); i++) pads[i]->SetState(i < sp.pads.size() ? sp.pads[i] : BoostPadState());
		}
		Bot::State st;
		if (!sp.car.isOnGround) { st.jumpStarted = sp.tick; st.noJump = true; st.doubleJumped = true; }
		Bot::Plan follow;
		Bot::State fst;
		bool following = false;
		result = -1;
		for (int k = 0; k < maxTicks; k++)
		{
			CarState before = car->GetState();
			CarControls c;
			if (afterTouch && st.touched >= 0 && r.tick >= st.touched)
			{
				if (!following || (r.tick - st.touched) % 10 == 0)
				{
					if (!following) { fst = Bot::State(); fst.jumpStarted = r.tick; fst.noJump = !before.isOnGround; fst.doubleJumped = true; }
					following = (*afterTouch)(r.tick, before, arena->ball->GetState(), follow) || following;
				}
				if (following) c = Bot::Drive(before, follow, r.tick, fst);
			}
			else if (ctrl) c = (*ctrl)(r.tick, before, arena->ball->GetState());
			else if (fixed) c = r.tick < static_cast<int>(fixed->size()) ? (*fixed)[r.tick] : CarControls();
			else c = Bot::Drive(before, plan, r.tick, st);
			if (static_cast<int>(r.ctl.size()) <= r.tick) r.ctl.resize(r.tick + 1);
			r.ctl[r.tick] = c;
			Frame& cur = r.frames.back();
			cur.throttle = c.throttle; cur.steer = c.steer; cur.pitch = c.pitch; cur.yaw = c.yaw; cur.roll = c.roll;
			cur.jump = c.jump; cur.boost = c.boost; cur.handbrake = c.handbrake;
			car->controls = c;
			uint64_t t0 = arena->tickCount;
			arena->Step(1);
			r.tick++;
			CarState cs = car->GetState();
			BallState bs = arena->ball->GetState();
			bool hit = cs.ballHitInfo.isValid && cs.ballHitInfo.tickCountWhenHit >= t0;
			r.frames.push_back(SimFrame(r.tick, cs, bs, hit, cs.ballHitInfo.ballPos + cs.ballHitInfo.relativePosOnBall));
			if (hit)
			{
				if (r.lastTouch < 0 || r.tick - r.lastTouch > 8) r.touches++;
				if (r.firstTouch < 0) r.firstTouch = r.tick;
				r.lastTouch = r.tick;
				if (st.touched < 0) st.touched = r.tick + 4;
			}
			int e = ended(r, sp.tick);
			if (e != 0) { result = e; break; }
		}
		r.car = car->GetState();
		r.ball = arena->ball->GetState();
		r.pads.clear();
		for (BoostPad* p : arena->GetBoostPads()) r.pads.push_back(p->GetState());
		return r;
	}

	// Plays a found line out from its last save point (hands off: no inputs) until the shot is over, by the same end
	// rules as Run, and grades the whole rep: your frames up to the search start, the line, then the play-out. The
	// frames graded are exactly the ones the search produced, so what's shown is what was found.
	SimRun PlayOut(Arena* arena, Car* car, const std::vector<Frame>& fr, const SavePoint& f, int maxTick, const ShotReference& ref)
	{
		SimRun r;
		r.frames.assign(fr.begin(), fr.begin() + f.start);
		r.frames.insert(r.frames.end(), f.frames.begin(), f.frames.end());
		r.ctl = f.ctl;
		r.touchTick = f.firstTouch;
		ClearCarInternals(arena, car);
		car->SetState(f.car);
		arena->ball->SetState(f.ball);
		{
			const auto& pads = arena->GetBoostPads();
			for (size_t i = 0; i < pads.size(); i++) pads[i]->SetState(i < f.pads.size() ? f.pads[i] : BoostPadState());
		}
		const float boost0 = f.frames.empty() ? 0.0f : f.frames.front().boostAmount;
		int lastTouch = f.lastTouch, stopAt = -1;
		bool bounced = false;
		for (int i = f.tick; i < maxTick; i++)
		{
			Frame& cur = r.frames.back();
			CarControls c;
			cur.throttle = 0; cur.steer = 0; cur.pitch = 0; cur.yaw = 0; cur.roll = 0; cur.jump = false; cur.boost = false; cur.handbrake = false;
			car->controls = c;
			uint64_t t0 = arena->tickCount;
			arena->Step(1);
			CarState cs = car->GetState();
			BallState bs = arena->ball->GetState();
			bool hit = cs.ballHitInfo.isValid && cs.ballHitInfo.tickCountWhenHit >= t0;
			Frame g = SimFrame(i + 1, cs, bs, hit, cs.ballHitInfo.ballPos + cs.ballHitInfo.relativePosOnBall);
			if (hit) lastTouch = i + 1;
			r.frames.push_back(g);
			float dirY = bs.vel.y >= 0.0f ? 1.0f : -1.0f;
			float toLine = 5120.0f - bs.pos.y * dirY;
			float tLine = std::fabs(bs.vel.y) > 1.0f ? toLine / std::fabs(bs.vel.y) : 1e9f;
			bool rollingIn = std::fabs(bs.vel.y) > 300.0f && toLine > 0.0f && tLine < 6.0f && std::fabs(bs.pos.x + bs.vel.x * tLine) < 950.0f;
			if (!r.wentIn && std::fabs(bs.pos.y) > 5215.0f && std::fabs(bs.pos.x) < 893.0f && bs.pos.z < 643.0f)
			{
				r.wentIn = true; r.floorFirst = bounced; r.eventTick = i + 1; stopAt = i + 1 + 60;
				continue;
			}
			if (stopAt >= 0)
			{
				if (i + 1 >= stopAt && (r.wentIn || !rollingIn || i + 1 >= r.eventTick + 900)) break;
				continue;
			}
			if (lastTouch >= 0 && bs.pos.z < 100.0f && bs.vel.z > 0.0f && i + 1 - lastTouch > 10 && !ShotGeometry::BallWithCar(g))
			{
				bounced = true; r.eventTick = i + 1; stopAt = i + 1 + 60;
				continue;
			}
			if (lastTouch >= 0 && i + 1 - lastTouch > 480) { r.eventTick = i + 1; stopAt = i + 1 + 60; }
		}
		if (r.eventTick < 0) r.eventTick = static_cast<int>(r.frames.size()) - 1;
		r.boostUsed = boost0 - r.frames.back().boostAmount;
		r.shot = ShotGeometry::Analyze(r.frames);
		RepSegments segs = Segmenter::Split(r.frames, r.eventTick);
		r.grade = Grader::GradeRep(r.frames, segs, r.shot, r.wentIn, r.floorFirst, ref);
		return r;
	}

	// Where the ball goes from a state on its own (nobody touches it)
	std::vector<Vec> BallPathFrom(Arena* ballArena, const BallState& b, int ticks)
	{
		std::vector<Vec> path;
		path.reserve(ticks + 1);
		ballArena->ball->SetState(b);
		path.push_back(b.pos);
		for (int i = 0; i < ticks; i++) { ballArena->Step(1); path.push_back(ballArena->ball->GetState().pos); }
		return path;
	}

	// A bot plan from any save point: meet the ball (on its own path from there) at tick `base + ti` and send it
	// toward aimPt (a point the ball's center should pass through: the goal, or a spot on the backboard)
	bool PlanFrom(const SavePoint& sp, const std::vector<Vec>& ballPath, int ti, Vec aimPt, float reach, int jumpLead,
		bool dodge, bool doubleJump, bool power, Bot::Plan& plan)
	{
		const int horizon = static_cast<int>(ballPath.size()) - 1;
		if (ti < 6 || ti > horizon) return false;
		Vec B = ballPath[ti];
		Vec A = aimPt;
		float flight = (A - B).Length() / 2000.0f;
		A.z += 0.5f * 650.0f * flight * flight;
		Vec d = A - B;
		float dl = d.Length();
		if (dl < 1) return false;
		d = d / dl;
		Vec C = B - d * reach;
		const bool onWall = sp.car.isOnGround && sp.car.rotMat.up.z < 0.7f;   // on a wall (or the ceiling): jump off it
		const bool startAir = !sp.car.isOnGround || onWall;
		bool ground = C.z < 60.0f && !startAir;
		if (ground)
		{
			Vec dg = d; dg.z = 0;
			if (dg.Length() < 0.1f) return false;
			dg = dg / dg.Length();
			C = B - dg * reach;
			C.z = 17.0f;
		}
		float tau = ti / 120.0f;
		const Vec p0 = sp.car.pos;
		const Vec v0 = onWall ? sp.car.vel + sp.car.rotMat.up * 300.0f : sp.car.vel;   // the jump pushes you off the wall
		if (ground)
		{
			Vec h = C - p0; h.z = 0;
			if (h.Length() / tau > 2300.0f) return false;
		}
		else
		{
			Vec drift = p0 + v0 * tau + Vec(0, 0, -0.5f * 650.0f * tau * tau);
			Vec need = C - drift;
			// Close in (the ball is coming right at you, e.g. off the backboard): no time to move, just turn and hit it
			bool closeIn = (B - drift).Length() < 260.0f && ti <= 40;
			if (!closeIn && 2.0f * need.Length() / (tau * tau) > 1250.0f) return false;
		}
		plan.target = C;
		plan.hitDir = d;
		plan.arriveTick = sp.tick + ti;
		plan.jumpTick = onWall ? sp.tick : (ground || startAir) ? -1 : std::max(sp.tick, sp.tick + ti - jumpLead);
		if (!ground && !startAir && plan.jumpTick < 0) plan.jumpTick = sp.tick;
		plan.dodge = dodge;
		plan.doubleJump = doubleJump && !dodge;
		plan.dodgeLead = ground ? 14 : 8;
		plan.power = power;
		return true;
	}

	bool AddPlan(std::vector<PlanSpec>& out, const SavePoint& sp, const std::vector<Vec>& path, int ti, Vec aim, float reach, int jl,
		bool dodge, bool dj, bool power)
	{
		PlanSpec ps;
		if (!PlanFrom(sp, path, ti, aim, reach, jl, dodge, dj, power, ps.plan)) return false;
		ps.aimPt = aim; ps.ti = ti; ps.jl = jl; ps.reach = reach; ps.dodge = dodge; ps.dj = dj; ps.power = power;
		out.push_back(ps);
		return true;
	}

	bool AddLiftPlan(std::vector<PlanSpec>& out, const SavePoint& sp, const std::vector<Vec>& path, int ti, const Vec& T, float reach, int jl,
		bool dodge, bool power)
	{
		if (!AddPlan(out, sp, path, ti, LiftAim(path, ti, T), reach, jl, dodge, false, power)) return false;
		out.back().lift = true;
		out.back().liftT = T;
		return true;
	}

	// Debug counters for the step search (test builds define ENGINE_STEP_DEBUG; never on in the plugin)
#ifdef ENGINE_STEP_DEBUG
	const bool kStepDebug = std::getenv("ENG_STEPDBG") != nullptr;
#else
	constexpr bool kStepDebug = false;
#endif
	int gStepDbg[32] = {};
#ifndef ENGINE_SCALE_CAP
#define ENGINE_SCALE_CAP 0.0f
#endif
	// How far the search may grow past its normal size when the machine has time to spare: a little on a normal
	// look (it stays quick), a lot more when you ask it to think harder (that wait is what the button is for)
	float ScaleCap(int depth) { return ENGINE_SCALE_CAP > 0.0f ? ENGINE_SCALE_CAP : depth >= 2 ? 3.0f : 1.5f; }

	// One step of a mechanic for the search: its candidate plans from a save point, when it's over, and how good
	// the save point at its end is (for keeping the best few)
	struct StepDef
	{
		const char* name;
		std::function<void(const SavePoint&, const std::vector<Vec>& ballPath, std::vector<PlanSpec>& out)> plans;
		// Last step only: where the ball will cross the goal line from a save point (to re-aim the best few lines)
		std::function<bool(const SavePoint&, Vec& crossed)> crossing;
		int aimFix = 0;
		StepEnd ended;
		std::function<float(const SavePoint&)> score;
		int maxTicks = 300;
		int beam = 8;
		AfterTouch afterTouch;          // empty = let go after the touch
		std::vector<AfterTouch> follows;   // or several ways to follow up (each candidate gets one; your inputs try all)
		// Candidates from recorded inputs (yours, reshaped a little), indexed by tick. Each is tried as it is and,
		// if the step has an afterTouch, also with the bot taking over after the touch.
		std::function<void(const SavePoint&, std::vector<std::vector<CarControls>>& out)> inputs;
		std::vector<Controller> controllers;   // closed-loop skills to try (each from every save point)
		// A skill tuned from each save point (cross-entropy over its knobs, scored by fitness on where the step ends
		// up); the best few tuned versions go on like any other branch
		struct Tune
		{
			int pop = 24, iters = 5, keep = 3;
			Knobs mu{}, sd{}, lo{}, hi{};
			std::function<Controller(const Knobs&)> make;
			std::function<float(const SavePoint&)> fitness;
		};
		std::shared_ptr<Tune> tune;
	};

	// The search itself. budget = sim runs it may spend across all steps. Returns the survivors of the last step.
	std::vector<SavePoint> StepSearch(Arena* arena, Car* car, Arena* ballArena, const SavePoint& start, const std::vector<StepDef>& steps,
		int budget, std::mt19937& rng, const std::function<bool()>& stop, int& runs, std::vector<int>* passedPerStep = nullptr)
	{
		std::vector<SavePoint> frontier = { start };
		if (passedPerStep) passedPerStep->assign(steps.size(), 0);
		for (size_t k = 0; k < steps.size() && !frontier.empty(); k++)
		{
			const StepDef& st = steps[k];
			const int share = std::max(20, budget / static_cast<int>(steps.size()) / static_cast<int>(frontier.size()));
			std::vector<SavePoint> next;
			for (int fi = 0; fi < static_cast<int>(frontier.size()); fi++)
			{
				const SavePoint& sp = frontier[fi];
				std::vector<Vec> path = BallPathFrom(ballArena, sp.ball, 360);
				std::vector<PlanSpec> cands;
				st.plans(sp, path, cands);
				if (kStepDebug) fprintf(stderr, "step %zu: %zu candidate plans from tick %d (car on ground %d at %.0f %.0f %.0f vel %.0f %.0f %.0f, ball %.0f %.0f %.0f vel %.0f %.0f %.0f)\n", k, cands.size(), sp.tick, (int)sp.car.isOnGround, sp.car.pos.x, sp.car.pos.y, sp.car.pos.z, sp.car.vel.x, sp.car.vel.y, sp.car.vel.z, sp.ball.pos.x, sp.ball.pos.y, sp.ball.pos.z, sp.ball.vel.x, sp.ball.vel.y, sp.ball.vel.z);
				std::shuffle(cands.begin(), cands.end(), rng);
				if (static_cast<int>(cands.size()) > share) cands.resize(share);
				int ci = 0;
				for (const PlanSpec& ps : cands)
				{
					if (stop()) break;
					int res = -1;
					const AfterTouch* at = !st.follows.empty() ? &st.follows[ci++ % st.follows.size()] : st.afterTouch ? &st.afterTouch : nullptr;
					SavePoint r = RunFrom(arena, car, sp, ps.plan, st.maxTicks, st.ended, res, at);
					runs++;
					if (kStepDebug) { gStepDbg[k * 8 + (r.firstTouch < 0 ? 0 : res == 1 ? 1 : res < 0 ? 1 - res : 7)]++; }
					if (res != 1) continue;
					r.score = st.score(r);
					r.spec = ps; r.viaPlan = true; r.parent = fi;
					{
						SavePoint::ChainStep cs; cs.viaPlan = true; cs.spec = ps;
						cs.follow = !st.follows.empty() ? (ci - 1) % static_cast<int>(st.follows.size()) : (st.afterTouch ? 99 : -1);
						r.chain.push_back(cs);
					}
					next.push_back(std::move(r));
				}
				if (stop()) break;
				if (st.inputs)
				{
					std::vector<std::vector<CarControls>> ins;
					st.inputs(sp, ins);
					Bot::Plan none;
					none.arriveTick = sp.tick + st.maxTicks + 1000;
					const int nf = static_cast<int>(st.follows.size()) + (st.afterTouch ? 1 : 0);
					for (const auto& in0 : ins)
					{
						auto inPtr = std::make_shared<std::vector<CarControls>>(in0);
						const std::vector<CarControls>& in = *inPtr;
						for (int follow = 0; follow <= nf && !stop(); follow++)
						{
							int res = -1;
							const AfterTouch* at = follow == 0 ? nullptr : follow <= static_cast<int>(st.follows.size()) ? &st.follows[follow - 1] : &st.afterTouch;
							SavePoint r = RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, at, &in);
							runs++;
							if (kStepDebug) { gStepDbg[k * 8 + (r.firstTouch < 0 ? 0 : res == 1 ? 1 : res < 0 ? 1 - res : 7)]++; }
							if (res != 1) continue;
							r.score = st.score(r) + 0.05f;   // a nudge toward your own way in when it's as good
							r.note = "your setup, a better second touch";
							r.parent = fi;
							{
								SavePoint::ChainStep cs; cs.viaPlan = false; cs.inputs = inPtr;
								cs.follow = follow == 0 ? -1 : follow <= static_cast<int>(st.follows.size()) ? follow - 1 : 99;
								r.chain.push_back(cs);
							}
							next.push_back(std::move(r));
						}
					}
				}
				for (int ci2 = 0; ci2 < static_cast<int>(st.controllers.size()) && !stop(); ci2++)
				{
					int res = -1;
					Bot::Plan none;
					none.arriveTick = sp.tick + st.maxTicks + 1000;
					SavePoint r = RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci2]);
					runs++;
					if (kStepDebug) { gStepDbg[k * 8 + (r.firstTouch < 0 ? 0 : res == 1 ? 1 : res < 0 ? 1 - res : 7)]++; }
					if (kStepDebug && st.crossing)
					{
						Vec cr; bool ok = st.crossing(r, cr);
						const Frame& lf = r.frames.back();
						fprintf(stderr, "  ctrl %d of %s: res %d touches %d ticks %d cross %d at %.0f %.0f | ball %.0f %.0f %.0f v %.0f %.0f %.0f car %.0f %.0f %.0f\n", ci2, st.name, res, r.touches, r.tick - sp.tick, (int)ok, cr.x, cr.z,
							lf.ballX, lf.ballY, lf.ballZ, lf.ballVX, lf.ballVY, lf.ballVZ, lf.carX, lf.carY, lf.carZ);
#ifdef ENGINE_STEP_DEBUG
						static int traced = 0;
						if (std::getenv("ENG_TRACE") && ci2 == atoi(std::getenv("ENG_TRACE")) && traced++ < 2)
							for (size_t q = std::max(0, sp.tick - r.start); q < r.frames.size(); q += 4)
							{
								const Frame& g = r.frames[q];
								fprintf(stderr, "     t%4d ball %5.0f %5.0f %4.0f v %5.0f %5.0f %5.0f | car %5.0f %5.0f %4.0f v %5.0f %5.0f %5.0f pitch %6d boost %d%s\n", g.tick, g.ballX, g.ballY, g.ballZ, g.ballVX, g.ballVY, g.ballVZ,
									g.carX, g.carY, g.carZ, g.carVX, g.carVY, g.carVZ, g.carPitch, (int)g.boost, g.touch ? " T" : "");
							}
#endif
					}
					if (res != 1) continue;
					r.score = st.score(r);
					r.parent = fi;
					SavePoint::ChainStep cs; cs.ctrl = ci2;
					r.chain.push_back(cs);
					next.push_back(std::move(r));
				}
				if (st.tune && !stop())
				{
					const StepDef::Tune& T = *st.tune;
					Knobs mu = T.mu, sd = T.sd;
					std::mt19937 trng(static_cast<unsigned>(sp.tick) * 2654435761u + static_cast<unsigned>(fi));
					std::normal_distribution<float> N01(0.0f, 1.0f);
					struct Cand { float f; Knobs x; SavePoint r; };
					std::vector<Cand> best;   // the best few seen, distinct settings
					for (int it = 0; it < T.iters && !stop(); it++)
					{
						std::vector<std::pair<float, Knobs>> sc;
						for (int k2 = 0; k2 < T.pop && !stop(); k2++)
						{
							Knobs x;
							for (int j = 0; j < kCarryKnobs; j++) x[j] = std::clamp(mu[j] + sd[j] * N01(trng), T.lo[j], T.hi[j]);
							Controller ctl = T.make(x);
							Bot::Plan none;
							none.arriveTick = sp.tick + st.maxTicks + 1000;
							int res = -1;
							SavePoint r = RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &ctl);
							runs++;
							float f = res == 1 ? T.fitness(r) : -1e6f;
							sc.push_back({ f, x });
							if (res == 1 && (best.size() < static_cast<size_t>(T.keep) || f > best.back().f))
							{
								best.push_back({ f, x, std::move(r) });
								std::sort(best.begin(), best.end(), [](const Cand& a, const Cand& b) { return a.f > b.f; });
								if (best.size() > static_cast<size_t>(T.keep)) best.pop_back();
							}
						}
						if (sc.empty()) break;
						std::sort(sc.begin(), sc.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
						const int el = std::max(2, static_cast<int>(sc.size()) / 5);
						for (int j = 0; j < kCarryKnobs; j++)
						{
							float m = 0, v = 0;
							for (int e = 0; e < el; e++) m += sc[e].second[j];
							m /= el;
							for (int e = 0; e < el; e++) v += (sc[e].second[j] - m) * (sc[e].second[j] - m);
							mu[j] = m;
							sd[j] = std::max(std::sqrt(v / el), (T.hi[j] - T.lo[j]) * 0.02f);
						}
					}
					for (Cand& c : best)
					{
						SavePoint r = std::move(c.r);
						r.score = st.score(r) + c.f * 0.001f;
						r.parent = fi;
						r.note = st.name == std::string("Travel") ? "carried it in" : "carried in high";
						SavePoint::ChainStep cs;
						cs.tuned = std::make_shared<Controller>(T.make(c.x));
						r.chain.push_back(cs);
						next.push_back(std::move(r));
					}
				}
				if (stop()) break;
			}
			// Re-aim the best few (the shooting method): where did the ball really cross the goal line vs where the plan
			// aimed it? Aim off by that much the other way and play the step again from the same save point.
			if (st.crossing && st.aimFix > 0 && !next.empty())
			{
				std::sort(next.begin(), next.end(), [](const SavePoint& a, const SavePoint& b) { return a.score > b.score; });
				const int top = std::min<int>(st.aimFix, static_cast<int>(next.size()));
				std::vector<SavePoint> fixed;
				for (int i = 0; i < top && !stop(); i++)
				{
					if (!next[i].viaPlan || next[i].parent < 0) continue;
					SavePoint cur = next[i];
					const SavePoint& par = frontier[cur.parent];
					std::vector<Vec> path = BallPathFrom(ballArena, par.ball, 360);
					for (int iter = 0; iter < 3 && !stop(); iter++)
					{
						Vec cr;
						if (!st.crossing(cur, cr)) break;
						const Vec want = cur.spec.lift ? cur.spec.liftT : cur.spec.aimPt;
						Vec err(cr.x - want.x, 0, cr.z - want.z);
						if (err.Length() < 30.0f) break;
						PlanSpec ps = cur.spec;
						std::vector<PlanSpec> one;
						if (ps.lift)
						{
							// Aim the push at a spot moved by the miss (the shooting method, on the target)
							if (!AddLiftPlan(one, par, path, ps.ti, ps.liftT - err, ps.reach, ps.jl, ps.dodge, ps.power)) break;
						}
						else if (!AddPlan(one, par, path, ps.ti, ps.aimPt - err, ps.reach, ps.jl, ps.dodge, ps.dj, ps.power)) break;
						int res = -1;
						SavePoint r = RunFrom(arena, car, par, one[0].plan, st.maxTicks, st.ended, res, st.afterTouch ? &st.afterTouch : nullptr);
						runs++;
						if (res != 1) break;
						r.score = st.score(r);
						r.spec = one[0]; r.viaPlan = true; r.parent = cur.parent;
						{
							SavePoint::ChainStep cs; cs.viaPlan = true; cs.spec = one[0]; cs.follow = st.afterTouch ? 99 : -1;
							r.chain.push_back(cs);
						}
						cur = r;
						fixed.push_back(r);
					}
				}
				for (SavePoint& f : fixed) next.push_back(std::move(f));
			}
			if (passedPerStep) (*passedPerStep)[k] = static_cast<int>(next.size());
			std::sort(next.begin(), next.end(), [](const SavePoint& a, const SavePoint& b) { return a.score > b.score; });
			// Keep the best few, but not near-copies of each other (same end tick and ball spot)
			std::vector<SavePoint> keep;
			for (SavePoint& c : next)
			{
				bool dup = false;
				for (const SavePoint& q : keep)
					if (std::abs(q.tick - c.tick) <= 2 && (q.ball.pos - c.ball.pos).Length() < 60.0f && (q.ball.vel - c.ball.vel).Length() < 150.0f) { dup = true; break; }
				if (!dup) keep.push_back(std::move(c));
				if (static_cast<int>(keep.size()) >= (k + 1 == steps.size() ? st.beam * 5 : st.beam)) break;
			}
			frontier = std::move(keep);
		}
		return frontier;
	}

	// Plays a found line again, closed-loop, from a nudged start with the timing shifted: the bot re-aims each step's
	// plan from wherever the car really is, and your own inputs are shifted. Returns the controls it ended up using
	// (empty if a step broke), so the caller can replay and grade them exactly.
	bool RerunChain(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const SavePoint& line,
		const std::vector<StepDef>& steps, int nudge, int shift, SavePoint& endAt)
	{
		SavePoint sp = StartPoint(arena, car, fr, s);
		if (nudge > 0)
		{
			// The same nudge Run() applies, so the replay that grades it starts from the exact same spot
			std::mt19937 rng(static_cast<unsigned>(nudge) * 7919u);
			std::uniform_real_distribution<float> u(-1.0f, 1.0f);
			sp.car.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			sp.car.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
			sp.ball.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			sp.ball.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
		}
		static thread_local std::unique_ptr<Arena> ballArena;
		if (!ballArena) ballArena.reset(Arena::Create(GameMode::SOCCAR));
		for (size_t k = 0; k < line.chain.size() && k < steps.size(); k++)
		{
			const SavePoint::ChainStep& cs = line.chain[k];
			const StepDef& st = steps[k];
			const bool last = k + 1 == line.chain.size();
			const AfterTouch* at = cs.follow < 0 ? nullptr : cs.follow == 99 ? (st.afterTouch ? &st.afterTouch : nullptr)
				: cs.follow < static_cast<int>(st.follows.size()) ? &st.follows[cs.follow] : nullptr;
			Bot::Plan none;
			none.arriveTick = sp.tick + st.maxTicks + 1000;
			// The ways to play this step again: as found (re-planned from where things are now), and on the last step
			// a few small adjustments (a touch earlier or later, the other carry styles), like a player adjusting the shot
			std::vector<std::function<SavePoint(int&)>> tries;
			if (cs.tuned)
			{
				tries.push_back([&, c = cs.tuned](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, c.get()); });
				for (int ci = 0; ci < static_cast<int>(st.controllers.size()); ci++)
					tries.push_back([&, ci](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
			}
			else if (cs.ctrl >= 0 && cs.ctrl < static_cast<int>(st.controllers.size()))
			{
				tries.push_back([&, ci = cs.ctrl](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
				// A player adjusts the carry to how the touches are really going: the other carry styles are fair game
				for (int ci = 0; ci < static_cast<int>(st.controllers.size()); ci++)
						if (ci != cs.ctrl) tries.push_back([&, ci](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
			}
			else if (cs.viaPlan)
			{
				std::vector<Vec> path = BallPathFrom(ballArena.get(), sp.ball, 360);
				for (int dt : { 0, -3, 3, -6, 6 })
				{
					PlanSpec again;
					Bot::Plan p = cs.spec.plan;
					const int ti2 = std::max(4, cs.spec.ti + dt);
					const Vec aim2 = cs.spec.lift ? LiftAim(path, ti2, cs.spec.liftT) : cs.spec.aimPt;
					if (PlanFrom(sp, path, ti2, aim2, cs.spec.reach, cs.spec.jl, cs.spec.dodge, cs.spec.dj, cs.spec.power, again.plan)) p = again.plan;
					else if (dt != 0) continue;
					if (p.jumpTick >= 0) p.jumpTick = std::max(sp.tick, p.jumpTick + shift);
					if (p.dodge) p.dodgeLead = std::max(2, p.dodgeLead - shift);
					tries.push_back([&, p](int& res) { return RunFrom(arena, car, sp, p, st.maxTicks, st.ended, res, at); });
				}
			}
			else
			{
				// Your own part is replayed as you did it (from the nudged start); the timing shift tests the engine's part
				const std::vector<CarControls>* c = cs.inputs.get();
				tries.push_back([&, c](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, at, c); });
			}
			bool ok = false;
			SavePoint best;
			float bestScore = -1e9f;
			for (auto& t : tries)
			{
				int res = -1;
				SavePoint r = t(res);
				if (res != 1) continue;
				float sc = st.score(r);
				if (!ok || sc > bestScore) { best = std::move(r); bestScore = sc; ok = true; }
				if (!last) break;   // earlier steps: the first that works
			}
			if (!ok) { if (kStepDebug) fprintf(stderr, "rerun nudge %d shift %d: step %zu (%s) failed\n", nudge, shift, k, st.name); return false; }
			sp = std::move(best);
		}
		endAt = std::move(sp);
		return true;
	}

	// Shared pieces for the mechanics' steps
	float kWx = 892.755f - kBallR - 60.0f, kZHi = 642.775f - kBallR - 50.0f;

	// Where the ball is headed (straight line with gravity, no bounces): is it on target, how high, how hard
	void AimBall(const BallState& b, float gs, float& onTarget, float& height, float& speed)
	{
		Vec p = b.pos, v = b.vel;
		if (gs < 0) { p = Vec(-p.x, -p.y, p.z); v = Vec(-v.x, -v.y, v.z); }
		onTarget = 0; height = 0; speed = v.Length();
		if (v.y < 100.0f) return;
		float t = (5120.0f - p.y) / v.y;
		if (t > 3.0f) return;
		float x = p.x + v.x * t, z = std::max(93.0f, p.z + v.z * t - 325.0f * t * t);
		float miss = std::hypot(std::max(0.0f, std::fabs(x) - 780.0f), std::max(0.0f, z - 530.0f));
		onTarget = 1.0f - std::min(1.0f, miss / 200.0f);   // a near miss is still a miss
		height = z;
	}

	// Your recorded inputs, by tick
	std::vector<CarControls> YourInputs(const std::vector<Frame>& fr)
	{
		std::vector<CarControls> yours(fr.size());
		for (size_t i = 0; i < fr.size(); i++)
		{
			const Frame& f = fr[i];
			CarControls& c = yours[i];
			c.throttle = f.throttle; c.steer = f.steer; c.pitch = f.pitch; c.yaw = f.yaw; c.roll = f.roll;
			c.jump = f.jump; c.boost = f.boost; c.handbrake = f.handbrake;
		}
		return yours;
	}

	// Your way into a touch, as you did it and reshaped a little: jumps a couple of ticks earlier or later, the nose a
	// touch up/down/left/right into the touch, or less boost right before it. Only from the start of the search.
	std::function<void(const SavePoint&, std::vector<std::vector<CarControls>>&)> YourWayIn(const std::vector<Frame>& fr, int s, int touch)
	{
		std::vector<CarControls> yours = YourInputs(fr);
		return [yours, touch, s](const SavePoint& sp, std::vector<std::vector<CarControls>>& out) {
			if (sp.tick != s) return;
			const int n = static_cast<int>(yours.size());
			for (int shift : { 0, -2, 2, -4, 4 })
				for (int v = 0; v < 6; v++)
				{
					std::vector<CarControls> c = yours;
					if (shift != 0)
						for (int i = s + 1; i < std::min(n, touch + 2); i++)
						{
							int j = std::clamp(i - shift, s, n - 1);
							c[i].jump = yours[j].jump;
						}
					const int w0 = std::max(s, touch - 30), w1 = std::min(n, touch + 4);
					for (int i = w0; i < w1; i++)
					{
						if (v == 1) c[i].pitch = Bot::Clamp1(c[i].pitch + 0.15f);
						if (v == 2) c[i].pitch = Bot::Clamp1(c[i].pitch - 0.15f);
						if (v == 3) c[i].yaw = Bot::Clamp1(c[i].yaw + 0.15f);
						if (v == 4) c[i].yaw = Bot::Clamp1(c[i].yaw - 0.15f);
						if (v == 5 && i >= touch - 12) c[i].boost = false;
					}
					out.push_back(std::move(c));
				}
		};
	}

	// The shot: a touch into the goal from wherever the last step left off (top and low, middle and corners),
	// re-aimed from where the best tries really crossed the goal line
	StepDef ShotStep(float gs, int horizonTicks, bool highOnly = false)
	{
		StepDef shot;
		shot.name = "Shot";
		shot.maxTicks = horizonTicks + 50;
		shot.beam = 8;
		const float wx = kWx, zHi = kZHi;
		shot.plans = [gs, wx, zHi, horizonTicks, highOnly](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
			const int horizon = std::min(horizonTicks, static_cast<int>(path.size()) - 1);
			const Vec aims[6] = { Vec(0, gs * 5200.0f, zHi), Vec(wx, gs * 5200.0f, zHi), Vec(-wx, gs * 5200.0f, zHi),
				Vec(0, gs * 5200.0f, 300.0f), Vec(wx * 0.7f, gs * 5200.0f, 250.0f), Vec(-wx * 0.7f, gs * 5200.0f, 250.0f) };
			const int nAims = highOnly ? 3 : 6;   // where only high in the net counts (an air dribble's 4 and 5), aim high
			for (int ti = 4; ti <= horizon; ti += 3)
				for (int ai = 0; ai < nAims; ai++)
					for (const Vec& a : { aims[ai] })
					for (int v = 0; v < 4; v++)
						for (int jl : { 24, 60 })
							AddPlan(out, sp, path, ti, a, 150.0f, jl, (v & 1) == 1, true, (v & 2) != 0);
			// Off a carry the ball keeps most of its own speed, so a touch aimed at a point sends it low: aim the push
			// the ball actually needs to drop high into the net (the middle, or a top corner for the 5)
			if (highOnly)
				for (int ti = 4; ti <= horizon; ti += 3)
					for (float x : { 0.0f, 620.0f, -620.0f })
						for (float reach : { 140.0f, 165.0f })
							for (int pw = 0; pw < 2; pw++)
								AddLiftPlan(out, sp, path, ti, Vec(x, gs * 5120.0f, 515.0f), reach, 24, false, pw == 1);
		};
		shot.ended = [](const SavePoint& at, int stepStart) {
			if (at.lastTouch > stepStart && at.tick - at.lastTouch >= 6) return 1;
			return 0;
		};
		shot.aimFix = 16;
		shot.crossing = [gs](const SavePoint& at, Vec& cr) {
			Vec p = at.ball.pos, v = at.ball.vel;
			if (v.y * gs < 100.0f) return false;
			float t = (gs * 5120.0f - p.y) / v.y;
			if (t < 0.0f || t > 2.5f) return false;
			cr = Vec(p.x + v.x * t, gs * 5200.0f, std::max(93.0f, p.z + v.z * t - 325.0f * t * t));
			return true;
		};
		shot.score = [gs](const SavePoint& at) {
			float on, h, sp;
			AimBall(at.ball, gs, on, h, sp);
			return on * (1.0f + std::clamp((h - 150.0f) / 300.0f, 0.0f, 1.0f) + std::clamp((sp - 1400.0f) / 1400.0f, 0.0f, 1.0f));
		};
		return shot;
	}

	// ---- Each mechanic's step lists for the search (the rulebook's steps, with how to try them). Several lists =
	// several ways to play the mechanic (e.g. an air dribble with or without an extra carry touch). Empty = the
	// mechanic isn't step-searched yet, or the search starts too late for it. ----
	std::vector<std::vector<StepDef>> MechanicPlans(const ShotAnalysis& real, const std::vector<Frame>& fr, int s, float gs)
	{
		std::vector<std::vector<StepDef>> plans;
		if (!real.valid) return plans;

		if (real.type == ShotType::DoubleTap && real.setupTouchTick > s + 6)
		{
			std::vector<StepDef> steps;
			const int surface = real.tapSurface != 0 ? real.tapSurface : 1;
			// 1. Setup: send it into the backboard (the surface you used), somewhere a second touch can reach
			StepDef setup;
			setup.name = "Setup";
			setup.maxTicks = 760;
			setup.beam = 8;
			setup.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				const int horizon = std::min(300, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 6)
					for (float x : { -450.0f, 0.0f, 450.0f })
						for (float z : { 850.0f, 1150.0f })
							for (int jl : { 12, 24, 48, 84, 120 })
								for (int pw = 0; pw < 2; pw++)
								{
									AddPlan(out, sp, path, ti, Vec(x, gs * 5150.0f, z), 150.0f, jl, false, true, pw == 1);
								}
			};
			setup.ended = [surface](const SavePoint& at, int stepStart) {
				if (at.firstTouch < 0) return 0;
				if (at.touches > 1) return -1;                       // touched it again before the wall
				const int n = static_cast<int>(at.frames.size());
				if (n >= 3)
				{
					int surf = ShotGeometry::WallTapSurface(at.frames[n - 3], at.frames[n - 1]);
					if (surf == surface) return 1;
					if (surf != 0) return -4;                         // off a different surface
				}
				const Frame& f = at.frames.back();
				if (f.ballZ < 100.0f && f.ballVZ > 0.0f && at.tick - at.firstTouch > 10) return -2;   // hit the floor first
				if (at.tick - at.firstTouch > 420) return -3;   // long setups from midfield take a while to get there
				return 0;
			};
			// Your own setup first (you already know how to get it to the wall)
			setup.inputs = YourWayIn(fr, s, real.setupTouchTick);
			// After the setup touch: fly to where the ball will be a moment after it comes off the wall, lined up to hit
			// it at the goal (the "read")
			for (float after : { 0.25f, 0.45f, 0.65f })
				setup.follows.push_back([gs, after](int tick, const CarState& car, const BallState& b, Bot::Plan& f) {
					const float wallY = gs * (5120.0f - kBallR);
					if (b.vel.y * gs < 200.0f) return false;
					float t = (wallY - b.pos.y) / b.vel.y;
					if (t < 0.0f || t > 4.0f) return false;
					Vec P = b.pos + b.vel * t + Vec(0, 0, -325.0f * t * t);
					Vec vw = b.vel + Vec(0, 0, -650.0f * t);
					Vec vr(vw.x * 0.9f, -vw.y * 0.6f, vw.z * 0.9f);              // off the wall (rough bounce)
					Vec Q = P + vr * after + Vec(0, 0, -325.0f * after * after);
					if (Q.z < 140.0f) Q.z = 140.0f;
					Vec aim(0, gs * 5120.0f, 300.0f);
					Vec d = aim - Q; d = d / std::max(1.0f, d.Length());
					f.target = Q - d * 150.0f; f.hitDir = d;
					f.arriveTick = tick + static_cast<int>((t + after) * 120.0f);
					f.jumpTick = car.isOnGround ? tick : -1;
					f.dodge = false; f.doubleJump = false; f.power = false;
					return true;
				});
			setup.score = [](const SavePoint& at) {
				// A rebound you can get to: ball near the middle, not too high, car close, flip and boost left
				// High off the wall (time to read it before it drops), near the middle, car close and coming in slower than
				// the ball is coming out
				float dist = (at.ball.pos - at.car.pos).Length();
				float hi = 1.0f - std::min(2.0f, std::fabs(at.ball.pos.z - 800.0f) / 400.0f);   // high enough to read, low enough to come down
				return 1.5f * hi - dist / 1500.0f - std::fabs(at.ball.pos.x) / 1200.0f
					+ (at.car.HasFlipOrJump() ? 0.2f : 0.0f) + at.car.boost / 400.0f;
			};
			steps.push_back(setup);

			steps.push_back(ShotStep(gs, 170));
			plans.push_back(std::move(steps));
		}

		if (real.type == ShotType::AirDribble && real.airFirstTouch > s + 4)
		{
			// 1. Setup: off the ground or wall with the ball, into the first touch in the air (your way in first)
			StepDef setup;
			setup.name = "Setup";
			setup.maxTicks = 420;
			setup.beam = 8;
			setup.inputs = YourWayIn(fr, s, real.airFirstTouch);
			setup.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				// The bot's own pop: a soft touch up and toward the goal
				const int horizon = std::min(240, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 6)
					for (float up : { 1200.0f, 1800.0f })
						for (int jl : { 12, 24, 48, 84 })
						{
							Vec B = path[ti];
							Vec aim(B.x * 0.5f, gs * 5120.0f, B.z + up);
							AddPlan(out, sp, path, ti, aim, 160.0f, jl, false, true, false);
						}
			};
			setup.ended = [](const SavePoint& at, int stepStart) {
				if (at.firstTouch < 0 || at.tick - at.firstTouch < 6) return 0;
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;   // still on the floor: that's a pop, not the air touch yet
				return (Vec(f.ballX, f.ballY, f.ballZ) - Vec(f.carX, f.carY, f.carZ)).Length() < 450.0f ? 1 : -1;
			};
			setup.score = [gs](const SavePoint& at) {
				float dist = (at.ball.pos - at.car.pos).Length();
				float toward = at.ball.vel.y * gs / 1000.0f;
				// A first touch already in the air starts the chain (one touching it off the wall or ground doesn't count)
				const int ti = at.firstTouch - at.start;
				bool air = ti >= 1 && ti < static_cast<int>(at.frames.size()) && !ShotGeometry::OnSurface(at.frames[ti - 1]);
				// A good first touch (measured on Coop's 151 air dribbles: after one, the carry finishes high 51% of the
				// time, after the rest 10%): the ball up high (above 1,425) and touched soft (rising slower than ~500),
				// the car's nose up under it
				const float z = at.ball.pos.z, vz = at.ball.vel.z;
				const bool good = z > 1425.0f && vz <= 506.0f && dist < 300.0f;
				const float firstTouch = (good ? 1.0f : 0.0f) + 0.5f * std::clamp((z - 1425.0f) / 300.0f, -1.0f, 1.0f)
					- std::max(0.0f, vz - 506.0f) / 500.0f + 0.3f * at.car.rotMat.forward.z;
				return 2.0f * firstTouch - dist / 600.0f + 0.5f * std::clamp(toward, -1.0f, 1.0f) + at.car.boost / 200.0f + (air ? 0.5f : 0.0f);
			};

			// 2. Carry: another soft touch, keeping the ball close and moving toward the goal
			StepDef carry;
			carry.name = "Carry";
			carry.maxTicks = 140;
			carry.beam = 8;
			carry.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				const int horizon = std::min(100, static_cast<int>(path.size()) - 1);
				for (int ti = 6; ti <= horizon; ti += 2)
					for (float up : { 200.0f, 500.0f, 900.0f, 1400.0f })
						for (float reach : { 130.0f, 150.0f, 170.0f })
						{
							Vec B = path[ti];
							Vec aim(B.x * 0.6f, gs * 5120.0f, B.z + up);
							AddPlan(out, sp, path, ti, aim, reach, 24, false, false, false);
						}
			};
			for (float back : { 110.0f, 140.0f })
				for (float down : { 30.0f, 60.0f })
					for (float k : { 2.0f, 3.5f })
						carry.controllers.push_back(CarrySkill(gs, back, down, k));
			// ...or carried on an arc that would drop it high into the net (keeps the ball up instead of letting it sink)
			for (float back : { 110.0f, 140.0f })
				for (float k : { 2.0f, 3.5f })
					carry.controllers.push_back(CarryAim(gs, 0.0f, 515.0f, back, k));
			carry.maxTicks = 360;
			carry.ended = [gs](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;   // landed on the floor: the air dribble's over
				if (f.ballZ < 150.0f) return -1;
				float dist = (Vec(f.ballX, f.ballY, f.ballZ) - Vec(f.carX, f.carY, f.carZ)).Length();
				if (dist > 650.0f) return -1;                         // lost it
				if (at.lastTouch <= stepStart || at.tick - at.lastTouch < 8) return 0;
				// Touches in this step
				int touches = 0, last = -100;
				for (int i = std::max(0, stepStart - at.start); i < static_cast<int>(at.frames.size()); i++)
					if (at.frames[i].touch) { if (i - last > 8) touches++; last = i; }
				float toLine = 5120.0f - f.ballY * gs;
				// Carried close enough to shoot (or a long carry): the step's done
				if ((toLine < 1600.0f && dist < 400.0f) || touches >= 5) return 1;
				return 0;
			};
			carry.score = [gs](const SavePoint& at) {
				float dist = (at.ball.pos - at.car.pos).Length();
				float toward = at.ball.vel.y * gs / 1000.0f;
				return -dist / 600.0f + std::clamp(toward, -1.0f, 1.0f) + at.car.boost / 200.0f;
			};

			StepDef shot = ShotStep(gs, 120, true);
			// ...or carry it on into the shot: the same carry, pushed at the top of the net or a top corner
			for (float x : { 0.0f, 520.0f, -520.0f })
				for (float back : { 110.0f, 140.0f })
					for (float k : { 2.5f, 4.0f })
						shot.controllers.push_back(CarrySkill(gs, back, 70.0f, k, Vec(x, 0, 640.0f)));
			// Carried into a spot high in the net: the middle up high, or a top corner (the 5)
			for (float x : { 0.0f, 620.0f, -620.0f })
				for (float z : { 500.0f, 530.0f })
					for (float back : { 110.0f, 140.0f })
						for (float k : { 2.5f, 4.0f })
							shot.controllers.push_back(CarryAim(gs, x, z, back, k));
			// The shot has to come out of the carry: still in the air, no landing in between. A carry into the net keeps
			// touching it, so the step isn't over at the first gap between touches: it's over once the ball's let go
			// heading into the net (or in), or after a longer gap (the carry's done either way).
			auto cross = shot.crossing;
			shot.maxTicks = 260;
			// ...and a carry tuned from each spot to finish high in the net (a top corner scores extra: the 5)
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 450.0f, 125.0f, 40.0f, 2.5f, 0.0f, 0.0f, 0.0f };
				T->sd = { 300.0f, 300.0f, 30.0f, 50.0f, 1.0f, 0.3f, 0.0f, 0.0f };
				T->lo = { -700.0f, 0.0f, 90.0f, -60.0f, 1.0f, -0.5f, 0.0f, 0.0f };
				T->hi = { 700.0f, 1600.0f, 200.0f, 160.0f, 6.0f, 1.0f, 0.0f, 0.0f };
				T->make = [gs](const Knobs& x) { return ParamCarry(gs, x); };
				T->fitness = [cross, gs](const SavePoint& at) {
					float cx = 0, cz = 0;
					const Frame& f = at.frames.back();
					if (f.ballY * gs > 5120.0f)
					{
						bool found = false;
						for (size_t i = at.frames.size() - 1; i > 0; i--)
						{
							const Frame& a = at.frames[i - 1];
							const Frame& b = at.frames[i];
							if (a.ballY * gs < 5120.0f && b.ballY * gs >= 5120.0f)
							{
								float u = (5120.0f - a.ballY * gs) / std::max(1e-3f, (b.ballY - a.ballY) * gs);
								cx = a.ballX + (b.ballX - a.ballX) * u; cz = a.ballZ + (b.ballZ - a.ballZ) * u;
								found = true;
								break;
							}
						}
						if (!found) return -2000.0f;
					}
					else
					{
						Vec cr;
						if (!cross(at, cr)) return -2000.0f;
						cx = cr.x; cz = cr.z;
					}
					const bool in = std::fabs(cx) < 892.755f - kBallR && cz < 642.775f - kBallR + 10.0f;
					const float off = std::fabs(cz - 515.0f);
					if (!in) return -300.0f - off - std::max(0.0f, std::fabs(cx) - 800.0f);
					return 300.0f - off + (cz >= 480.0f ? 1000.0f : 0.0f) + (cz >= 480.0f && std::fabs(cx) >= 580.0f ? 300.0f : 0.0f);
				};
				shot.tune = T;
			}
			shot.ended = [cross, gs](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (at.lastTouch <= stepStart && ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;
				if (f.ballY * gs > 5120.0f + kBallR) return 1;
				if (at.lastTouch <= stepStart) return 0;
				const int since = at.tick - at.lastTouch;
				if (since < 6) return 0;
				Vec cr;
				if (cross(at, cr) && std::fabs(cr.x) < 800.0f && cr.z > 200.0f) return 1;
				return since >= 24 ? 1 : 0;
			};

			// Travel: carry it from far out to a good spot about 2,000 out for the finish (tuned from each spot toward
			// that handoff; the finish alone only works from close in)
			StepDef travel;
			travel.name = "Travel";
			travel.maxTicks = 480;
			travel.beam = 8;
			travel.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
			travel.ended = [gs](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (at.tick - stepStart > 30 && ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;
				if (f.ballZ < 150.0f) return -1;
				if ((Vec(f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ)).Length() > 700.0f) return -1;
				return 5120.0f - f.ballY * gs < 2200.0f ? 1 : 0;
			};
			travel.score = [gs](const SavePoint& at) { return HandoffScore(at, gs) / 1000.0f; };
			travel.controllers.push_back(ParamCarry(gs, { 0.0f, 900.0f, 125.0f, 40.0f, 2.5f, 0.0f, 0.0f, 0.0f }));
			travel.controllers.push_back(ParamCarry(gs, { 0.0f, 1300.0f, 140.0f, 60.0f, 2.0f, 0.2f, 0.0f, 0.0f }));
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 900.0f, 125.0f, 40.0f, 2.5f, 0.0f, 0.0f, 0.0f };
				T->sd = { 300.0f, 400.0f, 30.0f, 50.0f, 1.0f, 0.3f, 0.0f, 0.0f };
				T->lo = { -700.0f, 0.0f, 90.0f, -60.0f, 1.0f, -0.5f, 0.0f, 0.0f };
				T->hi = { 700.0f, 2000.0f, 200.0f, 160.0f, 6.0f, 1.0f, 0.0f, 0.0f };
				T->make = [gs](const Knobs& x) { return ParamCarry(gs, x); };
				T->fitness = [gs](const SavePoint& at) { return HandoffScore(at, gs); };
				travel.tune = T;
			}

			// A 4 needs a real chain (3+ touches in the air): the pop, a carry to a good spot, then the finish
			plans.push_back({ setup, travel, shot });
			plans.push_back({ setup, carry, shot });
			StepDef carry2 = carry;
			carry2.name = "Carry 2";
			plans.push_back({ setup, carry, carry2, shot });
		}

		// Flip reset (yours are off the wall, under the ball near the top of its arc, upside down): your takeoff, then
		// a reset skill tuned from where you are 40 ticks before your reset, then the shot (with the flip if you used it)
		// Your inputs replay closely through the takeoff but drift during the climb (about 140 off by 40 ticks before
		// your reset), so the hand-off to the reset skill is tried at three points, each its own list
		// Off for now (Oct 14): each piece works from your recorded states (the reset skill gets 27 of 30 from 40
		// ticks out, the flip shot scores about half the time after your resets), but chained in the search the
		// engine's own resets leave the ball where the shot can't follow yet
		constexpr bool kFlipResetSteps = false;
		std::vector<int> frHandoffs;
		if (kFlipResetSteps && real.type == ShotType::FlipReset && real.resetTick >= 0 && real.resetTakeoff >= 0)
			for (int h : { real.resetTakeoff + 20, real.resetTakeoff + 70, real.resetTick - 40 })
				if (h > s + 6 && h < real.resetTick - 10 && (frHandoffs.empty() || h > frHandoffs.back() + 15)) frHandoffs.push_back(h);
		for (const int frHandoff : frHandoffs)
		{
			StepDef setup;
			setup.name = "Setup";
			setup.maxTicks = frHandoff - s + 2;
			setup.beam = 8;
			setup.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
			setup.inputs = YourWayIn(fr, s, frHandoff);
			setup.ended = [frHandoff](const SavePoint& at, int) {
				if (at.tick < frHandoff) return 0;
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f)) return -1;   // still on the wall
				return (Vec(f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ)).Length() < 700.0f ? 1 : -1;
			};
			setup.score = [](const SavePoint& at) { return -(at.ball.pos - at.car.pos).Length() / 500.0f; };

			StepDef reset;
			reset.name = "Reset";
			reset.maxTicks = std::max(180, real.resetTick - frHandoff + 160);   // the climb from an early hand-off takes a while
			reset.beam = 8;
			reset.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
			reset.ended = [](const SavePoint& at, int stepStart) {
				// Over once the reset happened and the car is off the ball again (the shot starts clear of it)
				const int k = static_cast<int>(at.frames.size());
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;
				int resetAt = -1;
				for (int i = std::max(1, stepStart - at.start + 1); i < k; i++)
					if (ShotGeometry::IsFlipReset(at.frames[i - 1], at.frames[i])) { resetAt = i; break; }
				if (resetAt < 0) return 0;
				return k - resetAt >= 12 ? 1 : 0;                // a moment on: the shot takes it from here
			};
			auto afterReset = [gs](const SavePoint& at) {
				// A reset you can shoot from: soft (the ball keeps going where it was going, toward the goal), the ball
				// above the car and still close, high enough to have time
				float dist = (at.ball.pos - at.car.pos).Length();
				return std::clamp(at.ball.vel.y * gs, -600.0f, 1500.0f) / 500.0f - dist / 600.0f
					+ std::clamp((at.ball.pos.z - at.car.pos.z) / 100.0f, -1.0f, 1.0f) + std::clamp((at.ball.pos.z - 700.0f) / 600.0f, -1.0f, 1.0f);
			};
			reset.score = afterReset;
			const Knobs rmu = { 0.0f, 0.0f, 3.0f, 250.0f, 150.0f, 0.15f, 150.0f, 100.0f };
			reset.controllers.push_back(ResetSkill(gs, rmu));
			reset.controllers.push_back(ResetSkill(gs, { 0.4f, 0.0f, 3.0f, 250.0f, 150.0f, 0.15f, 150.0f, 100.0f }));
			reset.controllers.push_back(ResetSkill(gs, { 0.0f, 0.0f, 4.5f, 120.0f, 250.0f, 0.1f, 250.0f, 0.0f }));
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = rmu;
				T->sd = { 0.5f, 0.4f, 1.2f, 150.0f, 150.0f, 0.12f, 120.0f, 250.0f };
				T->lo = { -1.5f, -1.2f, 0.8f, 0.0f, -200.0f, 0.0f, 0.0f, -400.0f };
				T->hi = { 1.5f, 1.2f, 7.0f, 800.0f, 600.0f, 0.4f, 450.0f, 700.0f };
				T->pop = 20; T->iters = 4;
				T->make = [gs](const Knobs& x) { return ResetSkill(gs, x); };
				T->fitness = [afterReset](const SavePoint& at) { return 1000.0f + 100.0f * afterReset(at); };
				reset.tune = T;
			}

			StepDef shot = ShotStep(gs, 240);
			// The car may still be sitting on the ball after the reset: touches only count once it's come off it
			// (and, if you shot off the flip, once it has flipped)
			const bool needFlip = real.flipShot;
			shot.ended = [needFlip](const SavePoint& at, int stepStart) {
				const int k = static_cast<int>(at.frames.size());
				const int from = std::max(1, stepStart - at.start);
				// The touch that counts: the first after the flip (if you shot off the flip), else the first after the car
				// has come off the ball (four ticks without touching it)
				int after = -1;
				if (needFlip)
				{
					for (int i = from; i < k; i++) if (at.frames[i].jump && !at.frames[i - 1].jump && !at.frames[i - 1].onGround) { after = i; break; }
				}
				else
				{
					int free = 0;
					for (int i = from; i < k; i++) { free = at.frames[i].touch ? 0 : free + 1; if (free >= 4) { after = i; break; } }
				}
				const Frame& f = at.frames.back();
				if (after < 0) return (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) ? -1 : 0;
				int shotAt = -1;
				for (int i = after; i < k; i++) if (at.frames[i].touch) { shotAt = i; break; }
				if (shotAt < 0) return k - after > 80 ? -1 : 0;
				return k - 1 - shotAt >= 6 ? 1 : 0;
			};
			{
				auto cross = shot.crossing;
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 450.0f, 180.0f, 2.5f, 260.0f, 0.1f, 0.0f, 0.0f };
				T->sd = { 350.0f, 200.0f, 60.0f, 1.0f, 80.0f, 0.1f, 0.3f, 0.0f };
				T->lo = { -800.0f, 100.0f, 80.0f, 0.8f, 150.0f, 0.0f, -1.0f, 0.0f };
				T->hi = { 800.0f, 900.0f, 400.0f, 6.0f, 450.0f, 0.4f, 1.0f, 0.0f };
				T->pop = 20; T->iters = 5;
				T->make = [gs](const Knobs& x) { return FlipShot(gs, x); };
				T->fitness = [cross](const SavePoint& at) {
					Vec cr;
					if (!cross(at, cr)) return -2000.0f + std::clamp(at.ball.vel.y, -2000.0f, 2000.0f) * 0.0f;
					const bool in = std::fabs(cr.x) < 892.755f - kBallR && cr.z < 642.775f - kBallR + 10.0f;
					const float sp = at.ball.vel.Length();
					if (!in) return -300.0f - std::fabs(cr.z - 520.0f) - std::max(0.0f, std::fabs(cr.x) - 800.0f);
					return 300.0f - std::fabs(cr.z - 520.0f) + (cr.z >= 450.0f && sp >= 1700.0f ? 1000.0f : 0.0f)
						+ (cr.z >= 560.0f && sp >= 2400.0f ? 300.0f : 0.0f) + std::min(sp, 3000.0f) * 0.05f;
				};
				shot.tune = T;
			}
			{
				// You shot off the flip, so the line does too: only the plans that dodge into the ball
				auto all = shot.plans;
				shot.plans = [all](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
					std::vector<PlanSpec> tmp;
					all(sp, path, tmp);
					for (PlanSpec& p : tmp) if (p.dodge) out.push_back(std::move(p));
				};
			}
			plans.push_back({ setup, reset, shot });
		}
		return plans;
	}
}

const char* Engine::StatusText(EngineStatus s)
{
	switch (s)
	{
	case EngineStatus::NoBall:       return "Nothing to play from here.";
	case EngineStatus::NothingFound: return "The engine couldn't find a goal from here either.";
	case EngineStatus::Cancelled:    return "Stopped.";
	default:                         return "";
	}
}

// The engine from one start tick (Line below picks the start, and may try a second one)
static EngineResult LineAt(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	auto clock0 = std::chrono::steady_clock::now();
	auto elapsed = [&]() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - clock0).count(); };
	// The search gets about two thirds of the time; the rest is kept for checking its best lines hold up
	// (if the search ran to the end of the clock, nothing would get checked and nothing would be shown)
	auto cancelled = [&]() { return cancel && cancel->load(); };
	auto stop = [&]() { return cancelled() || elapsed() > maxSeconds * 0.65f; };
	auto hardStop = [&]() { return cancelled() || elapsed() > maxSeconds; };
	float scale = 1.0f;   // shrinks the search to fit the time on slower machines

	EngineResult out;
	const int n = static_cast<int>(fr.size());
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	int s = std::clamp(startTick, 1, n - 2);
	out.startTick = s;
	{
		RepSegments realSegs = Segmenter::Split(fr, eventTick >= 0 ? eventTick : n - 1);
		RepGrade rg = Grader::GradeRep(fr, realSegs, real, realIn, floorBeforeGoal, ref);
		out.realGrade = real.valid ? rg.overall : 0;
	}
	// Which goal: the one your shot went at (or the way the ball is heading)
	const Frame& f0 = fr[s];
	float gs = real.valid ? real.goalSign : (f0.ballVY >= 0 ? 1.0f : -1.0f);

	// Where the ball goes on its own from here (nobody touches it)
	const int horizon = 480;
	std::vector<Vec> ballPath;
	{
		std::unique_ptr<Arena> ballOnly(Arena::Create(GameMode::SOCCAR));
		BallState bs;
		bs.pos = Vec(f0.ballX, f0.ballY, f0.ballZ);
		bs.vel = Vec(f0.ballVX, f0.ballVY, f0.ballVZ);
		bs.angVel = Vec(f0.ballAVX, f0.ballAVY, f0.ballAVZ);
		ballOnly->ball->SetState(bs);
		ballPath.push_back(bs.pos);
		for (int i = 0; i < horizon; i++) { ballOnly->Step(1); ballPath.push_back(ballOnly->ball->GetState().pos); }
	}

	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	Car* car = arena->AddCar(Team::BLUE, Body(carBody));
	const int maxTick = std::min(s + horizon + 240, s + 900);
	const Vec p0(f0.carX, f0.carY, f0.carZ), v0(f0.carVX, f0.carVY, f0.carVZ);
	const bool startAir = !f0.onGround;

	// Aim points in the goal (ball center): the middle up high, the top corners, the low corners
	const float wx = 892.755f - kBallR - 60.0f, zHi = 642.775f - kBallR - 50.0f, zLo = kBallR + 60.0f;
	std::vector<Vec> aims = { Vec(0, gs * 5200.0f, zHi), Vec(wx, gs * 5200.0f, zHi), Vec(-wx, gs * 5200.0f, zHi),
		Vec(0, gs * 5200.0f, 330.0f), Vec(wx, gs * 5200.0f, zLo), Vec(-wx, gs * 5200.0f, zLo) };

	// aimPt: the point (x, z on the goal line) the hit is pointed at; the search corrects it from where shots really go
	struct Option { Bot::Plan plan; Vec aimPt; float reach = 150; int jumpLead = 60; };
	auto makePlan = [&](int T, Vec aimPt, float reach, int jumpLead, bool dodge, bool doubleJump, Option& o, bool power = false) -> bool {
		int ti = T - s;
		if (ti < 6 || ti > horizon) return false;
		Vec B = ballPath[ti];
		if (std::fabs(B.y) > 5100.0f) return false;   // already at the goal
		Vec A = aimPt;
		// Aim a little above the point to allow for the drop on the way (about 2,000 uu/s off the hit)
		float flight = (A - B).Length() / 2000.0f;
		A.z += 0.5f * 650.0f * flight * flight;
		Vec d = A - B;
		float dl = d.Length();
		if (dl < 1) return false;
		d = d / dl;
		Vec C = B - d * reach;
		bool ground = C.z < 60.0f;
		// From the ground, a short jump into it (lifts the ball: height in the net is what makes ground 4s and 5s)
		bool jumpShot = ground && jumpLead <= 30;
		if (jumpShot)
		{
			Vec dg = d; dg.z = 0;
			if (dg.Length() < 0.1f) return false;
			dg = dg / dg.Length();
			C = B - dg * reach;
			// How far under the ball the car meets it decides the lift: the aim height (which the aim correction
			// moves toward where the shot really went) sets it, so a shot that crossed too low gets hit lower next time
			float under = std::clamp((aimPt.z - 300.0f) * 0.25f, -40.0f, 110.0f);
			C.z = std::max(30.0f, B.z - under);
			ground = false;
		}
		else if (ground)
		{
			Vec dg = d; dg.z = 0;
			if (dg.Length() < 0.1f) return false;
			dg = dg / dg.Length();
			C = B - dg * reach;   // on the ground the hit is mostly flat; the car's center sits behind the ball
			C.z = 17.0f;
		}
		float tau = ti / 120.0f;
		if (ground)
		{
			Vec h = C - p0; h.z = 0;
			if (h.Length() / tau > 2300.0f && !startAir) return false;
		}
		else
		{
			Vec need = C - p0 - v0 * tau - Vec(0, 0, -0.5f * 650.0f * tau * tau);
			if (2.0f * need.Length() / (tau * tau) > 1250.0f) return false;
		}
		o.plan.target = C;
		o.plan.hitDir = d;
		o.plan.arriveTick = T;
		o.plan.jumpTick = ground ? -1 : std::max(s, T - jumpLead);   // (starting in the air: after landing, if it does)
		o.plan.dodge = dodge;
		o.plan.doubleJump = doubleJump && !dodge && !jumpShot;
		o.plan.dodgeLead = ground ? 14 : 8;
		o.plan.power = power;
		o.aimPt = aimPt;
		o.jumpLead = jumpLead;
		o.reach = reach;
		return true;
	};

	// Shot-type lock (the rulebook in Mechanics.h): the engine plays the mechanic you went for, the way you went for it.
	// A ground shot stays a ground shot, an aerial stays a real aerial (flown, not a jump-and-tip), a flip reset
	// still gets the reset and uses the flip, however early it starts.
	auto styleOk = [&](const SimRun& r) {
		if (r.touchTick < 0) return false;   // it never touched the ball: whatever happened next isn't the engine's shot
		if (!real.valid) return true;
		return Mechanics::SameMechanic(real, fr, r.shot, r.frames, s);
	};
	auto styledQuality = [&](const SimRun& r) { return Quality(r, ref) - (styleOk(r) ? 0.0f : 100000.0f); };

	struct Tried { Option o; SimRun run; float q; };
	std::vector<Tried> tried;
	auto tryOption = [&](const Option& o) {
		SimRun r = Run(arena.get(), car, fr, s, o.plan, maxTick, ref, 5000 + out.tries);
		out.tries++;
		float q = styledQuality(r);
		r.frames.clear();
		r.frames.shrink_to_fit();
		r.ctl.clear();
		r.ctl.shrink_to_fit();
		tried.push_back({ o, std::move(r), q });
	};

	// ---- Coarse: when to meet it, and how to get there (aimed at the middle, up high) ----
	int budget = depth >= 2 ? 4500 : 1000;   // the from-scratch bot's share of the sim runs
	std::vector<Option> coarse;
	for (int T = s + 12; T <= s + horizon; T += 4)
		for (int jl : { 12, 18, 24, 48, 84, 120, 168, 230 })
			for (int v = 0; v < 4; v++)
			{
				Option o;
				if (!makePlan(T, aims[0], 150.0f, jl, (v & 1) == 1, true, o, (v & 2) != 0)) continue;
				bool dup = false;
				for (const Option& c : coarse)
					if (c.plan.arriveTick == o.plan.arriveTick && c.plan.jumpTick == o.plan.jumpTick && c.plan.dodge == o.plan.dodge && c.plan.power == o.plan.power) { dup = true; break; }
				if (!dup) coarse.push_back(o);
			}
	if (coarse.empty()) { out.status = EngineStatus::NoBall; out.ms = elapsed() * 1000.0f; return out; }
	// Spread the coarse budget over the whole range of meeting times
	int coarseBudget = budget * 40 / 100;
	int step = std::max(1, static_cast<int>(coarse.size()) / std::max(1, coarseBudget));
	{
		// Time the first few runs on this machine, and shrink the whole search to fit the clock if it's slow
		auto c0 = std::chrono::steady_clock::now();
		int timed = 0;
		for (size_t i = 0; i < coarse.size() && timed < 12 && !stop(); i += step, timed++) tryOption(coarse[i]);
		float perRun = std::chrono::duration<float>(std::chrono::steady_clock::now() - c0).count() / std::max(1, timed);
		float planned = static_cast<float>(budget + (depth >= 2 ? 2500 + 4000 : 500 + 700) + 250);
		const int baseBudget = budget;
		scale = std::clamp(maxSeconds * 0.6f / std::max(1e-4f, perRun * planned), 0.12f, 1.0f);
		budget = std::max(120, static_cast<int>(budget * scale));
		coarseBudget = budget * 40 / 100;
		size_t start = static_cast<size_t>(timed) * step;
		step = std::max(1, static_cast<int>(coarse.size()) / std::max(1, coarseBudget));
		for (size_t i = start; i < coarse.size() && !stop(); i += step) tryOption(coarse[i]);
		// The first few runs are the slow ones (warming up), so measure again over the whole coarse pass. With time to
		// spare, the rest of the search (aiming, nudging, refining, the mechanic steps) grows to use it: the wait you
		// already sit through buys a deeper look, up to a few times the normal size.
		if (ScaleCap(depth) > 1.0f && out.tries > 40 && !stop())
		{
			float perRun2 = std::chrono::duration<float>(std::chrono::steady_clock::now() - c0).count() / std::max(1, out.tries);
			float use = maxSeconds * (depth >= 2 ? 0.55f : 0.4f) - elapsed();
			float grown = std::clamp(use / std::max(1e-4f, perRun2 * planned), 0.12f, ScaleCap(depth));
			if (grown > scale)
			{
				scale = grown;
				budget = std::max(budget, static_cast<int>(baseBudget * scale));
			}
		}
	}

	// ---- Aim correction on the best few ("shooting"): play it, see where the ball really crosses the goal line,
	// move the aim point by the miss, play it again. Each line, aimed at the middle up high and both top corners. ----
	std::mt19937 rng(777u + static_cast<unsigned>(n));
	auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	auto sameOption = [](const Option& a, const Option& b) {
		return a.plan.arriveTick == b.plan.arriveTick && a.plan.jumpTick == b.plan.jumpTick && a.plan.dodge == b.plan.dodge && a.plan.power == b.plan.power &&
			(a.aimPt - b.aimPt).Length() < 15.0f && std::fabs(a.reach - b.reach) < 1.0f;
	};
	auto seenOption = [&](const Option& o) { for (const Tried& t : tried) if (sameOption(t.o, o)) return true; return false; };
	auto correct = [&](Option o, Vec want, int iters) {
		for (int it = 0; it < iters && !stop() && out.tries < budget; it++)
		{
			if (seenOption(o)) break;
			tryOption(o);
			const SimRun& r = tried.back().run;
			if (r.touchTick < 0 || !r.shot.valid || !r.shot.reachesGoalLine) break;
			float ex = r.shot.crossX - want.x, ez = r.shot.crossZ - want.z;
			if (std::fabs(ex) < 40.0f && std::fabs(ez) < 40.0f) break;
			Vec next = o.aimPt - Vec(ex * 0.8f, 0, ez * 0.8f);
			next.x = std::clamp(next.x, -2500.0f, 2500.0f);
			next.z = std::clamp(next.z, -800.0f, 2500.0f);
			Option o2;
			if (!makePlan(o.plan.arriveTick, next, o.reach, o.jumpLead, o.plan.dodge, o.plan.doubleJump, o2, o.plan.power)) break;
			o = o2;
		}
	};
	{
		std::vector<int> order;
		for (int i = 0; i < static_cast<int>(tried.size()); i++)
			if (tried[i].run.touchTick >= 0 && tried[i].run.shot.valid && tried[i].run.shot.shotDir.y * gs > 0.2f) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		int topK = depth >= 2 ? 40 : 16;
		std::vector<Option> bases;
		for (int i = 0; i < static_cast<int>(order.size()) && static_cast<int>(bases.size()) < topK; i++) bases.push_back(tried[order[i]].o);
		for (const Option& b : bases)
			for (int w : { 0, 1, 2 })
			{
				if (stop() || out.tries >= budget) break;
				Option o;
				if (!makePlan(b.plan.arriveTick, aims[w], b.reach, b.jumpLead, b.plan.dodge, b.plan.doubleJump, o, b.plan.power)) continue;
				correct(o, aims[w], 4);
			}
	}

	// ---- Then nudge the best ones around: a little earlier or later, reach, jump timing, flip or not ----
	int guard = 0;
	while (out.tries < budget && !stop() && guard++ < budget * 4)
	{
		std::vector<int> order(tried.size());
		for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
		size_t top = std::min<size_t>(8, order.size());
		std::partial_sort(order.begin(), order.begin() + top, order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		const Option& par = tried[order[pick(0, static_cast<int>(top) - 1)]].o;
		int T = par.plan.arriveTick, jl = par.jumpLead;
		Vec aimPt = par.aimPt;
		float reach = par.reach;
		bool dodge = par.plan.dodge, dj = par.plan.doubleJump, power = par.plan.power;
		switch (pick(0, 5))
		{
		case 0: aimPt = aimPt + Vec(pick(-4, 4) * 40.0f, 0, pick(-4, 4) * 40.0f); break;
		case 1: T += pick(-8, 8); break;
		case 2: reach = std::clamp(reach + pick(-3, 3) * 10.0f, 110.0f, 200.0f); break;
		case 3: jl = std::max(12, jl + pick(-24, 24)); break;
		case 4: dodge = !dodge; dj = !dodge; break;
		case 5: power = !power; break;
		}
		Option o;
		if (!makePlan(T, aimPt, reach, jl, dodge, dj, o, power)) continue;
		if (!seenOption(o)) tryOption(o);
	}

	// ---- Refining a line (cross-entropy search): starting from a set of inputs, it reshapes the stick, steering,
	// throttle and boost over time and moves the jumps, keeping whatever plays out best. Run on your own line
	// (the best version of the shot you went for) and on the from-scratch bot's best lines (polishing them
	// toward a 4 or 5: more lift, more power, a cleaner line-up). ----
	struct CemTry { std::vector<CarControls> ctl; SimRun run; float q; int source; std::function<SimRun(int, int)> rerun; std::string note; std::vector<int> bounds; };   // source 0 your line, 1 the bot's, 2 the step search
	std::vector<CemTry> cemBest;   // the best few goals, inputs kept

	// ---- Multi-step mechanics: the step search (its lines join the refined ones and go through the same checks) ----
	std::string stepNote;
	std::vector<std::vector<StepDef>> mechPlans = MechanicPlans(real, fr, s, gs);   // kept alive: found lines are re-played from it
	if (!mechPlans.empty() && !stop())
	{
		std::unique_ptr<Arena> ballArena(Arena::Create(GameMode::SOCCAR));
		std::mt19937 srng(777u + static_cast<unsigned>(n));
		int runs = 0, replayed = 0, scored = 0;
		std::string per;
		for (size_t pi = 0; pi < mechPlans.size(); pi++)
		{
			const std::vector<StepDef>& mechSteps = mechPlans[pi];
			SavePoint sp0 = StartPoint(arena.get(), car, fr, s);
			std::vector<int> passed;
			const float until = maxSeconds * (0.45f * (pi + 1) / mechPlans.size());
			auto stepStop = [&]() { return cancelled() || elapsed() > until; };
			std::vector<SavePoint> finals = StepSearch(arena.get(), car, ballArena.get(), sp0, mechSteps,
				std::max(200, static_cast<int>((depth >= 2 ? 12000 : 3000) * scale / mechPlans.size())), srng, stepStop, runs, &passed);
			Bot::Plan none;
			none.arriveTick = maxTick;
			for (SavePoint& f : finals)
			{
				if (hardStop()) break;
				std::vector<CarControls> c = f.ctl;
				c.resize(maxTick + 1);   // after the last step: hands off
				SimRun r = PlayOut(arena.get(), car, fr, f, maxTick, ref);
				replayed++;
				if (kStepDebug && f.tick < static_cast<int>(r.frames.size()))
				{
					for (int t = s; t <= f.tick && t - s < static_cast<int>(f.frames.size()); t++)
					{
						const Frame& a = f.frames[t - s];
						const Frame& b2 = r.frames[t];
						float d = (Vec(a.ballX, a.ballY, a.ballZ) - Vec(b2.ballX, b2.ballY, b2.ballZ)).Length() + (Vec(a.carX, a.carY, a.carZ) - Vec(b2.carX, b2.carY, b2.carZ)).Length();
						if (d > 0.5f)
						{
							std::string bs;
							for (int b : f.bounds) bs += std::to_string(b) + " ";
							fprintf(stderr, "  first divergence at tick %d (start %d), bounds %s; touch a %d b %d onGround a %d b %d\n", t, s, bs.c_str(), (int)a.touch, (int)b2.touch, (int)a.onGround, (int)b2.onGround);
							break;
						}
					}
					const Frame& g = r.frames[f.tick];
					fprintf(stderr, "replay check: ball off by %.0f, car off by %.0f at tick %d; replay in %d grade %d\n",
						(Vec(g.ballX, g.ballY, g.ballZ) - f.ball.pos).Length(), (Vec(g.carX, g.carY, g.carZ) - f.car.pos).Length(), f.tick, (int)r.wentIn, r.grade.overall);
				}
				if (kStepDebug && !r.wentIn)
					fprintf(stderr, "final miss: reaches %d cross %.0f %.0f miss %.0f %s | air touches %d impulse %.0f close %.2f type %d\n", (int)r.shot.reachesGoalLine, r.shot.crossX, r.shot.crossZ,
						r.shot.missDist, r.shot.missDir.c_str(), r.shot.airTouches, r.shot.airImpulse, r.shot.airCloseFrac, (int)r.shot.type);
				if (kStepDebug && r.wentIn)
					fprintf(stderr, "final goal at %.0f %.0f speed %.0f impulse %.0f\n", r.shot.crossX, r.shot.crossZ, r.shot.shotSpeed, r.shot.airImpulse);
				if (kStepDebug && r.wentIn)
				{
					std::string why;
					bool same = Mechanics::SameMechanic(real, fr, r.shot, r.frames, s, &why);
					fprintf(stderr, "final goal: grade %d (you %d) type %d same %d %s | air touches %d close %.2f (you %.2f) | %s\n", r.grade.overall, out.realGrade,
						(int)r.shot.type, (int)same, why.c_str(), r.shot.airTouches, r.shot.airCloseFrac, real.airCloseFrac, r.grade.overallWhy.c_str());
				}
				float q = styledQuality(r);
				if (!r.wentIn) continue;
				scored++;
				r.frames.clear();
				r.frames.shrink_to_fit();
				// Robustness for these lines = the plan still works from a nudged start with the timing off (re-aimed
				// closed-loop like a player would), not that the exact same button presses still work
				auto line = std::make_shared<SavePoint>(f);
				auto rerun = [&, line, pi](int nudge, int shift) {
					if (nudge == 0 && shift == 0) return PlayOut(arena.get(), car, fr, *line, maxTick, ref);   // the line itself
					SavePoint endAt;
					if (!RerunChain(arena.get(), car, fr, s, *line, mechPlans[pi], nudge, shift, endAt)) { SimRun miss; miss.touchTick = -1; return miss; }
					return PlayOut(arena.get(), car, fr, endAt, maxTick, ref);
				};
				cemBest.push_back({ std::move(c), std::move(r), q, 2, rerun, f.note, f.bounds });
			}
			if (kStepDebug) { for (int k = 0; k < 4; k++) fprintf(stderr, "list %zu step%d: no touch %d, passed %d, fail1 %d, fail2 %d, fail3 %d, fail4 %d, ran out %d\n", pi, k, gStepDbg[k*8], gStepDbg[k*8+1], gStepDbg[k*8+2], gStepDbg[k*8+3], gStepDbg[k*8+4], gStepDbg[k*8+5], gStepDbg[k*8+7]); for (int& x : gStepDbg) x = 0; }
			per += pi ? " | " : "";
			for (size_t k = 0; k < passed.size(); k++) per += std::format("{}{} {}", k ? ", " : "", mechSteps[k].name, passed[k]);
		}
		out.tries += runs + replayed;
		std::sort(cemBest.begin(), cemBest.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
		stepNote = std::format("Step search: {} tries; passed {}; {} of {} finished lines scored.", runs, per, scored, replayed);
	}
	std::mt19937 crng(4242u + static_cast<unsigned>(n));
	std::normal_distribution<float> N01(0.0f, 1.0f);
	auto refine = [&](const std::vector<CarControls>& seed, int touch, int cemBudget, int source) {
		const int m = static_cast<int>(seed.size());
		const int w0 = std::max(s, touch - 180), w1 = std::min(m - 1, touch + 24);
		if (w1 - w0 < 20) return;
		const int knotGap = 15, K = (w1 - w0) / knotGap + 2;
		std::vector<std::pair<int, int>> jumps;   // jump presses in the window: start, length
		for (int i = std::max(w0, 1); i < w1 && jumps.size() < 4; i++)
			if (seed[i].jump && !seed[i - 1].jump) { int len = 0; while (i + len < m && seed[i + len].jump) len++; jumps.push_back({ i, len }); }
		const int D = K * 6 + static_cast<int>(jumps.size());
		std::vector<float> mean(D, 0.0f), sigma(D);
		for (int d = 0; d < D; d++) sigma[d] = d >= K * 6 ? 6.0f : (d % 6 == 5 ? 0.6f : 0.35f);
		auto build = [&](const std::vector<float>& x) {
			std::vector<CarControls> c = seed;
			for (size_t j = 0; j < jumps.size(); j++)
				for (int i = jumps[j].first; i < std::min(m, jumps[j].first + jumps[j].second); i++) c[i].jump = false;
			for (size_t j = 0; j < jumps.size(); j++)
			{
				int at = std::clamp(jumps[j].first + static_cast<int>(std::lround(x[K * 6 + j])), s + 1, m - 2), len = jumps[j].second;
				for (int i = at; i < std::min(m, at + len); i++) c[i].jump = true;
				if (at - 1 >= w0 && c[at - 1].jump) c[at - 1].jump = false;   // keep it a fresh press
			}
			for (int i = w0; i < w1; i++)
			{
				float f = static_cast<float>(i - w0) / knotGap;
				int k0 = std::min(K - 2, static_cast<int>(f));
				float t = f - k0;
				auto off = [&](int dim) { return x[k0 * 6 + dim] * (1 - t) + x[(k0 + 1) * 6 + dim] * t; };
				c[i].pitch = Bot::Clamp1(c[i].pitch + off(0));
				c[i].yaw = Bot::Clamp1(c[i].yaw + off(1));
				c[i].roll = Bot::Clamp1(c[i].roll + off(2));
				c[i].steer = Bot::Clamp1(c[i].steer + off(3));
				c[i].throttle = Bot::Clamp1(c[i].throttle + off(4));
				float b = off(5);
				if (b > 0.4f) c[i].boost = true;
				if (b < -0.4f) c[i].boost = false;
			}
			return c;
		};
		Bot::Plan none;
		none.arriveTick = touch + 120;
		const int pop = depth >= 2 ? 64 : 40, elites = 8;
		int cemTries = 0;
		std::vector<float> bestX(D, 0.0f);
		float bestQ = -1e12f;
		while (cemTries < cemBudget && !stop())
		{
			std::vector<std::pair<float, std::vector<float>>> gen;
			for (int mm = 0; mm < pop && cemTries < cemBudget && !stop(); mm++)
			{
				std::vector<float> x(D);
				if (mm == 0) x = bestX;   // keep the best so far in every round
				else
					for (int d = 0; d < D; d++)
					{
						float lim = d >= K * 6 ? 30.0f : 1.6f;
						x[d] = std::clamp(mean[d] + sigma[d] * N01(crng), -lim, lim);
					}
				std::vector<CarControls> c = build(x);
				// Each try from a slightly different start, so the search favors lines that hold up, not lucky ones
				SimRun r = Run(arena.get(), car, fr, s, none, maxTick, ref, 1000 + out.tries, &c);
				cemTries++;
				out.tries++;
				float q = styledQuality(r);
				if (q > bestQ) { bestQ = q; bestX = x; }
				if (r.wentIn)
				{
					r.frames.clear();
					r.frames.shrink_to_fit();
					cemBest.push_back({ std::move(c), std::move(r), q, source });
					std::sort(cemBest.begin(), cemBest.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
					if (cemBest.size() > 40) cemBest.pop_back();
				}
				gen.push_back({ q, std::move(x) });
			}
			if (gen.size() < static_cast<size_t>(elites)) break;
			std::sort(gen.begin(), gen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (int d = 0; d < D; d++)
			{
				float mu = 0, v = 0;
				for (int e = 0; e < elites; e++) mu += gen[e].second[d];
				mu /= elites;
				for (int e = 0; e < elites; e++) v += (gen[e].second[d] - mu) * (gen[e].second[d] - mu);
				mean[d] = mu;
				float floorSigma = d >= K * 6 ? 1.0f : 0.04f;
				sigma[d] = std::max(floorSigma, 0.9f * std::sqrt(v / elites));
			}
		}
	};
	// Your line
	if (real.valid && !stop())
	{
		std::vector<CarControls> realCtl(n);
		for (int i = 0; i < n; i++)
		{
			const Frame& f = fr[i];
			realCtl[i].throttle = f.throttle; realCtl[i].steer = f.steer; realCtl[i].pitch = f.pitch; realCtl[i].yaw = f.yaw;
			realCtl[i].roll = f.roll; realCtl[i].jump = f.jump; realCtl[i].boost = f.boost; realCtl[i].handbrake = f.handbrake;
		}
		refine(realCtl, real.touchTick, std::max(60, static_cast<int>((depth >= 2 ? 2500 : 500) * scale)), 0);
	}
	// The bot's best two lines that reached the ball, polished
	{
		std::vector<int> order;
		for (int i = 0; i < static_cast<int>(tried.size()); i++) if (tried[i].run.touchTick >= 0) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		for (int i = 0; i < std::min<int>(2, static_cast<int>(order.size())) && !stop(); i++)
		{
			SimRun seedRun = Run(arena.get(), car, fr, s, tried[order[i]].o.plan, maxTick, ref, 0);
			out.tries++;
			if (seedRun.touchTick < 0 || seedRun.ctl.empty()) continue;
			std::vector<CarControls> seed = seedRun.ctl;
			seed.resize(std::max<size_t>(seed.size(), static_cast<size_t>(maxTick) + 1));
			refine(seed, seedRun.touchTick, std::max(40, static_cast<int>((depth >= 2 ? 2000 : 350) * scale)), 1);
		}
	}

	// ---- Pick the best goal (from either engine) that also works from slightly nudged starts ----
	struct Pick { float q; int botIdx; int cemIdx; };
	std::vector<Pick> picks;
	for (int i = 0; i < static_cast<int>(tried.size()); i++) if (tried[i].run.wentIn) picks.push_back({ tried[i].q, i, -1 });
	for (int i = 0; i < static_cast<int>(cemBest.size()); i++) picks.push_back({ cemBest[i].q, -1, i });
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.q > b.q; });
	// shift: a human's timing is never frame-perfect, so the checks also jump / press everything a tick or two early or late
	auto replay = [&](const Pick& pk, int nudge, int shift = 0) {
		if (pk.botIdx >= 0)
		{
			Bot::Plan p = tried[pk.botIdx].o.plan;
			if (p.jumpTick >= 0) p.jumpTick = std::max(s, p.jumpTick + shift);
			return Run(arena.get(), car, fr, s, p, maxTick, ref, nudge);
		}
		if (cemBest[pk.cemIdx].rerun) return cemBest[pk.cemIdx].rerun(nudge, shift);
		Bot::Plan none;
		none.arriveTick = (real.valid ? real.touchTick : s) + 120;
		if (shift == 0) return Run(arena.get(), car, fr, s, none, maxTick, ref, nudge, &cemBest[pk.cemIdx].ctl, cemBest[pk.cemIdx].bounds.empty() ? nullptr : &cemBest[pk.cemIdx].bounds);
		const std::vector<CarControls>& c0 = cemBest[pk.cemIdx].ctl;
		std::vector<CarControls> c = c0;
		for (int i = s; i < static_cast<int>(c.size()); i++) { int j = std::clamp(i - shift, s, static_cast<int>(c0.size()) - 1); c[i] = c0[j]; }
		return Run(arena.get(), car, fr, s, none, maxTick, ref, nudge, &c);
	};
	static const int kShift[kNudges] = { -2, 2, -1, 1, 0, 0 };
	// Your takeoff (only if you started on the ground) and boost (from the button, since free play refills it)
	const bool startedGrounded = ShotGeometry::OnSurface(fr[s]);
	int yourTakeoff = -1;
	if (startedGrounded)
		for (int i = s + 1; i < n; i++) if (fr[i].jump && !fr[i - 1].jump && fr[i - 1].onGround) { yourTakeoff = i; break; }
	auto boostTicks = [](const std::vector<Frame>& f, int from, int to) {
		int b = 0;
		for (int i = std::max(0, from); i < std::min(static_cast<int>(f.size()), to); i++) b += f[i].boost ? 1 : 0;
		return b * (100.0f / 3.0f) / 120.0f;   // a full tank lasts 3 s
	};
	float yourBoost = real.valid ? boostTicks(fr, s, real.touchTick) : 0.0f;
	int checked = 0;
	for (const Pick& pk : picks)
	{
		if (checked >= (depth >= 2 ? 150 : 60) || cancelled() || (hardStop() && checked >= 3)) break;
		if (pk.q < -50000.0f) continue;   // not the kind of shot you went for
		checked++;
		int expect = pk.botIdx >= 0 ? tried[pk.botIdx].run.grade.overall : cemBest[pk.cemIdx].run.grade.overall;
		if (expect <= out.realGrade) continue;   // only lines that beat your shot
		int held = 0;
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun r = replay(pk, 300 + j, kShift[j - 1]);
			out.tries++;
			held += r.wentIn && r.grade.overall >= expect - 1;
			if (j - held > 2) break;
		}
		if (kStepDebug && pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2) fprintf(stderr, "verify step-search line: expect %d (you %d), held %d\n", expect, out.realGrade, held);
		if (held < 4) continue;
		SimRun win = replay(pk, 0);
		if (kStepDebug && pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2) fprintf(stderr, "  win: in %d grade %d style %d touch %d\n", (int)win.wentIn, win.grade.overall, (int)styleOk(win), win.touchTick);
		if (!win.wentIn || win.grade.overall <= out.realGrade || !styleOk(win)) continue;
		out.status = EngineStatus::Found;
		out.grade = win.grade.overall;
		out.wentIn = win.wentIn;
		out.gradeWhy = win.grade.overallWhy;
		out.held = held;
		out.heldOf = kNudges;
		out.eventTick = win.eventTick;
		out.touchTick = win.touchTick;
		const Frame& hit = win.frames[std::min(static_cast<int>(win.frames.size()) - 1, std::max(0, win.touchTick))];
		win.boostUsed = boostTicks(win.frames, s, win.touchTick);
		if (!startedGrounded) win.takeoff = -1;

		// The cue: the one or two biggest differences from what you did, in words (the numbers are on the hover)
		struct Diff { float size; std::string say; };
		std::vector<Diff> diffs;
		auto amount = [](float size) { return size >= 3.0f ? "a lot " : (size >= 1.6f ? "" : "a bit "); };
		// Where on the ball the car hits it (relative to the ball's center, in the car's frame at contact)
		auto contact = [](const Frame& f, float& under, float& side) {
			RotMat R = Angle(f.carYaw * kU2R, f.carPitch * kU2R, f.carRoll * kU2R).ToRotMat();
			Vec rel = Vec(f.touchX - f.ballX, f.touchY - f.ballY, f.touchZ - f.ballZ);
			under = -rel.z; side = rel.Dot(R.right);
		};
		if (real.valid)
		{
			const Frame& yh = fr[real.touchTick];
			if (win.takeoff >= 0 && yourTakeoff >= 0 && std::abs(win.takeoff - yourTakeoff) >= 6)
			{
				float sz = std::abs(win.takeoff - yourTakeoff) / 8.0f;
				diffs.push_back({ sz, std::format("jump {}{}", amount(sz), win.takeoff < yourTakeoff ? "earlier" : "later") });
			}
			if (win.takeoff >= 0 && yourTakeoff < 0) diffs.push_back({ 5.0f, "jump for it" });
			if (win.takeoff < 0 && yourTakeoff >= 0) diffs.push_back({ 5.0f, "stay on the ground" });
			if (hit.touch && yh.touch)
			{
				float eu, es, yu, ys;
				contact(hit, eu, es); contact(yh, yu, ys);
				if (std::fabs(eu - yu) >= 15.0f)
				{
					float sz = std::fabs(eu - yu) / 20.0f;
					diffs.push_back({ sz, eu > yu ? std::format("get {}more under it", amount(sz)) : std::format("hit it {}flatter", amount(sz)) });
				}
				if (std::fabs(es - ys) >= 15.0f)
				{
					float sz = std::fabs(es - ys) / 20.0f;
					diffs.push_back({ sz, std::format("hit it {}more on its {} side", amount(sz), es > ys ? "right" : "left") });
				}
			}
			if (std::abs(win.touchTick - real.touchTick) >= 8)
			{
				float sz = std::abs(win.touchTick - real.touchTick) / 10.0f;
				diffs.push_back({ sz, std::format("meet it {}{}", amount(sz), win.touchTick < real.touchTick ? "sooner" : "later") });
			}
			if (std::fabs(win.boostUsed - yourBoost) >= 10.0f)
			{
				float sz = std::fabs(win.boostUsed - yourBoost) / 15.0f;
				diffs.push_back({ sz, win.boostUsed > yourBoost ? std::format("{}more boost", amount(sz)) : std::format("{}less boost", amount(sz)) });
			}
			if (win.shot.shotSpeed - real.shotSpeed >= 300.0f)
			{
				float sz = (win.shot.shotSpeed - real.shotSpeed) / 400.0f;
				diffs.push_back({ sz, std::format("hit it {}harder", amount(sz)) });
			}
		}
		// An air dribble found by the step search is judged on the carry, not on one hit (how many touches, how
		// close, how high it went in): until the step-by-step cues land, say it in those words
		if (real.valid && real.type == ShotType::AirDribble && pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && win.shot.airDribble)
		{
			diffs.clear();
			const int more = win.shot.airTouches - real.airTouches;
			if (more >= 1) diffs.push_back({ 1.0f + more / 2.0f, std::format("keep touching it ({} touches in the air, you had {})", win.shot.airTouches, real.airTouches) });
			if (win.shot.airCloseFrac - real.airCloseFrac >= 0.15f)
				diffs.push_back({ (win.shot.airCloseFrac - real.airCloseFrac) * 8.0f, "keep it closer to your nose" });
			if (win.shot.reachesGoalLine && real.reachesGoalLine && win.shot.crossZ - real.crossZ >= 60.0f)
				diffs.push_back({ (win.shot.crossZ - real.crossZ) / 60.0f, std::format("carry it in {}higher", amount((win.shot.crossZ - real.crossZ) / 60.0f)) });
			if (win.shot.reachesGoalLine && std::fabs(win.shot.crossX) >= 580.0f && std::fabs(real.crossX) < 580.0f)
				diffs.push_back({ 2.0f, "take it to the corner" });
		}
		std::sort(diffs.begin(), diffs.end(), [](const Diff& a, const Diff& b) { return a.size > b.size; });
		std::vector<std::string> parts;
		for (const Diff& d : diffs) if (parts.size() < 2 && d.size >= 0.75f) parts.push_back(d.say);
		bool yourLine = pk.botIdx < 0 && cemBest[pk.cemIdx].source == 0;
		std::string kind = yourLine ? "Engine (your line, sharpened)" : "Engine";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && !cemBest[pk.cemIdx].note.empty()) kind = "Engine (" + cemBest[pk.cemIdx].note + ")";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && real.valid && real.type == ShotType::FlipReset) kind = "Engine (your takeoff, its reset and shot)";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && real.valid && real.type == ShotType::AirDribble)
			kind = cemBest[pk.cemIdx].note == "your setup, a better second touch" ? "Engine (your setup, then carried in)" : "Engine (carried in)";
		std::string joined;
		for (size_t i = 0; i < parts.size(); i++) joined += (i == 0 ? "" : " and ") + parts[i];
		if (joined.empty()) joined = "same idea, cleaner contact";
		out.cue = kind + ": " + joined;
		if (!out.cue.empty()) out.cue[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out.cue[0])));

		std::string lines;
		if (win.takeoff >= 0) lines += std::format("Takes off at {} ({}).\n", Secs(win.takeoff - s), yourTakeoff >= 0 ? "you: " + Secs(yourTakeoff - s) : std::string("you didn't jump"));
		if (real.valid)
		{
			const Frame& yh = fr[real.touchTick];
			lines += std::format("Meets the ball at {}, {:.0f} high (you: {}, {:.0f} high).\n", Secs(win.touchTick - s), hit.ballZ, Secs(real.touchTick - s), yh.ballZ);
			lines += std::format("Ball off the hit at {} (you: {}).\n", Units::Speed(win.shot.shotSpeed), Units::Speed(real.shotSpeed));
			if (hit.touch && yh.touch)
			{
				float eu, es, yu, ys;
				contact(hit, eu, es); contact(yh, yu, ys);
				lines += std::format("Contact {:.0f} under the ball's center and {:.0f} {} (you: {:.0f} under, {:.0f} {}).\n",
					eu, std::fabs(es), es >= 0 ? "right" : "left", yu, std::fabs(ys), ys >= 0 ? "right" : "left");
			}
		}
		lines += std::format("Boost used: {:.0f} (you: {:.0f}).\n", win.boostUsed, yourBoost);
		out.compare = lines;
		out.detail = std::format("{}Would be a {} ({}).\n{}It plays perfect inputs from {} into your rep: it shows the ceiling for this shot. "
			"It scored from {} of {} slightly different starts, with the timing a tick or two early or late.", lines, out.grade, out.gradeWhy,
			yourLine ? "The engine started from your own inputs and reshaped them. " : "The engine played this from scratch (none of your inputs). ",
			Secs(s - (real.valid ? real.startTick : 0)), held, kNudges);
		out.frames = std::move(win.frames);
		break;
	}
	if (out.status != EngineStatus::Found)
		out.status = (cancel && cancel->load()) ? EngineStatus::Cancelled : EngineStatus::NothingFound;
	int touched = 0, goals = 0, high = 0;
	for (const Tried& t : tried) { touched += t.run.touchTick >= 0; goals += t.run.wentIn; high += t.run.wentIn && t.run.grade.overall >= 4; }
	for (const CemTry& c : cemBest) high += c.run.grade.overall >= 4;
	out.note = std::format("{} lines tried: {} touched the ball, {} scored, {} as a 4 or 5. {:.1f} s.{}", tried.size(), touched, goals, high, elapsed(),
		scale < 0.99f ? std::format(" This computer is slower, so the search was sized down to {:.0f}% to fit the time.", scale * 100.0f)
		: scale > 1.01f ? std::format(" It had time to spare, so it searched {:.1f}x deeper.", scale) : std::string());
	if (!stepNote.empty()) out.note += " " + stepNote;
	out.ms = elapsed() * 1000.0f;
	return out;
}

static EngineResult LineOnce(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth);

EngineResult Engine::Line(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	if (depth < 2) return LineOnce(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, startTick, 1);
	// Thinking harder: the normal search first, then the big one, so it never comes back with less
	auto t0 = std::chrono::steady_clock::now();
	EngineResult quick = LineOnce(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds * 0.25f, startTick, 1);
	float used = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
	if (cancel && cancel->load()) return quick;
	EngineResult deep = LineOnce(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, std::max(1.0f, maxSeconds - used), startTick, 2);
	bool qf = quick.status == EngineStatus::Found, df = deep.status == EngineStatus::Found;
	EngineResult& best = (df && (!qf || deep.grade > quick.grade)) ? deep : quick;
	best.tries = quick.tries + deep.tries;
	return std::move(best);
}

static EngineResult LineOnce(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	const int n = static_cast<int>(fr.size());
	if (n < 4) { EngineResult r; r.status = EngineStatus::NoBall; return r; }
	if (startTick >= 0) return LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, startTick, depth);

	// Default start: half a second before your touch, so the engine plays the shot you were already lined up for
	// ("Engine from here" in the viewer picks any other moment, e.g. the start of the rep for a different setup)
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	int s = real.valid ? std::max(real.startTick, real.touchTick - 60) : 1;
	return LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, s, depth);
}
