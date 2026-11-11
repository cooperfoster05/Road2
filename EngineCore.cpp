// The engine, part 1: worker threads, loading a recorded moment into the simulator, one simulated run, and how a run is scored.
// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "EngineInternal.h"

namespace EngineInternal
{
	int EngineThreads()
	{
		if (tThreads > 0) return tThreads;
		static const int n = [] {
			if (const char* e = std::getenv("RLC_ENGINE_THREADS")) return std::clamp(std::atoi(e), 1, 64);   // testing
			unsigned h = std::thread::hardware_concurrency();
			return static_cast<int>(std::clamp<unsigned>(h > 2 ? h - 2 : 1, 1, 14)); }();
		return n;
	}

	void RunOn(const Workers* wk, int count, const std::function<void(int, int)>& fn)
	{
		if (wk) const_cast<Workers*>(wk)->Run(count, fn);
		else for (int i = 0; i < count; i++) fn(i, 0);
	}

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

	void ParkTick(Arena* arena, Car* car)
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

	const WheelSnap& FreshWheels(const CarConfig& cfg)
	{
		static std::mutex mx;
		static std::vector<std::pair<CarConfig, std::unique_ptr<WheelSnap>>> snaps;
		std::lock_guard<std::mutex> lock(mx);
		for (auto& p : snaps)
			if ((p.first.hitboxSize - cfg.hitboxSize).Length() < 1e-3f && (p.first.hitboxPosOffset - cfg.hitboxPosOffset).Length() < 1e-3f
				&& p.first.frontWheels.wheelRadius == cfg.frontWheels.wheelRadius && p.first.backWheels.wheelRadius == cfg.backWheels.wheelRadius)
				return *p.second;
		std::unique_ptr<Arena> z(Arena::Create(GameMode::SOCCAR));
		Car* c = z->AddCar(Team::BLUE, cfg);
		ParkTick(z.get(), c);
		auto snap = std::make_unique<WheelSnap>();
		const btVehicleRL& v = c->_bulletVehicle;
		snap->wheels = v.m_wheelInfo; snap->forwardWS = v.m_forwardWS; snap->axle = v.m_axle;
		snap->forwardImpulse = v.m_forwardImpulse; snap->sideImpulse = v.m_sideImpulse;
		snap->steering = v.m_steeringValue; snap->pitch = v.m_pitchControl;
		snaps.push_back({ cfg, std::move(snap) });
		return *snaps.back().second;
	}

	void ClearCarInternals(Arena* arena, Car* car)
	{
		ParkTick(arena, car);
		const WheelSnap& w = FreshWheels(car->config);
		btVehicleRL& v = car->_bulletVehicle;
		v.m_wheelInfo = w.wheels; v.m_forwardWS = w.forwardWS; v.m_axle = w.axle;
		v.m_forwardImpulse = w.forwardImpulse; v.m_sideImpulse = w.sideImpulse;
		v.m_steeringValue = w.steering; v.m_pitchControl = w.pitch;
	}

	Grip GetGrip(Car* car)
	{
		Grip g{};
		const btVehicleRL& v = car->_bulletVehicle;
		for (int i = 0; i < v.m_wheelInfo.size() && i < 4; i++) { g[i] = v.m_wheelInfo[i].m_latFriction; g[4 + i] = v.m_wheelInfo[i].m_longFriction; }
		return g;
	}

	void SetGrip(Car* car, const Grip& g)
	{
		btVehicleRL& v = car->_bulletVehicle;
		for (int i = 0; i < v.m_wheelInfo.size() && i < 4; i++) { v.m_wheelInfo[i].m_latFriction = g[i]; v.m_wheelInfo[i].m_longFriction = g[4 + i]; }
	}

	Grip RecordedGrip(const Frame& f)
	{
		Grip g{};
		if (f.onGround && (f.onWall || f.carZ > 60.0f)) for (int i = 4; i < 8; i++) g[i] = 1.0f;
		return g;
	}

	// Plays the plan out from tick s: the bot drives until it touches the ball, then lets go
	// fixedCtl: play these inputs (indexed by tick) instead of the bot
	// resets: ticks where a step-search line's save points were (the car's leftover wheel state is cleared there,
	// exactly as the search did), so replaying its inputs reproduces it exactly
	SimRun Run(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const Bot::Plan& plan, int maxTick,
		const ShotReference& ref, int nudge, const std::vector<CarControls>* fixedCtl, const std::vector<int>* resets)
	{
		SimRun r;
		r.frames.assign(fr.begin(), fr.begin() + s + 1);
		r.frames.reserve(maxTick + 2);
		ClearCarInternals(arena, car);
		SetClock(arena, s);
		LoadState(car, arena->ball, fr, s);
		SetGrip(car, RecordedGrip(fr[s]));
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
				const uint64_t tc = arena->tickCount;
				const Grip g = GetGrip(car);
				ClearCarInternals(arena, car);
				arena->tickCount = tc;
				SetGrip(car, g);
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
		else if (a.type == ShotType::Dribble)
		{
			// Each mechanic's credit points at its own 5 (Oct 23): a dribble's is a hard flick (2,800) toward a corner (550 out)
			credit += 1.5f * ramp(a.shotSpeed, 2300.0f, 2950.0f);
			credit += 1.5f * ramp(std::fabs(a.crossX), 150.0f, 640.0f);
		}
		else
		{
			float touchSec = (a.touchTick - a.startTick) / 120.0f;
			float vsBest = ref.valid ? touchSec - ref.touchSec : 0.0f;
			credit += ramp(a.crossZ, 150.0f, 420.0f);                  // upper half of the net at 350
			credit += ramp(a.shotSpeed, 1800.0f, 2950.0f);             // fast at 2,800 (past it, so a 5 survives a replay)
			credit += ref.valid ? ramp(-vsBest, -0.6f, 0.0f) : 1.0f;   // early: within 0.2 s of your best (0.05 s for a 5)
			credit += ramp(-a.aimErrDeg, -8.0f, -1.6f);               // on line: 3.5 deg for a 4, 1.6 for a 5
			credit += ramp(-a.cornerDist, -900.0f, -200.0f);           // toward a top corner (or a rocket) for a 5
		}
		q += 2000.0f * credit + a.shotSpeed * 0.2f;
		if (a.bouncedIn) q -= 4000.0f;
		return q;
	}

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

using namespace EngineInternal;

// ---- For tools outside the engine (the bot trainer and its benchmark): the same start and tick as Run ----
#include "EngineSim.h"

void EngineSim::Start(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, int nudge)
{
	ClearCarInternals(arena, car);
	SetClock(arena, s);
	LoadState(car, arena->ball, fr, s);
	SetGrip(car, RecordedGrip(fr[s]));
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
}

Frame EngineSim::Step(Arena* arena, Car* car, const CarControls& c, Frame& cur)
{
	cur.throttle = c.throttle; cur.steer = c.steer; cur.pitch = c.pitch; cur.yaw = c.yaw; cur.roll = c.roll;
	cur.jump = c.jump; cur.boost = c.boost; cur.handbrake = c.handbrake;
	car->controls = c;
	uint64_t t0 = arena->tickCount;
	arena->Step(1);
	CarState cs = car->GetState();
	BallState bs = arena->ball->GetState();
	Frame g;
	g.tick = cur.tick + 1;
	g.t = g.tick / 120.0f;
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
	}
	return g;
}

const CarConfig& EngineSim::Body(int carBody) { return ::Body(carBody); }
