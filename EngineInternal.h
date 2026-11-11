// The engine's shared insides (Nov 11: Engine.cpp was one 4,000-line file; it is now five, and this is what they
// share). Only the Engine*.cpp files include this. No pch on purpose: these files only see RocketSim, never the
// BakkesMod SDK.
#pragma once
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
#include <map>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

using namespace RocketSim;

#ifdef _WIN32
// Engine worker threads run below normal priority, like the search thread that starts them (no <windows.h> here)
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void* thread, int priority);
#endif

namespace EngineInternal
{
	constexpr float kU2R = 3.14159265f / 32768.0f;

	constexpr float kR2U = 32768.0f / 3.14159265f;

	constexpr int   kNudges = 6;

	constexpr float kBallR = 91.25f;

	constexpr float kCleanerMargin = 800.0f;   // Quality points: about 0.4 of the credit toward a 5 (e.g. 20% closer on two counts)

	// Multi-core (Oct 20). The search used one core. Independent sim runs now spread over worker threads, each with
	// its own arena (RocketSim arenas are independent, and every run resets pads and wheel state, so a run gives the
	// same result on any worker). Two cores are left free for the game. Workers run below normal priority on Windows.
	inline thread_local int tThreads = 0;   // set while two searches share the cores (either start, side by side)

	int EngineThreads();

#ifdef _WIN32
	inline void LowPriority() { SetThreadPriority(GetCurrentThread(), -1); }   // THREAD_PRIORITY_BELOW_NORMAL

#else
	inline void LowPriority() {}

#endif

	// Controllers that remember things between ticks keep that memory per thread, so the same controller can run on
	// several cores at once (its copies share one Mem otherwise)
	// and every run starts it fresh (before, a controller could pick up where an earlier run on the same thread left
	// off when the ticks happened to line up, so a line's checks depended on what ran before it)
	inline thread_local unsigned tRunEpoch = 0;

	template <class M> M& PerThread(const std::shared_ptr<M>& key)
	{
		struct E { std::weak_ptr<M> owner; std::unique_ptr<M> state; unsigned epoch = 0; };
		thread_local std::unordered_map<const M*, E> m;
		if (m.size() > 4096) for (auto it = m.begin(); it != m.end();) it = it->second.owner.expired() ? m.erase(it) : std::next(it);
		E& e = m[key.get()];
		if (!e.state || e.owner.expired() || e.epoch != tRunEpoch) { e.owner = key; e.state = std::make_unique<M>(*key); e.epoch = tRunEpoch; }
		return *e.state;
	}

	// Batches have a fixed size, whatever the number of cores, so a search gives the same lines on any computer
	constexpr int kBatch = 16;

	// One arena and car per worker (worker 0 is the calling thread and uses the caller's), and a pool of threads that
	// stays up for the whole search (starting threads for every batch costs too much, more so on Windows)
	struct Workers
	{
		std::vector<std::unique_ptr<Arena>> arenas;
		std::vector<Car*> cars;
		int n = 1;
		Arena* main0 = nullptr;
		Workers(Arena* a0, Car* c0, int count, const CarConfig& body) : n(std::max(1, count))
		{
			arenas.resize(n); cars.resize(n);
			cars[0] = c0; main0 = a0;
			for (int w = 1; w < n; w++) { arenas[w].reset(Arena::Create(GameMode::SOCCAR)); cars[w] = arenas[w]->AddCar(Team::BLUE, body); }
			for (int w = 1; w < n; w++) ts.emplace_back([this, w] { LowPriority(); Loop(w); });
		}
		~Workers()
		{
			{ std::lock_guard<std::mutex> l(m); quit = true; }
			cv.notify_all();
			for (std::thread& t : ts) t.join();
		}
		Arena* A(int w) const { return w == 0 ? main0 : arenas[w].get(); }
		Car* C(int w) const { return cars[w]; }
		// fn(i, w) for i in [0, count); returns when all are done
		void Run(int count, const std::function<void(int, int)>& fn)
		{
			if (ts.empty() || count <= 1) { for (int i = 0; i < count; i++) fn(i, 0); return; }
			{
				std::lock_guard<std::mutex> l(m);
				job = &fn; jobCount = count; next = 0; active = static_cast<int>(ts.size()); gen++;
			}
			cv.notify_all();
			Work(0);
			std::unique_lock<std::mutex> l(m);
			done.wait(l, [&] { return active == 0; });
			job = nullptr;
		}
	private:
		std::vector<std::thread> ts;
		std::mutex m;
		std::condition_variable cv, done;
		const std::function<void(int, int)>* job = nullptr;
		int jobCount = 0, active = 0, gen = 0;
		bool quit = false;
		std::atomic<int> next{ 0 };
		void Work(int w) { for (int i; (i = next.fetch_add(1)) < jobCount;) (*job)(i, w); }
		void Loop(int w)
		{
			int seen = 0;
			for (;;)
			{
				std::unique_lock<std::mutex> l(m);
				cv.wait(l, [&] { return quit || gen != seen; });
				if (quit) return;
				seen = gen;
				l.unlock();
				Work(w);
				l.lock();
				if (--active == 0) done.notify_one();
			}
		}
	};

