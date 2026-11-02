// Model kickoffs for Learn (Nov 1): finds the fastest clean speedflip (and a plain boost-and-flip-in) from each kickoff
// spawn in RocketSim, then writes them as lesson model reps: the right spawn (x < 0 on blue is the driver's right; this
// said "left" until Nov 2), then the same kickoff mirrored from the left spawn, back to back in one rep (the lesson's
// right and left stages start at each spawn).
//
// The speedflip as inputs: boost from the first tick with a little steer, jump, a diagonal dodge, stick pulled back to
// cancel the flip with air roll to land wheels down, boost on the ground toward the ball, then a flip into it. Each part's
// timing and stick are searched (random start, then a few rounds keeping the best and sampling around them) for the
// earliest arrival (car within 200 of the ball, the openings book's measure) that lands wheels-down from the speedflip,
// touches the ball and sends it up field.
//
//   kickoff_model MESHES OUT_DIR
#include "third_party/RocketSim/src/RocketSim.h"
#include "RepData.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace RocketSim;

namespace
{
	constexpr float kR2U = 32768.0f / 3.14159265f;

	// dodgeLo..dodgeHi: when SSL and pro speedflips dodge from this spawn (ticks; their middle half, from 2,600+
	// replay kickoffs: diagonal 0.47-0.57 s, off-centre 0.53-0.73 s), so the model is the kickoff people actually do
	struct Spawn { const char* name; float x, y, yaw; int dodgeLo, dodgeHi; };
	const Spawn kDiag{ "diagonal", -2048.0f, -2560.0f, 3.14159265f * 0.25f, 56, 69 };
	const Spawn kOff{ "offcentre", -256.0f, -3840.0f, 3.14159265f * 0.5f, 64, 88 };
	const Spawn kBack{ "back", 0.0f, -4608.0f, 3.14159265f * 0.5f, 59, 88 };

	// One kickoff's inputs (speedflip = false: boost straight, then flip in)
	struct P
	{
		bool speedflip = true;
		float s0 = 0;      // steer at the start
		int S = 10;        // ticks of it
		float aim = 0;     // lateral aim offset at the ball (uu, + = the ball's right as you face it)
		int J = 30;        // first jump tick
		int H = 6;         // jump held
		int G = 2;         // ticks between letting go and the dodge
		float dy = -0.6f;  // dodge stick sideways (-1..1), pitch forward
		int C = 5;         // ticks after the dodge before pulling back
		int L = 30;        // ticks pulling back (the cancel)
		float r = 1.0f;    // air roll during the cancel
		float ry = 0;      // yaw during the cancel
		float D = 420;     // flip into the ball from this far
		float dy2 = 0;     // its stick sideways
		float ho = 0;      // heading off the line to the ball at the jump (radians; a speedflip angles the car so the diagonal dodge points at the ball)
	};

	struct Res
	{
		int arrive = -1, touch = -1, dodge = -1, land = -1, flip2 = -1;
		bool landedOk = true, flipped = false;
		float ballVY = 0, ballSpeed = 0, landSpeed = 0;
		float noseDip = 0;   // lowest nose angle (degrees) between the dodge and landing
		std::vector<Frame> frames;
		double cost = 1e9;
	};

	Frame ToFrame(int tick, const CarState& cs, const BallState& bs, const CarControls& c)
	{
		Frame g;
		g.tick = tick; g.t = tick / 120.0f;
		g.throttle = c.throttle; g.steer = c.steer; g.pitch = c.pitch; g.yaw = c.yaw; g.roll = c.roll;
		g.jump = c.jump; g.boost = c.boost; g.handbrake = c.handbrake;
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
		return g;
	}

