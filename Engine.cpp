// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "Engine.h"
#include "Grader.h"
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
	};
	
	struct State { int jumpStarted = -1; bool doubleJumped = false; bool dodged = false; int touched = -1; };
	
	inline CarControls Drive(const CarState& s, const Plan& p, int tick, State& st)
	{
		CarControls c;
		const float g = -650.0f;
		int tau = p.arriveTick - tick;
		float T = std::max(1, tau) / 120.0f;
		if (st.touched >= 0 && tick >= st.touched) return c;   // after the touch: hands off
	
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
			// Ground dodge into the ball
			if (p.dodge && p.jumpTick < 0 && tau <= p.dodgeLead && tau > p.dodgeLead - 2) { c.jump = true; st.jumpStarted = tick; }
			return c;
		}
		if (st.jumpStarted < 0) st.jumpStarted = tick;
		int sinceJump = tick - st.jumpStarted;
	
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
			if (!cs.isOnGround) { st.jumpStarted = s - 1000; st.doubleJumped = true; }
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

	float Quality(const SimRun& r)
	{
		if (r.touchTick < 0) return -100000.0f;
		float q = r.grade.overall * 10000.0f;
		if (r.wentIn) q += 5000.0f - std::min(5000.0f, r.shot.cornerDist) + r.shot.shotSpeed * 0.5f - (r.shot.bouncedIn ? 3000.0f : 0.0f);
		else q -= r.shot.reachesGoalLine ? std::min(5000.0f, r.shot.missDist) : 5000.0f;
		return q - (r.touchTick - r.frames.front().tick) * 0.0f;
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

EngineResult Engine::Line(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	auto clock0 = std::chrono::steady_clock::now();
	auto elapsed = [&]() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - clock0).count(); };
	auto stop = [&]() { return (cancel && cancel->load()) || elapsed() > maxSeconds; };

	EngineResult out;
	const int n = static_cast<int>(fr.size());
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	// Default start: when the ball starts moving, or, if you set the shot up yourself (a pop, a push to the wall),
	// right after that setup touch, so the engine plays the same shot you went for
	int s = real.valid ? real.startTick : 1;
	if (real.valid)
	{
		int k = real.touchTick, prevEnd = -1;
		for (int i = k - 1; i > real.startTick; i--)
			if (fr[i].touch && !fr[i + 1].touch && k - i > 10) { prevEnd = i; break; }
		if (prevEnd > 0) s = std::min(k - 6, prevEnd + 8);
	}
	if (startTick >= 0) s = startTick;
	s = std::clamp(s, 1, n - 2);
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
	auto makePlan = [&](int T, Vec aimPt, float reach, int jumpLead, bool dodge, bool doubleJump, Option& o) -> bool {
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
		if (ground)
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
		o.plan.jumpTick = ground || startAir ? -1 : std::max(s, T - jumpLead);
		if (ground && !startAir) o.plan.jumpTick = -1;
		if (startAir) o.plan.jumpTick = s;   // already flying
		o.plan.dodge = dodge;
		o.plan.doubleJump = doubleJump && !dodge;
		o.plan.dodgeLead = ground ? 14 : 8;
		o.aimPt = aimPt;
		o.jumpLead = jumpLead;
		o.reach = reach;
		return true;
	};

	struct Tried { Option o; SimRun run; float q; };
	std::vector<Tried> tried;
	auto tryOption = [&](const Option& o) {
		SimRun r = Run(arena.get(), car, fr, s, o.plan, maxTick, ref, 5000 + out.tries);
		out.tries++;
		float q = Quality(r);
		r.frames.clear();
		r.frames.shrink_to_fit();
		tried.push_back({ o, std::move(r), q });
	};

	// ---- Coarse: when to meet it, and how to get there (aimed at the middle, up high) ----
	const int budget = depth >= 2 ? 1500 : 320;   // the from-scratch bot's share of the sim runs
	std::vector<Option> coarse;
	for (int T = s + 12; T <= s + horizon; T += 4)
		for (int jl : { 24, 48, 84, 120, 168, 230 })
			for (int v = 0; v < 2; v++)
			{
				Option o;
				if (!makePlan(T, aims[0], 150.0f, jl, v == 1, true, o)) continue;
				bool dup = false;
				for (const Option& c : coarse)
					if (c.plan.arriveTick == o.plan.arriveTick && c.plan.jumpTick == o.plan.jumpTick && c.plan.dodge == o.plan.dodge) { dup = true; break; }
				if (!dup) coarse.push_back(o);
			}
	if (coarse.empty()) { out.status = EngineStatus::NoBall; out.ms = elapsed() * 1000.0f; return out; }
	// Spread the coarse budget over the whole range of meeting times
	int coarseBudget = budget * 40 / 100;
	int step = std::max(1, static_cast<int>(coarse.size()) / std::max(1, coarseBudget));
	for (size_t i = 0; i < coarse.size() && !stop(); i += step) tryOption(coarse[i]);

	// ---- Aim correction on the best few ("shooting"): play it, see where the ball really crosses the goal line,
	// move the aim point by the miss, play it again. Each line, aimed at the middle up high and both top corners. ----
	std::mt19937 rng(777u + static_cast<unsigned>(n));
	auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	auto sameOption = [](const Option& a, const Option& b) {
		return a.plan.arriveTick == b.plan.arriveTick && a.plan.jumpTick == b.plan.jumpTick && a.plan.dodge == b.plan.dodge &&
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
			if (!makePlan(o.plan.arriveTick, next, o.reach, o.jumpLead, o.plan.dodge, o.plan.doubleJump, o2)) break;
			o = o2;
		}
	};
	{
		std::vector<int> order;
		for (int i = 0; i < static_cast<int>(tried.size()); i++)
			if (tried[i].run.touchTick >= 0 && tried[i].run.shot.valid && tried[i].run.shot.shotDir.y * gs > 0.2f) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		int topK = depth >= 2 ? 24 : 8;
		std::vector<Option> bases;
		for (int i = 0; i < static_cast<int>(order.size()) && static_cast<int>(bases.size()) < topK; i++) bases.push_back(tried[order[i]].o);
		for (const Option& b : bases)
			for (int w : { 0, 1, 2 })
			{
				if (stop() || out.tries >= budget) break;
				Option o;
				if (!makePlan(b.plan.arriveTick, aims[w], b.reach, b.jumpLead, b.plan.dodge, b.plan.doubleJump, o)) continue;
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
		bool dodge = par.plan.dodge, dj = par.plan.doubleJump;
		switch (pick(0, 4))
		{
		case 0: aimPt = aimPt + Vec(pick(-4, 4) * 40.0f, 0, pick(-4, 4) * 40.0f); break;
		case 1: T += pick(-8, 8); break;
		case 2: reach = std::clamp(reach + pick(-3, 3) * 10.0f, 110.0f, 200.0f); break;
		case 3: jl = std::max(12, jl + pick(-24, 24)); break;
		case 4: dodge = !dodge; dj = !dodge; break;
		}
		Option o;
		if (!makePlan(T, aimPt, reach, jl, dodge, dj, o)) continue;
		if (!seenOption(o)) tryOption(o);
	}

	// ---- Second engine: your own line, optimized. Starting from what you pressed in the 1.5 s before your touch,
	// it reshapes the stick, steering, throttle and boost over time and moves your jumps, keeping whatever plays
	// out best (cross-entropy search). It finds the best version of the shot you went for. ----
	struct CemTry { std::vector<CarControls> ctl; SimRun run; float q; };
	std::vector<CemTry> cemBest;   // the best few, inputs kept
	if (real.valid && !stop())
	{
		std::vector<CarControls> realCtl(n);
		for (int i = 0; i < n; i++)
		{
			const Frame& f = fr[i];
			realCtl[i].throttle = f.throttle; realCtl[i].steer = f.steer; realCtl[i].pitch = f.pitch; realCtl[i].yaw = f.yaw;
			realCtl[i].roll = f.roll; realCtl[i].jump = f.jump; realCtl[i].boost = f.boost; realCtl[i].handbrake = f.handbrake;
		}
		const int k = real.touchTick;
		const int w0 = std::max(s, k - 180), w1 = std::min(n - 1, k + 24);
		const int knotGap = 15, K = (w1 - w0) / knotGap + 2;
		std::vector<std::pair<int, int>> jumps;   // your jump presses in the window: start, length
		for (int i = std::max(w0, 1); i < w1 && jumps.size() < 4; i++)
			if (fr[i].jump && !fr[i - 1].jump) { int len = 0; while (i + len < n && fr[i + len].jump) len++; jumps.push_back({ i, len }); }
		const int D = K * 6 + static_cast<int>(jumps.size());
		std::vector<float> mean(D, 0.0f), sigma(D);
		for (int d = 0; d < D; d++) sigma[d] = d >= K * 6 ? 6.0f : (d % 6 == 5 ? 0.6f : 0.35f);
		auto build = [&](const std::vector<float>& x) {
			std::vector<CarControls> c = realCtl;
			for (size_t j = 0; j < jumps.size(); j++)
			{
				int at = jumps[j].first, len = jumps[j].second;
				for (int i = at; i < std::min(n, at + len); i++) c[i].jump = false;
			}
			for (size_t j = 0; j < jumps.size(); j++)
			{
				int at = jumps[j].first + static_cast<int>(std::lround(x[K * 6 + j])), len = jumps[j].second;
				at = std::clamp(at, s + 1, n - 2);
				for (int i = at; i < std::min(n, at + len); i++) c[i].jump = true;
				if (at - 1 >= 0 && c[at - 1].jump && at - 1 >= w0) c[at - 1].jump = false;   // keep it a fresh press
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
		none.arriveTick = k + 120;
		const int cemBudget = depth >= 2 ? 1500 : 320, pop = depth >= 2 ? 48 : 32, elites = 6;
		std::mt19937 crng(4242u + static_cast<unsigned>(n));
		std::normal_distribution<float> N01(0.0f, 1.0f);
		int cemTries = 0;
		std::vector<float> bestX(D, 0.0f);
		float bestQ = -1e12f;
		while (cemTries < cemBudget && !stop())
		{
			std::vector<std::pair<float, std::vector<float>>> gen;
			for (int m = 0; m < pop && cemTries < cemBudget && !stop(); m++)
			{
				std::vector<float> x(D);
				if (m == 0) x = bestX;   // keep the best so far in every round
				else
					for (int d = 0; d < D; d++)
					{
						float lim = d >= K * 6 ? 30.0f : 1.6f;
						x[d] = std::clamp(mean[d] + sigma[d] * N01(crng), -lim, lim);
					}
				std::vector<CarControls> c = build(x);
				// Each try from a slightly different start, so the search favors lines that hold up, not lucky ones
				SimRun r = Run(arena.get(), car, fr, s, none, maxTick, ref, 1000 + cemTries, &c);
				cemTries++;
				out.tries++;
				float q = Quality(r);
				if (q > bestQ) { bestQ = q; bestX = x; }
				if (r.wentIn)
				{
					// Keep the best few goals with their inputs for the final check
					r.frames.clear();
					r.frames.shrink_to_fit();
					cemBest.push_back({ std::move(c), std::move(r), q });
					std::sort(cemBest.begin(), cemBest.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
					if (cemBest.size() > 16) cemBest.pop_back();
				}
				gen.push_back({ q, std::move(x) });
			}
			if (gen.size() < static_cast<size_t>(elites)) break;
			std::sort(gen.begin(), gen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (int d = 0; d < D; d++)
			{
				float m = 0, v = 0;
				for (int e = 0; e < elites; e++) m += gen[e].second[d];
				m /= elites;
				for (int e = 0; e < elites; e++) v += (gen[e].second[d] - m) * (gen[e].second[d] - m);
				mean[d] = m;
				float floorSigma = d >= K * 6 ? 1.0f : 0.04f;
				sigma[d] = std::max(floorSigma, 0.9f * std::sqrt(v / elites));
			}
		}
	}

	// ---- Pick the best goal (from either engine) that also works from slightly nudged starts ----
	struct Pick { float q; int botIdx; int cemIdx; };
	std::vector<Pick> picks;
	for (int i = 0; i < static_cast<int>(tried.size()); i++) if (tried[i].run.wentIn) picks.push_back({ tried[i].q, i, -1 });
	for (int i = 0; i < static_cast<int>(cemBest.size()); i++) picks.push_back({ cemBest[i].q, -1, i });
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.q > b.q; });
	auto replay = [&](const Pick& pk, int nudge) {
		if (pk.botIdx >= 0) return Run(arena.get(), car, fr, s, tried[pk.botIdx].o.plan, maxTick, ref, nudge);
		Bot::Plan none;
		none.arriveTick = (real.valid ? real.touchTick : s) + 120;
		return Run(arena.get(), car, fr, s, none, maxTick, ref, nudge, &cemBest[pk.cemIdx].ctl);
	};
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
		if (checked++ >= (depth >= 2 ? 40 : 20) || stop()) break;
		int expect = pk.botIdx >= 0 ? tried[pk.botIdx].run.grade.overall : cemBest[pk.cemIdx].run.grade.overall;
		int held = 0;
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun r = replay(pk, 300 + j);
			out.tries++;
			held += r.wentIn && r.grade.overall >= expect - 1;
			if (j - held > 2) break;
		}
		if (held < 4) continue;
		SimRun win = replay(pk, 0);
		if (!win.wentIn) continue;
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

		// The cue: the biggest differences from what you did
		std::vector<std::string> parts;
		if (real.valid)
		{
			const Frame& yh = fr[real.touchTick];
			if (win.takeoff >= 0 && yourTakeoff >= 0 && std::abs(win.takeoff - yourTakeoff) >= 10)
				parts.push_back(std::format("take off {} {}", Secs(std::abs(win.takeoff - yourTakeoff)), win.takeoff < yourTakeoff ? "earlier" : "later"));
			if (win.takeoff >= 0 && yourTakeoff < 0) parts.push_back("jump for it");
			if (win.takeoff < 0 && yourTakeoff >= 0) parts.push_back("stay on the ground");
			if (std::abs(win.touchTick - real.touchTick) >= 10)
				parts.push_back(std::format("meet it {} {}", Secs(std::abs(win.touchTick - real.touchTick)), win.touchTick < real.touchTick ? "sooner" : "later"));
			if (std::fabs(hit.ballZ - yh.ballZ) >= 100.0f)
				parts.push_back(std::format("{:.0f} {}", std::fabs(hit.ballZ - yh.ballZ), hit.ballZ > yh.ballZ ? "higher" : "lower"));
			if (win.boostUsed - yourBoost >= 12.0f) parts.push_back("more boost");
			if (yourBoost - win.boostUsed >= 12.0f) parts.push_back("less boost");
			if (win.shot.shotSpeed - real.shotSpeed >= 350.0f) parts.push_back("hit it harder");
		}
		std::string kind = pk.botIdx >= 0 ? "Engine" : "Engine (your line, sharpened)";
		std::string joined;
		for (size_t i = 0; i < parts.size() && i < 3; i++) joined += (i == 0 ? "" : (i + 1 == std::min<size_t>(parts.size(), 3) ? " and " : ", ")) + parts[i];
		if (joined.empty()) joined = std::format("meets it {} in, {:.0f} high", Secs(win.touchTick - s), hit.ballZ);
		out.cue = kind + ": " + joined;
		if (!out.cue.empty()) out.cue[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out.cue[0])));

		std::string lines;
		if (win.takeoff >= 0) lines += std::format("Takes off at {} ({}).\n", Secs(win.takeoff - s), yourTakeoff >= 0 ? "you: " + Secs(yourTakeoff - s) : std::string("you didn't jump"));
		if (real.valid)
		{
			const Frame& yh = fr[real.touchTick];
			lines += std::format("Meets the ball at {}, {:.0f} high (you: {}, {:.0f} high).\n", Secs(win.touchTick - s), hit.ballZ, Secs(real.touchTick - s), yh.ballZ);
			lines += std::format("Ball off the hit at {} (you: {}).\n", Units::Speed(win.shot.shotSpeed), Units::Speed(real.shotSpeed));
		}
		lines += std::format("Boost used: {:.0f} (you: {:.0f}).\n", win.boostUsed, yourBoost);
		out.compare = lines;
		out.detail = std::format("{}Would be a {} ({}).\n{}It plays perfect inputs from {} into your rep: it shows the ceiling for this shot. "
			"It scored from {} of {} slightly different starts.", lines, out.grade, out.gradeWhy,
			pk.botIdx >= 0 ? "The engine played this from scratch (none of your inputs). " : "The engine started from your own inputs and reshaped them. ",
			Secs(s - (real.valid ? real.startTick : 0)), held, kNudges);
		out.frames = std::move(win.frames);
		break;
	}
	if (out.status != EngineStatus::Found)
		out.status = (cancel && cancel->load()) ? EngineStatus::Cancelled : EngineStatus::NothingFound;
	int touched = 0, goals = 0;
	for (const Tried& t : tried) { touched += t.run.touchTick >= 0; goals += t.run.wentIn; }
	out.note = std::format("{} lines tried: {} touched the ball, {} scored. {:.0f} ms.", tried.size(), touched, goals, elapsed() * 1000.0f);
	out.ms = elapsed() * 1000.0f;
	return out;
}