	void RunOn(const Workers* wk, int count, const std::function<void(int, int)>& fn);

	// Puts the sim's car and ball exactly where the recording had them at tick s
	void LoadState(Car* car, Ball* ball, const std::vector<Frame>& fr, int s);

	// Every arena keeps its own tick clock, and the car's "last hit the ball" ticks are on that clock. Loading a saved
	// state on another arena (each core has its own) could then see a phantom touch, or skip the hit's extra push. So
	// every load sets the clock to the same value for the same moment of the rep, on every arena.
	constexpr uint64_t kClock = 10000000;

	inline void SetClock(Arena* arena, int tick) { arena->tickCount = kClock + static_cast<uint64_t>(std::max(0, tick)); }

	// Every run starts with every boost pad up: pads a previous run picked up must not change the next one
	void ResetPads(Arena* arena);

	// The physics keeps a little wheel/suspension state that setting the car's state doesn't reset, so a run that
	// starts on the ground or a wall could depend on the run before it. One tick parked high in the air clears it, so
	// the same inputs from the same start always play out exactly the same.
	// Oct 20: that tick wasn't enough. The wheels keep friction and suspension values from the last time they touched
	// a surface, so a run from the ground or a wall still depended on the run before it (up to 25 uu after 200 ticks
	// in a test). After the parked tick, the wheels are now set back to those of a car that has never driven.
	struct WheelSnap
	{
		btAlignedObjectArray<btWheelInfoRL> wheels;
		btAlignedObjectArray<btVector3> forwardWS, axle;
		btAlignedObjectArray<float> forwardImpulse, sideImpulse;
		float steering = 0, pitch = 0;
	};

	void ParkTick(Arena* arena, Car* car);

	const WheelSnap& FreshWheels(const CarConfig& cfg);

	void ClearCarInternals(Arena* arena, Car* car);

	// A wheel's grip on its first tick touching a surface is what it had the last time it touched one (it's updated
	// after it's used). The sim can't know that from a recording, so a start from the recording picks the value that
	// replays Coop's reps best (Oct 20, 1,372 starts from his reps, recorded inputs, error 0.5 s later): on the ground
	// none (median 4.7 uu off, vs 13.6 for the full grip the old reset happened to leave), on a wall forward grip only
	// (15.9 vs 20.9). A save point mid-line keeps the real values, so a line played in pieces matches one played through.
	using Grip = std::array<float, 8>;

	Grip GetGrip(Car* car);

	void SetGrip(Car* car, const Grip& g);