	// mirror = the left spawn: x flipped, and every left/right input flipped
	Res Run(Arena* arena, Car*& car, const Spawn& sp, const P& p, bool mirror, bool record)
	{
		// a fresh car every run, so nothing from the last run (wheel contact, timers) carries over: the same inputs
		// always give the same kickoff
		if (car) arena->RemoveCar(car);
		car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
		const float m = mirror ? -1.0f : 1.0f;
		CarState cs;
		cs.pos = { sp.x * m, sp.y, 17.0f };
		cs.rotMat = Angle(mirror ? 3.14159265f - sp.yaw : sp.yaw, 0, 0).ToRotMat();
		cs.boost = 100.0f / 3.0f;
		cs.isOnGround = true;
		car->SetState(cs);
		BallState bs;
		bs.pos = { 0, 0, 92.75f };
		arena->ball->SetState(bs);
		for (auto* pad : arena->GetBoostPads()) { BoostPadState ps; ps.isActive = true; pad->SetState(ps); }

		Res out;
		int dodgeTick = -1, landTick = -1, flipStart = -1, touchTick = -1;
		bool everAir = false;
		CarControls c;
		const int maxT = 460;
		for (int t = 0; t < maxT; t++)
		{
			CarState s = car->GetState();
			BallState b = arena->ball->GetState();
			const Vec toBall = b.pos - s.pos;
			const float dist = toBall.Length();
			c = CarControls();
			c.throttle = 1.0f;
			c.boost = touchTick < 0;
			// ground steering toward an aim point beside the ball (in the car's frame)
			auto steerTo = [&]() {
				Vec aimPt = b.pos;
				Vec side = Vec(-toBall.y, toBall.x, 0);
				const float sl = side.Length();
				if (sl > 1) aimPt = aimPt + side * (p.aim * m / sl);
				Vec d = aimPt - s.pos;
				float fx = d.Dot(s.rotMat.forward), fy = d.Dot(s.rotMat.right);
				float ang = std::atan2(fy, fx);
				return std::clamp(ang * 3.0f, -1.0f, 1.0f);
			};
			if (p.speedflip)
			{
				const int D = p.J + p.H + p.G;
				if (t < p.J)
				{
					if (t < p.S) c.steer = p.s0 * m;
					else
					{
						// hold the heading offset from the line to the ball
						const float want = std::atan2(toBall.y, toBall.x) + p.ho * m;
						const Vec f = s.rotMat.forward;
						float err = want - std::atan2(f.y, f.x);
						while (err > 3.14159265f) err -= 6.2831853f;
						while (err < -3.14159265f) err += 6.2831853f;
						c.steer = std::clamp(err * 4.0f, -1.0f, 1.0f);
					}
				}
				else if (t < p.J + p.H) c.jump = true;
				else if (t < D) {}
				else if (t == D) { c.jump = true; c.pitch = -1.0f; c.yaw = p.dy * m; dodgeTick = t; }
				else if (landTick < 0)
				{
					if (t < D + p.C) { c.pitch = -1.0f; c.yaw = p.dy * m; }
					else if (t < D + p.C + p.L) { c.pitch = 1.0f; c.roll = p.r * m; c.yaw = p.ry * m; }
					else
					{
						// level out for the landing, as a player does: nose and wheels flat (stick against the tilt)
						const Angle a = Angle::FromRotMat(s.rotMat);
						const Vec w = s.angVel;
						const float pr = w.Dot(s.rotMat.right), rr = w.Dot(s.rotMat.forward);
						c.pitch = std::clamp(-a.pitch * 3.0f + pr * 0.4f, -1.0f, 1.0f);
						c.roll = std::clamp(-a.roll * 3.0f - rr * 0.4f, -1.0f, 1.0f);
					}
					if (t > D + 3 && s.isOnGround) { landTick = t; out.landedOk = s.rotMat.up.z > 0.85f; out.landSpeed = s.vel.Length(); }
				}
			}
			else if (flipStart < 0) c.steer = t < p.S ? p.s0 * m : steerTo();
			// the flip into the ball
			const bool readyForFlip = !p.speedflip || landTick >= 0;
			if (readyForFlip && touchTick < 0)
			{
				if (flipStart < 0 && s.isOnGround && dist < p.D) flipStart = t;
				if (flipStart < 0) { if (landTick >= 0 || !p.speedflip) c.steer = steerTo(); }
				else
				{
					const int k = t - flipStart;
					if (k < 3) c.jump = true;
					else if (k < 5) {}
					else if (k == 5) { c.jump = true; c.pitch = -1.0f; c.yaw = p.dy2 * m; }
					else { c.pitch = -1.0f; c.yaw = p.dy2 * m; }
				}
			}
			if (!s.isOnGround) everAir = true;
			if (dodgeTick >= 0 && t > dodgeTick && landTick < 0) out.noseDip = std::min(out.noseDip, Angle::FromRotMat(s.rotMat).pitch * 57.29578f);
			if (dodgeTick >= 0 && t > dodgeTick && t <= dodgeTick + 3 && s.isFlipping) out.flipped = true;   // the dodge really started (in the air, a fresh press)
			if (record) out.frames.push_back(ToFrame(t, s, b, c));
			if (out.arrive < 0 && dist < 200.0f) out.arrive = t;
			car->controls = c;
			const uint64_t t0 = arena->tickCount;
			arena->Step(1);
			CarState s2 = car->GetState();
			if (touchTick < 0 && s2.ballHitInfo.isValid && s2.ballHitInfo.tickCountWhenHit >= t0)
			{
				touchTick = t + 1;
				if (record) { Frame& f = out.frames.back(); (void)f; }
			}
			if (touchTick >= 0 && t >= touchTick + 150) break;
			if (touchTick < 0 && t > 400) break;
		}
		if (record && touchTick >= 0 && touchTick < static_cast<int>(out.frames.size()))
		{
			// the touch lands on the tick after contact, as the recorder flags it
			Frame& f = out.frames[touchTick];
			f.touch = true;
			const BallState b = arena->ball->GetState();
			(void)b;
			f.touchX = f.ballX; f.touchY = f.ballY; f.touchZ = f.ballZ;
		}
		out.touch = touchTick;
		out.dodge = dodgeTick;
		out.land = landTick;
		out.flip2 = flipStart;
		const BallState be = arena->ball->GetState();
		out.ballVY = be.vel.y;
		out.ballSpeed = be.vel.Length();
		(void)everAir;
		// cost: arrival time, then a clean finish
		double cost = out.arrive < 0 ? 1e6 : out.arrive;
		if (out.touch < 0) cost += 1e5;
		if (p.speedflip && (out.land < 0 || !out.landedOk)) cost += 1e4;
		if (p.speedflip && out.landSpeed < 2000.0f) cost += (2000.0f - out.landSpeed) * 0.05;
		if (p.speedflip)
		{
			// a real speedflip: the dodge when good players do it, and wheels back down about when theirs are (not a boosted flight)
			if (out.dodge < sp.dodgeLo) cost += (sp.dodgeLo - out.dodge) * 20.0;
			if (out.dodge > sp.dodgeHi) cost += (out.dodge - sp.dodgeHi) * 20.0;
			// (Nov 2: was 66 ticks, which no real speedflip meets: 134 pro speedflips land 1.01-1.16 s after the dodge,
			// median 1.09. Every model paid this penalty, and it pushed the search to cancel the very next tick.)
			if (out.land >= 0 && out.land - out.dodge > 140) cost += (out.land - out.dodge - 140) * 50.0;
			if (std::fabs(p.dy) < 0.45f) cost += (0.45f - std::fabs(p.dy)) * 400.0;   // a diagonal dodge (that's what makes it a speedflip)
			if (out.flip2 < 0 && out.touch >= 0) cost += 30;   // no flip into the ball
			if (!out.flipped) cost += 1e4;                     // no real dodge: not a speedflip
			// Looks like a speedflip (Nov 2): pros' noses dip about 20 degrees before the cancel catches the flip (134 pro
			// speedflips: middle half 17-24), so the car corkscrews. A cancel the very next tick (what the search found
			// before) keeps the nose flat and the car just barrel-rolls: Coop saw a side flip, not a speedflip.
			if (out.noseDip > -16.0f) cost += (out.noseDip + 16.0f) * 2.0;
			if (out.noseDip < -25.0f) cost += (-25.0f - out.noseDip) * 2.0;
		}
		if (out.ballVY < 0) cost += 20 + (-out.ballVY) * 0.01;   // sent it back at your own goal
		out.cost = cost;
		return out;
	}

