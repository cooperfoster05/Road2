// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "Engine.h"
#include "Grader.h"
#include "Mechanics.h"
#include "Segmenter.h"
#include "ShotGeometry.h"
#include "Units.h"

#include "third_party/RocketSim/src/RocketSim.h"

#include <algorithm>
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
	SimRun Run(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const Bot::Plan& plan, int maxTick,
		const ShotReference& ref, int nudge, const std::vector<CarControls>* fixedCtl = nullptr)
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

	// The exact game state at the end of a step
	struct SavePoint
	{
		CarState car;
		BallState ball;
		int tick = 0;                    // the tick this state is at
		std::vector<CarControls> ctl;    // the line's inputs by tick, from the search start up to here
		std::vector<Frame> frames;       // the line's frames from the search start (frames[0] = the start)
		int start = 0;                   // the search start tick (frames[i] is tick start + i)
		int touches = 0, firstTouch = -1, lastTouch = -1;
		float score = 0;                 // the step's own score (higher is better), for keeping the best few
		std::string note;                // what this branch did, for the cue later
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
		LoadState(car, arena->ball, fr, s);
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

	SavePoint RunFrom(Arena* arena, Car* car, const SavePoint& sp, const Bot::Plan& plan, int maxTicks, const StepEnd& ended, int& result,
		const AfterTouch* afterTouch = nullptr)
	{
		SavePoint r = sp;
		car->SetState(sp.car);
		arena->ball->SetState(sp.ball);
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
		const bool startAir = !sp.car.isOnGround;
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
		const Vec p0 = sp.car.pos, v0 = sp.car.vel;
		if (ground)
		{
			Vec h = C - p0; h.z = 0;
			if (h.Length() / tau > 2300.0f) return false;
		}
		else
		{
			Vec need = C - p0 - v0 * tau - Vec(0, 0, -0.5f * 650.0f * tau * tau);
			if (2.0f * need.Length() / (tau * tau) > 1250.0f) return false;
		}
		plan.target = C;
		plan.hitDir = d;
		plan.arriveTick = sp.tick + ti;
		plan.jumpTick = (ground || startAir) ? -1 : std::max(sp.tick, sp.tick + ti - jumpLead);
		if (!ground && !startAir && plan.jumpTick < 0) plan.jumpTick = sp.tick;
		plan.dodge = dodge;
		plan.doubleJump = doubleJump && !dodge;
		plan.dodgeLead = ground ? 14 : 8;
		plan.power = power;
		return true;
	}

	// Debug counters for the step search (test builds define ENGINE_STEP_DEBUG; never on in the plugin)
#ifdef ENGINE_STEP_DEBUG
	const bool kStepDebug = std::getenv("ENG_STEPDBG") != nullptr;
#else
	constexpr bool kStepDebug = false;
#endif
	int gStepDbg[16] = {};

	// One step of a mechanic for the search: its candidate plans from a save point, when it's over, and how good
	// the save point at its end is (for keeping the best few)
	struct StepDef
	{
		const char* name;
		std::function<void(const SavePoint&, const std::vector<Vec>& ballPath, std::vector<Bot::Plan>& out)> plans;
		StepEnd ended;
		std::function<float(const SavePoint&)> score;
		int maxTicks = 300;
		int beam = 8;
		AfterTouch afterTouch;          // empty = let go after the touch
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
			for (const SavePoint& sp : frontier)
			{
				std::vector<Vec> path = BallPathFrom(ballArena, sp.ball, 360);
				std::vector<Bot::Plan> cands;
				st.plans(sp, path, cands);
				if (kStepDebug) fprintf(stderr, "step %zu: %zu candidate plans from tick %d (car on ground %d at %.0f %.0f %.0f vel %.0f %.0f %.0f, ball %.0f %.0f %.0f vel %.0f %.0f %.0f)\n", k, cands.size(), sp.tick, (int)sp.car.isOnGround, sp.car.pos.x, sp.car.pos.y, sp.car.pos.z, sp.car.vel.x, sp.car.vel.y, sp.car.vel.z, sp.ball.pos.x, sp.ball.pos.y, sp.ball.pos.z, sp.ball.vel.x, sp.ball.vel.y, sp.ball.vel.z);
				std::shuffle(cands.begin(), cands.end(), rng);
				if (static_cast<int>(cands.size()) > share) cands.resize(share);
				for (const Bot::Plan& pl : cands)
				{
					if (stop()) break;
					int res = -1;
					SavePoint r = RunFrom(arena, car, sp, pl, st.maxTicks, st.ended, res, st.afterTouch ? &st.afterTouch : nullptr);
					runs++;
					if (kStepDebug) { gStepDbg[k * 8 + (r.firstTouch < 0 ? 0 : res == 1 ? 1 : res < 0 ? 1 - res : 7)]++; }
					if (res != 1) continue;
					r.score = st.score(r);
					next.push_back(std::move(r));
				}
				if (stop()) break;
			}
			if (passedPerStep) (*passedPerStep)[k] = static_cast<int>(next.size());
			std::sort(next.begin(), next.end(), [](const SavePoint& a, const SavePoint& b) { return a.score > b.score; });
			// Keep the best few, but not near-copies of each other (same end tick and ball spot)
			std::vector<SavePoint> keep;
			for (SavePoint& c : next)
			{
				bool dup = false;
				for (const SavePoint& q : keep)
					if (std::abs(q.tick - c.tick) <= 2 && (q.ball.pos - c.ball.pos).Length() < 60.0f) { dup = true; break; }
				if (!dup) keep.push_back(std::move(c));
				if (static_cast<int>(keep.size()) >= (k + 1 == steps.size() ? st.beam * 5 : st.beam)) break;
			}
			frontier = std::move(keep);
		}
		return frontier;
	}

	// ---- Each mechanic's steps for the search (the rulebook's steps, with how to try them) ----
	// Empty = the mechanic isn't step-searched yet (or the search starts too late for it): the one-touch engine and
	// "your line, sharpened" cover it.
	std::vector<StepDef> MechanicSteps(const ShotAnalysis& real, int s, float gs)
	{
		std::vector<StepDef> steps;
		if (!real.valid) return steps;
		const float wx = 892.755f - kBallR - 60.0f, zHi = 642.775f - kBallR - 50.0f;
		auto aimBall = [](const BallState& b, float gs, float& onTarget, float& height, float& speed) {
			// Where the ball is headed (straight line with gravity, no bounces): is it on target, how high, how hard
			Vec p = b.pos, v = b.vel;
			if (gs < 0) { p = Vec(-p.x, -p.y, p.z); v = Vec(-v.x, -v.y, v.z); }
			onTarget = 0; height = 0; speed = v.Length();
			if (v.y < 100.0f) return;
			float t = (5120.0f - p.y) / v.y;
			if (t > 3.0f) return;
			float x = p.x + v.x * t, z = std::max(93.0f, p.z + v.z * t - 325.0f * t * t);
			float miss = std::hypot(std::max(0.0f, std::fabs(x) - 800.0f), std::max(0.0f, z - 550.0f));
			onTarget = 1.0f - std::min(1.0f, miss / 1000.0f);
			height = z;
		};

		if (real.type == ShotType::DoubleTap && real.setupTouchTick > s + 6)
		{
			const int surface = real.tapSurface != 0 ? real.tapSurface : 1;
			// 1. Setup: send it into the backboard (the surface you used), somewhere a second touch can reach
			StepDef setup;
			setup.name = "Setup";
			setup.maxTicks = 760;
			setup.beam = 8;
			setup.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<Bot::Plan>& out) {
				const int horizon = std::min(300, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 6)
					for (float x : { -450.0f, 0.0f, 450.0f })
						for (float z : { 850.0f, 1150.0f })
							for (int jl : { 12, 24, 48, 84, 120 })
								for (int pw = 0; pw < 2; pw++)
								{
									Bot::Plan pl;
									if (PlanFrom(sp, path, ti, Vec(x, gs * 5150.0f, z), 150.0f, jl, false, true, pw == 1, pl)) out.push_back(pl);
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
			// After the setup touch: fly to where the ball will be a moment after it comes off the wall, lined up to hit
			// it at the goal (the "read")
			setup.afterTouch = [gs](int tick, const CarState& car, const BallState& b, Bot::Plan& f) {
				const float wallY = gs * (5120.0f - kBallR);
				if (b.vel.y * gs < 200.0f) return false;
				float t = (wallY - b.pos.y) / b.vel.y;
				if (t < 0.0f || t > 4.0f) return false;
				Vec P = b.pos + b.vel * t + Vec(0, 0, -325.0f * t * t);
				Vec vw = b.vel + Vec(0, 0, -650.0f * t);
				Vec vr(vw.x * 0.9f, -vw.y * 0.6f, vw.z * 0.9f);              // off the wall (rough bounce)
				const float after = 0.3f;
				Vec Q = P + vr * after + Vec(0, 0, -325.0f * after * after);
				if (Q.z < 140.0f) Q.z = 140.0f;
				Vec aim(0, gs * 5120.0f, 360.0f);
				Vec d = aim - Q; d = d / std::max(1.0f, d.Length());
				f.target = Q - d * 150.0f; f.hitDir = d;
				f.arriveTick = tick + static_cast<int>((t + after) * 120.0f);
				f.jumpTick = car.isOnGround ? tick : -1;
				f.dodge = false; f.doubleJump = false; f.power = false;
				return true;
			};
			setup.score = [](const SavePoint& at) {
				// A rebound you can get to: ball near the middle, not too high, car close, flip and boost left
				// High off the wall (time to read it before it drops), near the middle, car close and coming in slower than
				// the ball is coming out
				float dist = (at.ball.pos - at.car.pos).Length();
				float hi = std::clamp((at.ball.pos.z - 400.0f) / 500.0f, -1.0f, 1.0f);
				return 1.5f * hi - dist / 1500.0f - std::fabs(at.ball.pos.x) / 1200.0f
					+ (at.car.HasFlipOrJump() ? 0.2f : 0.0f) + at.car.boost / 400.0f;
			};
			steps.push_back(setup);

			// 2. The second touch, off the rebound, into the goal
			StepDef shot;
			shot.name = "Shot";
			shot.maxTicks = 220;
			shot.beam = 8;
			shot.plans = [gs, wx, zHi](const SavePoint& sp, const std::vector<Vec>& path, std::vector<Bot::Plan>& out) {
				const int horizon = std::min(170, static_cast<int>(path.size()) - 1);
				const Vec aims[3] = { Vec(0, gs * 5200.0f, zHi), Vec(wx, gs * 5200.0f, zHi), Vec(-wx, gs * 5200.0f, zHi) };
				for (int ti = 4; ti <= horizon; ti += 3)
					for (const Vec& a : aims)
						for (int v = 0; v < 4; v++)
							for (int jl : { 24, 60 })
							{
								Bot::Plan pl;
								if (PlanFrom(sp, path, ti, a, 150.0f, jl, (v & 1) == 1, true, (v & 2) != 0, pl)) out.push_back(pl);
							}
			};
			shot.ended = [](const SavePoint& at, int stepStart) {
				if (at.touches >= 2 && at.tick - at.lastTouch >= 6) return 1;
				return 0;
			};
			shot.score = [gs, aimBall](const SavePoint& at) {
				float on, h, sp;
				aimBall(at.ball, gs, on, h, sp);
				return on * (1.0f + std::clamp((h - 150.0f) / 300.0f, 0.0f, 1.0f) + std::clamp((sp - 1400.0f) / 1400.0f, 0.0f, 1.0f));
			};
			steps.push_back(shot);
		}
		return steps;
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
		scale = std::clamp(maxSeconds * 0.6f / std::max(1e-4f, perRun * planned), 0.12f, 1.0f);
		budget = std::max(120, static_cast<int>(budget * scale));
		coarseBudget = budget * 40 / 100;
		size_t start = static_cast<size_t>(timed) * step;
		step = std::max(1, static_cast<int>(coarse.size()) / std::max(1, coarseBudget));
		for (size_t i = start; i < coarse.size() && !stop(); i += step) tryOption(coarse[i]);
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
	struct CemTry { std::vector<CarControls> ctl; SimRun run; float q; int source; };   // source 0 your line, 1 the bot's
	std::vector<CemTry> cemBest;   // the best few goals, inputs kept

	// ---- Multi-step mechanics: the step search (its lines join the refined ones and go through the same checks) ----
	std::string stepNote;
	{
		std::vector<StepDef> mechSteps = MechanicSteps(real, s, gs);
		if (!mechSteps.empty() && !stop())
		{
			std::unique_ptr<Arena> ballArena(Arena::Create(GameMode::SOCCAR));
			SavePoint sp0 = StartPoint(arena.get(), car, fr, s);
			std::mt19937 srng(777u + static_cast<unsigned>(n));
			int runs = 0, replayed = 0, scored = 0;
			std::vector<int> passed;
			auto stepStop = [&]() { return cancelled() || elapsed() > maxSeconds * 0.45f; };
			std::vector<SavePoint> finals = StepSearch(arena.get(), car, ballArena.get(), sp0, mechSteps,
				std::max(200, static_cast<int>((depth >= 2 ? 12000 : 3000) * scale)), srng, stepStop, runs, &passed);
			out.tries += runs;
			Bot::Plan none;
			none.arriveTick = maxTick;
			for (SavePoint& f : finals)
			{
				if (hardStop()) break;
				std::vector<CarControls> c = f.ctl;
				c.resize(maxTick + 1);   // after the last step: hands off
				SimRun r = Run(arena.get(), car, fr, s, none, maxTick, ref, 0, &c);
				out.tries++;
				replayed++;
				float q = styledQuality(r);
				if (!r.wentIn) continue;
				scored++;
				r.frames.clear();
				r.frames.shrink_to_fit();
				cemBest.push_back({ std::move(c), std::move(r), q, 2 });
			}
			std::sort(cemBest.begin(), cemBest.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
			if (kStepDebug) { for (int k = 0; k < 2; k++) fprintf(stderr, "step%d: no touch %d, passed %d, twice %d, floor %d, timeout %d, other surface %d, ran out %d\n", k, gStepDbg[k*8], gStepDbg[k*8+1], gStepDbg[k*8+2], gStepDbg[k*8+3], gStepDbg[k*8+4], gStepDbg[k*8+5], gStepDbg[k*8+7]); for (int& x : gStepDbg) x = 0; }
			std::string per;
			for (size_t k = 0; k < passed.size(); k++) per += std::format("{}{} {}", k ? ", " : "", mechSteps[k].name, passed[k]);
			stepNote = std::format("Step search: {} tries; passed {}; {} of {} finished lines scored.", runs, per, scored, replayed);
		}
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
		Bot::Plan none;
		none.arriveTick = (real.valid ? real.touchTick : s) + 120;
		if (shift == 0) return Run(arena.get(), car, fr, s, none, maxTick, ref, nudge, &cemBest[pk.cemIdx].ctl);
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
		if (held < 4) continue;
		SimRun win = replay(pk, 0);
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
		std::sort(diffs.begin(), diffs.end(), [](const Diff& a, const Diff& b) { return a.size > b.size; });
		std::vector<std::string> parts;
		for (const Diff& d : diffs) if (parts.size() < 2 && d.size >= 0.75f) parts.push_back(d.say);
		bool yourLine = pk.botIdx < 0 && cemBest[pk.cemIdx].source == 0;
		std::string kind = yourLine ? "Engine (your line, sharpened)" : "Engine";
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
		scale < 0.99f ? std::format(" This computer is slower, so the search was sized down to {:.0f}% to fit the time.", scale * 100.0f) : std::string());
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