	Grip RecordedGrip(const Frame& f);

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
		const ShotReference& ref, int nudge, const std::vector<CarControls>* fixedCtl = nullptr, const std::vector<int>* resets = nullptr);

	// Higher is better: the grade first, then how close the shot came to the next grade up. The partial credit
	// steers the search toward 4s and 5s (high in the net, hard, early, on line) instead of settling for a 3.
	float Quality(const SimRun& r, const ShotReference& ref);

	inline std::string Secs(int ticks) { return std::format("{:.2f} s", ticks / 120.0f); }

	const CarConfig& Body(int carBody);

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
	Vec LiftAim(const std::vector<Vec>& path, int ti, const Vec& T);

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
		Grip grip{};                     // the wheels' grip at this point (see GetGrip)
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
	SavePoint StartPoint(Arena* arena, Car* car, const std::vector<Frame>& fr, int s);

	// 1 = the step passed (stop here), -1 = it failed (drop this branch), 0 = keep going
	using StepEnd = std::function<int(const SavePoint& at, int stepStart)>;

	// Plays a plan from a save point until the step ends (or maxTicks run out, which counts as a fail)
	// After the step's touch, a step can keep flying the car somewhere instead of letting go (e.g. following the ball
	// to the backboard for the second touch of a double tap). Returns false = let go.
	using AfterTouch = std::function<bool(int tick, const CarState& car, const BallState& ball, Bot::Plan& follow)>;

	// Air dribble carry: keep the car under and behind the ball (relative to the goal), matching its speed, so the
	// nose meets it softly and keeps pushing it toward the goal. back/down: where the car's center sits from the
	// ball's; k: how hard it closes the gap.
	Controller CarrySkill(float gs, float back, float down, float k, Vec goalAim = Vec(0, 0, 450.0f));

	// Air dribble carry aimed at a spot in the net: every tick, work out the velocity the ball needs to drop into
	// (target.x, the goal line, target.z) from where it is (flying flat at least minFlat), and sit the car on the
	// side of the ball that pushes it that way. Soft touches keep nudging the ball onto that arc, so the carry ends
	// with the ball heading high into the net instead of sinking into the bottom of it.
	Controller CarryAim(float gs, float tx, float tz, float back, float k, float minFlat = 1300.0f);

	// A carry with knobs, for tuning from a save point: where it aims the ball (gx, gz on the goal line), where the
	// car sits under it (back, down), how hard it closes the gap (k), and how much lower it tucks when the ball is
	// heading into the net too low (lift: extra 'down' per unit the predicted crossing sits under 515)
	constexpr int kCarryKnobs = 8;   // the carry uses the first 6, the reset all 8

	using Knobs = std::array<float, kCarryKnobs>;

	Controller ParamCarry(float gs, Knobs P);

	// Ground dribble carry: keep the car under the ball (the ball sitting P[0] ahead of the car's center) while it heads
	// for the goal. Knobs: ball ahead of center (uu), position gain, cruise speed, throttle gain, steering gain, aim x on
	// the goal line, how much it steers the ball toward the goal (0 = follow the ball's own line), lateral gain.
	// Measured on Coop's 30 held-out dribbles (Oct 14): tuned from the start of his dribble, it brings the ball on the
	// roof to where he was 1 s before his flick, at his speed, on 18 of 29 (the losses are the 4-5 s carries).
	Controller DribbleSkill(float gs, Knobs P);

	// Dribble flick: keep dribbling (the carry skill's settings) while turning toward the aim for P[0] ticks, jump
	// (held P[1] ticks), then P[2] ticks after the jump flip with the stick at (pitch P[3], yaw P[4]) to flick it.
	// P[5]: aim x on the goal line. Never touches anything after the flip.
	Controller FlickSkill(float gs, Knobs P);

	// Air dribble first touch: from just after the pop (still on the wall or floor, or already up), leave the surface
	// after a set wait, then fly to where the ball will be a moment ahead and meet it softly from under and behind.
	// Knobs: where it aims the ball (gx, gz on the goal line), where the car sits (back, down), closing gain (k),
	// how far ahead it reads the ball (lead, s), how long it waits on the surface before jumping (ticks), and how long
	// it holds the jump (ticks). Never jumps a second time (the flip stays unused).
	Controller FirstTouchSkill(float gs, Knobs P);

	// A good first touch (measured on Coop's air dribbles: after one, the carry finishes high 51% of the time, after
	// the rest 10%): the ball up high (above 1,425), touched soft (rising slower than ~506), close to the car's nose
	float FirstTouchFit(const SavePoint& at, float gs);

	// Your reps are mostly in training or freeplay with unlimited boost on (your boost never drops while you hold it:
	// 2,455 of 2,587 reps that used boost, Oct 14). This gives the sim unlimited boost too (not switched on: see LineAt).
	bool UnlimitedBoost(const std::vector<Frame>& fr);

	void MatchBoostRule(Arena* arena, const std::vector<Frame>& fr);

	// The ball and car a moment (10 ticks) after an air dribble's first touch, from a run's frames (indexed by tick)
	bool FirstTouchAfter(const std::vector<Frame>& f, int firstTouch, float& z, float& vz, float& dist);

	inline bool GoodFirstTouch(float z, float vz, float dist) { return z > 1425.0f && vz <= 506.0f && dist < 300.0f; }

	// The closest the car got to the ball from a tick on (for scoring tries that didn't make it)
	float ClosestFrom(const SavePoint& at, int fromTick);

	// A good spot to hand an air dribble to the finish (measured on Coop's 165 air dribbles, 1,000 to 2,800 out:
	// from spots like this the tuned finish goes in high 65% of the time, from the rest 21%): the ball moving 1,675+,
	// above 1,130, within 300 of the car, the car keeping pace with it (not running into it)
	float HandoffScore(const SavePoint& at, float gs, bool* goodOut = nullptr);

	// Flip reset: get the wheels onto the ball. Flies at a spot just off the ball nose first, then close in turns its
	// wheels to the ball (nose toward the goal) and settles onto it. Knobs: how far the contact leans toward the goal
	// (a) and sideways (b), position gain (k), how close it flies in nose first (sw), closing speed at contact (uc),
	// and how far ahead it reads the ball (lead, s).
	Controller ResetSkill(float gs, Knobs P);

	// Flip shot (after a reset): stay lined up behind the ball along the shot (aimed at gx, gz on the goal line),
	// then flip into it once close. Knobs: aim (gx, gz), where it sits behind the ball (back) and how hard it closes
	// (k), the distance it flips from (trig), how far ahead it reads the ball (lead), flip direction mix (side).
	Controller FlipShot(float gs, Knobs P);

	SavePoint RunFrom(Arena* arena, Car* car, const SavePoint& sp, const Bot::Plan& plan, int maxTicks, const StepEnd& ended, int& result,
		const AfterTouch* afterTouch = nullptr, const std::vector<CarControls>* fixed = nullptr, const Controller* ctrl = nullptr);

	// Plays a found line out from its last save point (hands off: no inputs) until the shot is over, by the same end
	// rules as Run, and grades the whole rep: your frames up to the search start, the line, then the play-out. The
	// frames graded are exactly the ones the search produced, so what's shown is what was found.
	SimRun PlayOut(Arena* arena, Car* car, const std::vector<Frame>& fr, const SavePoint& f, int maxTick, const ShotReference& ref);

	// Where the ball goes from a state on its own (nobody touches it)
	std::vector<Vec> BallPathFrom(Arena* ballArena, const BallState& b, int ticks);

	// A bot plan from any save point: meet the ball (on its own path from there) at tick `base + ti` and send it
	// toward aimPt (a point the ball's center should pass through: the goal, or a spot on the backboard)
	bool PlanFrom(const SavePoint& sp, const std::vector<Vec>& ballPath, int ti, Vec aimPt, float reach, int jumpLead,
		bool dodge, bool doubleJump, bool power, Bot::Plan& plan);

	bool AddPlan(std::vector<PlanSpec>& out, const SavePoint& sp, const std::vector<Vec>& path, int ti, Vec aim, float reach, int jl,
		bool dodge, bool dj, bool power);

	bool AddLiftPlan(std::vector<PlanSpec>& out, const SavePoint& sp, const std::vector<Vec>& path, int ti, const Vec& T, float reach, int jl,
		bool dodge, bool power);

	// Debug counters for the step search (test builds define ENGINE_STEP_DEBUG; never on in the plugin)