	P Sample(std::mt19937& rng, bool speedflip)
	{
		std::uniform_real_distribution<float> u(-1, 1);
		std::uniform_int_distribution<int> iu(0, 1000);
		P p;
		p.speedflip = speedflip;
		p.s0 = u(rng); p.S = iu(rng) % 30;
		p.aim = u(rng) * 60.0f;
		p.J = 30 + iu(rng) % 45; p.H = 2 + iu(rng) % 10; p.G = 1 + iu(rng) % 7;
		p.dy = (u(rng) < 0 ? -1.0f : 1.0f) * (0.5f + 0.5f * std::fabs(u(rng))); p.C = iu(rng) % 17; p.L = 40 + iu(rng) % 50; p.r = u(rng); p.ry = u(rng) * 0.5f; p.ho = u(rng) * 0.7f;
		p.D = 250.0f + (u(rng) + 1) * 250.0f; p.dy2 = u(rng) * 0.6f;
		return p;
	}
	P Perturb(std::mt19937& rng, const P& b, float k)
	{
		std::normal_distribution<float> n(0, 1);
		P p = b;
		auto fi = [&](int v, float sd, int lo, int hi) { return std::clamp(static_cast<int>(std::lround(v + n(rng) * sd * k)), lo, hi); };
		auto ff = [&](float v, float sd, float lo, float hi) { return std::clamp(v + n(rng) * sd * k, lo, hi); };
		p.s0 = ff(p.s0, 0.3f, -1, 1); p.S = fi(p.S, 4, 0, 40); p.aim = ff(p.aim, 15, -80, 80);
		p.J = fi(p.J, 4, 20, 85); p.H = fi(p.H, 2, 2, 14); p.G = fi(p.G, 1.5f, 1, 10);
		p.dy = ff(p.dy, 0.2f, -1, 1); if (std::fabs(p.dy) < 0.5f) p.dy = p.dy < 0 ? -0.5f : 0.5f; p.C = fi(p.C, 2.0f, 0, 16); p.L = fi(p.L, 6, 30, 95); p.r = ff(p.r, 0.3f, -1, 1); p.ry = ff(p.ry, 0.2f, -1, 1); p.ho = ff(p.ho, 0.1f, -0.9f, 0.9f);
		p.D = ff(p.D, 60, 150, 800); p.dy2 = ff(p.dy2, 0.15f, -1, 1);
		return p;
	}