#ifdef ENGINE_STEP_DEBUG
	inline const bool kStepDebug = std::getenv("ENG_STEPDBG") != nullptr;

#else
	constexpr bool kStepDebug = false;

#endif

	inline int gStepDbg[64] = {};

#ifndef ENGINE_SCALE_CAP
#define ENGINE_SCALE_CAP 0.0f
#endif

	// How far the search may grow past its normal size when the machine has time to spare: a little on a normal
	// look (it stays quick), a lot more when you ask it to think harder (that wait is what the button is for)
	inline float ScaleCap(int depth) { return ENGINE_SCALE_CAP > 0.0f ? ENGINE_SCALE_CAP : depth >= 2 ? 3.0f : 1.5f; }

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
			// How close a failed try came (so the tuning can still tell which way to move when nothing works yet);
			// empty = all failures count the same
			std::function<float(const SavePoint&, int stepStart)> failFitness;
		};
		std::shared_ptr<Tune> tune;
	};

	// The search itself. budget = sim runs it may spend across all steps. Returns the survivors of the last step.
	// (From a set of save points, starting at step 'from': lists that share their first steps search them once.)
	std::vector<SavePoint> StepSearchFrom(Arena* arena, Car* car, Arena* ballArena, std::vector<SavePoint> frontier, size_t from, const std::vector<StepDef>& steps,
		int budget, std::mt19937& rng, const std::function<bool()>& stop, int& runs, std::vector<int>* passedPerStep = nullptr, const Workers* wk = nullptr);

	std::vector<SavePoint> StepSearch(Arena* arena, Car* car, Arena* ballArena, const SavePoint& start, const std::vector<StepDef>& steps,
		int budget, std::mt19937& rng, const std::function<bool()>& stop, int& runs, std::vector<int>* passedPerStep = nullptr, const Workers* wk = nullptr);

	// Plays a found line again, closed-loop, from a nudged start with the timing shifted: the bot re-aims each step's
	// plan from wherever the car really is, and your own inputs are shifted. Returns the controls it ended up using
	// (empty if a step broke), so the caller can replay and grade them exactly.
	bool RerunChain(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const SavePoint& line,
		const std::vector<StepDef>& steps, int nudge, int shift, SavePoint& endAt);

	// Shared pieces for the mechanics' steps
	inline float kWx = 892.755f - kBallR - 60.0f, kZHi = 642.775f - kBallR - 50.0f;

	// Where the ball is headed (straight line with gravity, no bounces): is it on target, how high, how hard
	void AimBall(const BallState& b, float gs, float& onTarget, float& height, float& speed);

	// Your recorded inputs, by tick
	std::vector<CarControls> YourInputs(const std::vector<Frame>& fr);

	// Your way into a touch, as you did it and reshaped a little: jumps a couple of ticks earlier or later, the nose a
	// touch up/down/left/right into the touch, or less boost right before it. Only from the start of the search.
	std::function<void(const SavePoint&, std::vector<std::vector<CarControls>>&)> YourWayIn(const std::vector<Frame>& fr, int s, int touch);

	// Your way up, closed-loop: your own inputs while you're on the wall or floor and through your jump, then the car
	// flies along the path you flew (your position, speed and car angle at each tick), correcting as it goes. Played
	// open-loop, your inputs drift about 140 off by 40 ticks before a flip reset; followed, the car stays on your path
	// and reaches the ball when you did. k: how hard it pulls back onto your path.
	Controller FollowYours(const std::vector<Frame>& fr, float k);

	// The shot: a touch into the goal from wherever the last step left off (top and low, middle and corners),
	// re-aimed from where the best tries really crossed the goal line
	StepDef ShotStep(float gs, int horizonTicks, bool highOnly = false);

	// ---- Each mechanic's step lists for the search (the rulebook's steps, with how to try them). Several lists =
	// several ways to play the mechanic (e.g. an air dribble with or without an extra carry touch). Empty = the
	// mechanic isn't step-searched yet, or the search starts too late for it. ----
	std::vector<std::vector<StepDef>> MechanicPlans(const ShotAnalysis& real, const std::vector<Frame>& fr, int s, float gs);
}