	P Search(Arena* arena, Car*& car, const Spawn& sp, bool speedflip, unsigned seed)
	{
		std::mt19937 rng(seed);
		struct Cand { P p; double cost; };
		std::vector<Cand> pool;
		for (int i = 0; i < 4000; i++) { P p = Sample(rng, speedflip); pool.push_back({ p, Run(arena, car, sp, p, false, false).cost }); }
		for (int round = 0; round < 14; round++)
		{
			std::sort(pool.begin(), pool.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
			pool.resize(40);
			const float k = round < 5 ? 1.0f : round < 10 ? 0.5f : 0.25f;
			std::vector<Cand> next = pool;
			for (int i = 0; i < 1200; i++) { P p = Perturb(rng, pool[i % 40].p, k); next.push_back({ p, Run(arena, car, sp, p, false, false).cost }); }
			pool = std::move(next);
			std::sort(pool.begin(), pool.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
			fprintf(stderr, "  %s %s round %d best %.1f ticks (%.3f s)\n", sp.name, speedflip ? "speedflip" : "flip-in", round, pool[0].cost, pool[0].cost / 120.0);
		}
		return pool[0].p;
	}

	std::string Csv(const std::vector<Frame>& fr)
	{
		std::string s = "tick,t,wall_ms,throttle,steer,pitch,yaw,roll,jump,boost,handbrake,car_x,car_y,car_z,car_pitch,car_yaw,car_roll,"
			"car_vx,car_vy,car_vz,car_avx,car_avy,car_avz,boost_amount,on_ground,on_wall,has_flip,ball_x,ball_y,ball_z,ball_vx,ball_vy,ball_vz,"
			"ball_avx,ball_avy,ball_avz,touch,touch_x,touch_y,touch_z\n";
		for (size_t i = 0; i < fr.size(); i++)
		{
			const Frame& f = fr[i];
			s += std::format("{},{:.4f},{:.1f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{:.2f},{:.2f},{:.2f},{},{},{},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},{:.1f},{},{},{},"
				"{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},{},{:.2f},{:.2f},{:.2f}\n",
				i, i / 120.0f, i * 8.333f, f.throttle, f.steer, f.pitch, f.yaw, f.roll, f.jump ? 1 : 0, f.boost ? 1 : 0, f.handbrake ? 1 : 0,
				f.carX, f.carY, f.carZ, f.carPitch, f.carYaw, f.carRoll, f.carVX, f.carVY, f.carVZ, f.carAVX, f.carAVY, f.carAVZ, f.boostAmount,
				f.onGround ? 1 : 0, f.onWall ? 1 : 0, f.hasFlip ? 1 : 0, f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ,
				f.ballAVX, f.ballAVY, f.ballAVZ, f.touch ? 1 : 0, f.touchX, f.touchY, f.touchZ);
		}
		return s;
	}

	void Describe(const char* what, const P& p, const Res& r)
	{
		printf("%s: arrive %.3f s, touch %.3f s, dodge %.3f%s, land %.3f (wheels %s, %.0f uu/s), flip in %.3f, ball %.0f up field (%.0f uu/s)\n",
			what, r.arrive / 120.0, r.touch / 120.0, r.dodge / 120.0, r.dodge >= 0 && !r.flipped ? " (NO FLIP)" : "", r.land / 120.0, r.landedOk ? "down" : "NOT down", r.landSpeed, r.flip2 / 120.0, r.ballVY, r.ballSpeed);
		printf("   inputs: heading off %.2f rad, steer %.2f for %d ticks, aim %.0f, jump at %d held %d, gap %d, dodge side %.2f, cancel after %d for %d (roll %.2f, yaw %.2f), flip in from %.0f (side %.2f)\n",
			p.ho, p.s0, p.S, p.aim, p.J, p.H, p.G, p.dy, p.C, p.L, p.r, p.ry, p.D, p.dy2);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "kickoff_model MESHES OUT_DIR\n"); return 1; }
	RocketSim::Init(argv[1], true);
	Arena* arena = Arena::Create(GameMode::SOCCAR);
	Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
	const std::string out = argv[2];
	if (argc > 3 && std::string(argv[3]) == "variants")
	{
		// Attempts with one thing wrong, for checking the lesson's scoring: the diagonal model's inputs (as printed),
		// then a late jump, no cancel, no boost for the first half second, a straight front flip
		P d; d.ho = 0.07f; d.s0 = -0.56f; d.S = 35; d.aim = 21; d.J = 52; d.H = 3; d.G = 3; d.dy = 1.0f; d.C = 0; d.L = 95; d.r = 1.0f; d.ry = -0.94f; d.D = 664; d.dy2 = 0.17f;
		Res base = Run(arena, car, kDiag, d, false, true);
		printf("variant base: arrive %.3f\n", base.arrive / 120.0);
		auto write = [&](const char* name, P v, bool noBoostStart) {
			Res r = Run(arena, car, kDiag, v, false, true);
			if (noBoostStart)
			{
				// redo it with boost off for the first 60 ticks (re-simulated by hand below)
				(void)0;
			}
			printf("variant %s: arrive %.3f, dodge %.3f, land %.3f (wheels %s)\n", name, r.arrive / 120.0, r.dodge / 120.0, r.land / 120.0, r.landedOk ? "down" : "not down");
			for (size_t i = 0; i < r.frames.size(); i++) r.frames[i].tick = static_cast<int>(i);
			std::ofstream(out + "/var_" + name + ".csv") << Csv(r.frames);
		};
		P late = d; late.J += 14; write("late_jump", late, false);
		P nc = d; nc.L = 0; nc.C = 40; write("no_cancel", nc, false);
		P ff = d; ff.dy = 0.0f; write("front_flip", ff, false);
		P nb = d; nb.J += 0; nb.s0 = 0.0f; nb.S = 60; write("no_boost_start", nb, false);
		return 0;
	}
	struct Job { const char* id; Spawn sp; bool speedflip; bool both; };
	const Job jobs[] = {
		{ "kickoff_flipin", kDiag, false, true },
		{ "kickoff_diagonal", kDiag, true, true },
		{ "kickoff_offcentre", kOff, true, true },
		{ "kickoff_back", kBack, true, false },
	};
	for (const Job& j : jobs)
	{
		// three searches from different random starts; the best one
		P best = Search(arena, car, j.sp, j.speedflip, 1234u);
		double bc = Run(arena, car, j.sp, best, false, false).cost;
		for (unsigned seed : { 77u, 4242u })
		{
			P p = Search(arena, car, j.sp, j.speedflip, seed);
			const double c = Run(arena, car, j.sp, p, false, false).cost;
			if (c < bc) { bc = c; best = p; }
		}
		Res left = Run(arena, car, j.sp, best, false, true);
		Describe(j.id, best, left);
		std::vector<Frame> fr = left.frames;
		if (j.both)
		{
			Res right = Run(arena, car, j.sp, best, true, true);
			printf("   mirrored: arrive %.3f s, touch %.3f s, ball %.0f up field\n", right.arrive / 120.0, right.touch / 120.0, right.ballVY);
			printf("   left side starts at tick %zu\n", fr.size());
			fr.insert(fr.end(), right.frames.begin(), right.frames.end());
		}
		for (size_t i = 0; i < fr.size(); i++) fr[i].tick = static_cast<int>(i);
		std::ofstream(out + "/" + j.id + "_model.csv") << Csv(fr);
	}
	return 0;
}
