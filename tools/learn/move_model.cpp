// Model reps for the movement lessons (Nov 2): the half flip, the speedflip (on the open field), the wavedash and the
// powerslide turn, each found in RocketSim the way tools/combine/kickoff_model.cpp finds the kickoffs: a parameterised
// input sequence, searched for the best result (time to turned-and-going, speed gained), then written as a lesson model
// rep with the ball parked out of the way. One file per lesson: <out>/<id>_model.csv.
//
//   move_model MESHES OUT_DIR
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
	constexpr float kPi = 3.14159265f;
	const Vec kBallPark{ 3600.0f, 4400.0f, 92.75f };   // the ball, parked in a far corner

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
		return g;
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
				0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f, 0.0f);
		}
		return s;
	}

	float Heading(const CarState& s) { return std::atan2(s.rotMat.forward.y, s.rotMat.forward.x); }
	float AngDiff(float a, float b) { float d = a - b; while (d > kPi) d -= 2 * kPi; while (d < -kPi) d += 2 * kPi; return std::fabs(d); }

	// Level out in the air for the landing (stick against the tilt), as a player does
	void LevelOut(const CarState& s, CarControls& c)
	{
		const Angle a = Angle::FromRotMat(s.rotMat);
		const float pr = s.angVel.Dot(s.rotMat.right), rr = s.angVel.Dot(s.rotMat.forward);
		c.pitch = std::clamp(-a.pitch * 3.0f + pr * 0.4f, -1.0f, 1.0f);
		c.roll = std::clamp(-a.roll * 3.0f - rr * 0.4f, -1.0f, 1.0f);
	}

	// Steer on the ground toward a heading
	float SteerTo(const CarState& s, float want) { float e = want - Heading(s); while (e > kPi) e -= 2 * kPi; while (e < -kPi) e += 2 * kPi; return std::clamp(e * 3.0f, -1.0f, 1.0f); }

	struct P { int a = 0, b = 0, c = 0, d = 0, e = 0, f = 0; float x = 0, y = 0, z = 0; };
	struct Res { double cost = 1e9; int act = -1, dodge = -1, land = -1, done = -1; float gain = 0, endSpeed = 0, noseDip = 0; bool ok = false; std::vector<Frame> frames; };

	enum class Move { HalfFlip, Speedflip, Wavedash, Powerslide, Recovery };
	const Vec kBackPost{ 800.0f, -4900.0f, 17.0f };   // recovery: get back to your back post
	const Vec kRecoveryBall{ 1700.0f, -4300.0f, 92.75f };   // recovery: the ball in your corner, so ball cam looks about the way you go

	Res Run(Arena* arena, Car*& car, Move mv, const P& p, bool record)
	{
		if (car) arena->RemoveCar(car);
		car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
		CarState cs;
		cs.pos = { 0.0f, -1500.0f, 17.0f };
		cs.rotMat = Angle(kPi * 0.5f, 0, 0).ToRotMat();   // facing +y (up field)
		cs.isOnGround = true;
		cs.boost = mv == Move::Wavedash ? 0.0f : 100.0f;
		if (mv == Move::Wavedash) cs.vel = { 0.0f, 1400.0f, 0.0f };   // top speed without boost: anything more is the wavedash
		if (mv == Move::Powerslide) cs.vel = { 0.0f, 1400.0f, 0.0f };
		if (mv == Move::Recovery)
		{
			// knocked into the air: tilted over, spinning, drifting toward the side wall (a Combine-style throw, level 2)
			// (Nov 3, Coop: "built around a weird rep") lower and shorter: about a second in the air, like a bump or a
			// 50/50, not two seconds of hang time
			cs.pos = { -1200.0f, -1500.0f, 220.0f };
			cs.vel = { -650.0f, 600.0f, 220.0f };
			cs.angVel = { 2.5f, -2.0f, 1.5f };
			cs.rotMat = Angle(0.4f, 0.5f, 2.3f).ToRotMat();
			cs.isOnGround = false;
			cs.hasJumped = true;
			cs.boost = 40.0f;
		}
		car->SetState(cs);
		BallState bs; bs.pos = mv == Move::Recovery ? kRecoveryBall : kBallPark; arena->ball->SetState(bs);

		Res out;
		const float up = kPi * 0.5f, down = -kPi * 0.5f;
		int dodgeTick = -1, landTick = -1, jumpTick = -1;
		const int maxT = 360;
		for (int t = 0; t < maxT; t++)
		{
			const CarState s = car->GetState();
			CarControls c;
			switch (mv)
			{
			case Move::HalfFlip:
			{
				// a = reverse ticks, b = jump held, c = gap, d = cancel after, e = cancel+roll ticks, x = air roll
				const int J = p.a + 2, D = J + p.b + p.c;   // (two still ticks first, so the jump press shows)
				if (t < J) { c.throttle = -1.0f; }
				else if (t < J + p.b) { c.throttle = -1.0f; c.jump = true; if (jumpTick < 0) jumpTick = t; }
				else if (t < D) { c.throttle = -1.0f; }
				else if (t == D) { c.jump = true; c.pitch = 1.0f; dodgeTick = t; }   // stick back: a backflip
				else if (landTick < 0)
				{
					if (t < D + p.d) c.pitch = 1.0f;
					else if (t < D + p.d + p.e) { c.pitch = -1.0f; c.roll = p.x; }   // stick forward cancels it; air roll brings you upright
					else LevelOut(s, c);
					c.throttle = 1.0f;
					c.boost = t > D + p.d && s.rotMat.forward.y < -0.5f;
					if (t > D + 4 && s.isOnGround) landTick = t;
				}
				else { c.throttle = 1.0f; c.boost = true; c.steer = SteerTo(s, down); }
				if (out.done < 0 && landTick >= 0 && s.isOnGround && AngDiff(Heading(s), down) < 0.26f && -s.vel.y > 1000.0f) out.done = t;
				break;
			}
			case Move::Speedflip:
			{
				// a = jump tick, b = held, c = gap, d = cancel after, e = cancel ticks, x = dodge sideways, y = air roll, z = yaw in the cancel
				const int D = p.a + p.b + p.c;
				c.throttle = 1.0f; c.boost = true;
				if (t < p.a) c.steer = SteerTo(s, up + p.f * 0.002f);
				else if (t < p.a + p.b) { c.jump = true; if (jumpTick < 0) jumpTick = t; }
				else if (t < D) {}
				else if (t == D) { c.jump = true; c.pitch = -1.0f; c.yaw = p.x; dodgeTick = t; }
				else if (landTick < 0)
				{
					if (t < D + p.d) { c.pitch = -1.0f; c.yaw = p.x; }
					else if (t < D + p.d + p.e) { c.pitch = 1.0f; c.roll = p.y; c.yaw = p.z; }
					else LevelOut(s, c);
					if (t > D + 3 && s.isOnGround) landTick = t;
				}
				else c.steer = SteerTo(s, up);
				if (dodgeTick >= 0 && landTick < 0 && t > dodgeTick) out.noseDip = std::min(out.noseDip, Angle::FromRotMat(s.rotMat).pitch * 57.29578f);
				if (out.done < 0 && landTick >= 0 && s.isOnGround && s.vel.Length() >= 2200.0f) out.done = t;
				break;
			}
			case Move::Wavedash:
			{
				// a = jump held, b = tilt-back ticks, x = tilt amount (pitch), z = dodge height (car z while falling)
				c.throttle = 1.0f;
				if (t < 2) {}
				else if (t < 2 + p.a) { c.jump = true; if (jumpTick < 0) jumpTick = t; }
				else if (dodgeTick < 0)
				{
					if (t > 2 + p.a + 4 && s.isOnGround) { landTick = t; break; }   // landed without the flip: not a wavedash
					if (t < 2 + p.a + p.b) c.pitch = p.x;   // tilt the nose up a little
					else { LevelOut(s, c); c.pitch = p.y; }
					if (!s.isOnGround && s.vel.z < 0.0f && s.pos.z < p.z) { c.jump = true; c.pitch = -1.0f; c.yaw = 0; c.roll = 0; dodgeTick = t; }
				}
				else if (landTick < 0) { c.pitch = -1.0f; if (s.isOnGround) landTick = t; }
				if (out.done < 0 && landTick >= 0 && t >= landTick + 30) { out.done = t; out.endSpeed = s.vel.Length(); }
				break;
			}
			case Move::Recovery:
			{
				// x, y = the level-out gains, z = yaw toward where you're going in the air, a = boost in the air when facing the spot (>0)
				const Vec to = kBackPost - s.pos;
				const float want = std::atan2(to.y, to.x);
				if (!s.isOnGround)
				{
					const Angle an = Angle::FromRotMat(s.rotMat);
					const float pr = s.angVel.Dot(s.rotMat.right), rr = s.angVel.Dot(s.rotMat.forward), yr = s.angVel.Dot(s.rotMat.up);
					c.pitch = std::clamp(-an.pitch * p.x + pr * p.y, -1.0f, 1.0f);
					c.roll = std::clamp(-an.roll * p.x - rr * p.y, -1.0f, 1.0f);
					float e = want - an.yaw; while (e > kPi) e -= 2 * kPi; while (e < -kPi) e += 2 * kPi;
					c.yaw = std::clamp(e * p.z - yr * 0.3f, -1.0f, 1.0f);
					c.boost = p.a > 0 && AngDiff(an.yaw, want) < 0.5f && std::fabs(an.pitch) < 0.3f && std::fabs(an.roll) < 0.3f;
					c.throttle = 1.0f;
				}
				else
				{
					if (landTick < 0) { landTick = t; out.ok = s.rotMat.up.z > 0.85f; }
					c.throttle = 1.0f; c.boost = true; c.steer = SteerTo(s, want);
				}
				if (out.done < 0 && landTick >= 0 && s.isOnGround && s.rotMat.up.z > 0.9f && AngDiff(Heading(s), want) < 0.5f && s.vel.Dot(to.Normalized()) > 1000.0f) out.done = t;
				break;
			}
			case Move::Powerslide:
			{
				// a = handbrake ticks, x = throttle in the slide, y = boost in the slide (>0.5)
				if (t < p.a) { c.steer = 1.0f; c.handbrake = true; c.throttle = p.x; c.boost = p.y > 0.5f; if (jumpTick < 0) jumpTick = t; }
				else { c.throttle = 1.0f; c.boost = true; c.steer = SteerTo(s, down); }
				if (out.done < 0 && t >= p.a && AngDiff(Heading(s), down) < 0.26f && -s.vel.y > 1000.0f) out.done = t;
				break;
			}
			}
			if (record) out.frames.push_back(ToFrame(t, s, arena->ball->GetState(), c));
			car->controls = c;
			arena->Step(1);
			if (out.done >= 0 && t >= out.done + 60) break;
		}
		const CarState e = car->GetState();
		out.act = jumpTick;
		out.dodge = dodgeTick;
		out.land = landTick;
		const bool wheelsFirst = mv != Move::Recovery || out.ok;
		out.ok = out.done >= 0 && e.rotMat.up.z > 0.8f && wheelsFirst;
		switch (mv)
		{
		case Move::HalfFlip:
		case Move::Powerslide:
		case Move::Recovery:
			out.cost = out.done < 0 ? 1e6 : out.done;
			if (mv == Move::HalfFlip && out.land < 0) out.cost += 1e5;
			break;
		case Move::Speedflip:
		{
			out.cost = out.done < 0 ? 1e6 : out.done;
			if (std::fabs(p.x) < 0.45f) out.cost += (0.45f - std::fabs(p.x)) * 400.0;
			if (out.noseDip > -16.0f) out.cost += (out.noseDip + 16.0f) * 2.0;   // like a pro speedflip (see kickoff_model.cpp)
			if (out.noseDip < -25.0f) out.cost += (-25.0f - out.noseDip) * 2.0;
			if (out.land >= 0 && out.dodge >= 0 && out.land - out.dodge > 140) out.cost += (out.land - out.dodge - 140) * 50.0;
			if (out.frames.empty()) {}
			break;
		}
		case Move::Wavedash:
			out.gain = out.endSpeed - 1400.0f;
			out.cost = out.done < 0 || out.dodge < 0 ? 1e6 : -out.gain;
			if (out.land >= 0 && out.dodge >= 0 && out.land - out.dodge > 8) out.cost += (out.land - out.dodge - 8) * 20.0;   // a wavedash lands right as it flips
			break;
		}
		if (!out.ok) out.cost += 1e4;
		return out;
	}

	P Sample(std::mt19937& rng, Move mv)
	{
		std::uniform_real_distribution<float> u(-1, 1);
		std::uniform_int_distribution<int> iu(0, 1000);
		P p;
		switch (mv)
		{
		case Move::HalfFlip: p.a = iu(rng) % 30; p.b = 2 + iu(rng) % 10; p.c = 1 + iu(rng) % 8; p.d = 4 + iu(rng) % 30; p.e = 20 + iu(rng) % 70; p.x = u(rng); break;
		case Move::Speedflip: p.a = 20 + iu(rng) % 50; p.b = 2 + iu(rng) % 10; p.c = 1 + iu(rng) % 7; p.d = iu(rng) % 17; p.e = 40 + iu(rng) % 50; p.x = (u(rng) < 0 ? -1.0f : 1.0f) * (0.5f + 0.5f * std::fabs(u(rng))); p.y = u(rng); p.z = u(rng) * 0.6f; p.f = iu(rng) % 200 - 100; break;
		case Move::Wavedash: p.a = 1 + iu(rng) % 12; p.b = iu(rng) % 40; p.x = u(rng) * 0.5f + 0.3f; p.y = u(rng) * 0.3f; p.z = 25.0f + (u(rng) + 1) * 20.0f; break;
		case Move::Powerslide: p.a = 10 + iu(rng) % 80; p.x = (u(rng) + 1) * 0.5f; p.y = u(rng); break;
		case Move::Recovery: p.x = 1.0f + (u(rng) + 1) * 3.0f; p.y = (u(rng) + 1) * 0.5f; p.z = (u(rng) + 1) * 2.0f; p.a = iu(rng) % 2; break;
		}
		return p;
	}
	P Perturb(std::mt19937& rng, const P& b, Move mv, float k)
	{
		std::normal_distribution<float> n(0, 1);
		P p = b;
		auto fi = [&](int v, float sd, int lo, int hi) { return std::clamp(static_cast<int>(std::lround(v + n(rng) * sd * k)), lo, hi); };
		auto ff = [&](float v, float sd, float lo, float hi) { return std::clamp(v + n(rng) * sd * k, lo, hi); };
		switch (mv)
		{
		case Move::HalfFlip: p.a = fi(p.a, 3, 0, 40); p.b = fi(p.b, 2, 2, 14); p.c = fi(p.c, 1.5f, 1, 10); p.d = fi(p.d, 3, 2, 40); p.e = fi(p.e, 8, 10, 100); p.x = ff(p.x, 0.3f, -1, 1); break;
		case Move::Speedflip: p.a = fi(p.a, 4, 10, 80); p.b = fi(p.b, 2, 2, 14); p.c = fi(p.c, 1.5f, 1, 10); p.d = fi(p.d, 2, 0, 16); p.e = fi(p.e, 6, 30, 95); p.x = ff(p.x, 0.2f, -1, 1); if (std::fabs(p.x) < 0.5f) p.x = p.x < 0 ? -0.5f : 0.5f; p.y = ff(p.y, 0.3f, -1, 1); p.z = ff(p.z, 0.2f, -1, 1); p.f = fi(p.f, 20, -150, 150); break;
		case Move::Wavedash: p.a = fi(p.a, 2, 1, 14); p.b = fi(p.b, 5, 0, 60); p.x = ff(p.x, 0.15f, -1, 1); p.y = ff(p.y, 0.1f, -1, 1); p.z = ff(p.z, 4, 18, 90); break;
		case Move::Powerslide: p.a = fi(p.a, 6, 5, 120); p.x = ff(p.x, 0.15f, 0, 1); p.y = ff(p.y, 0.4f, -1, 1); break;
		case Move::Recovery: p.x = ff(p.x, 0.5f, 0.5f, 8); p.y = ff(p.y, 0.15f, 0, 1.5f); p.z = ff(p.z, 0.4f, 0, 5); if (n(rng) > 1.2f) p.a = 1 - p.a; break;
		}
		return p;
	}

	P Search(Arena* arena, Car*& car, Move mv, unsigned seed, const char* name)
	{
		std::mt19937 rng(seed);
		struct Cand { P p; double cost; };
		std::vector<Cand> pool;
		for (int i = 0; i < 3000; i++) { P p = Sample(rng, mv); pool.push_back({ p, Run(arena, car, mv, p, false).cost }); }
		for (int round = 0; round < 12; round++)
		{
			std::sort(pool.begin(), pool.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
			pool.resize(40);
			const float k = round < 4 ? 1.0f : round < 8 ? 0.5f : 0.25f;
			std::vector<Cand> next = pool;
			for (int i = 0; i < 800; i++) { P p = Perturb(rng, pool[i % 40].p, mv, k); next.push_back({ p, Run(arena, car, mv, p, false).cost }); }
			pool = std::move(next);
			std::sort(pool.begin(), pool.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
			fprintf(stderr, "  %s round %d best %.1f\n", name, round, pool[0].cost);
		}
		return pool[0].p;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "move_model MESHES OUT_DIR [only]\n"); return 1; }
	RocketSim::Init(argv[1], true);
	Arena* arena = Arena::Create(GameMode::SOCCAR);
	Car* car = nullptr;
	const std::string out = argv[2];
	const std::string only = argc > 3 ? argv[3] : "";
	if (only == "variants")
	{
		// Attempts with one thing wrong, for checking the lessons' scoring (the models' own inputs, as printed, changed)
		auto write = [&](const char* name, Move mv, P p) {
			Res r = Run(arena, car, mv, p, true);
			printf("variant %s: done %.3f, gain %.0f\n", name, r.done / 120.0, r.gain);
			for (size_t i = 0; i < r.frames.size(); i++) r.frames[i].tick = static_cast<int>(i);
			std::ofstream(out + "/var_" + name + ".csv") << Csv(r.frames);
		};
		P hf; hf.a = 0; hf.b = 4; hf.c = 1; hf.d = 40; hf.e = 56; hf.x = -0.93f;
		P h1 = hf; h1.d = 300; write("half_flip_nocancel", Move::HalfFlip, h1);
		P h2 = hf; h2.d = 70; write("half_flip_latecancel", Move::HalfFlip, h2);
		P sf; sf.a = 29; sf.b = 8; sf.c = 1; sf.d = 5; sf.e = 59; sf.f = 31; sf.x = -0.74f; sf.y = -1.0f; sf.z = 0.99f;
		P s1 = sf; s1.x = 0.0f; write("speedflip_straight", Move::Speedflip, s1);
		P s2 = sf; s2.d = 40; write("speedflip_latecancel", Move::Speedflip, s2);
		P wd; wd.a = 13; wd.b = 55; wd.x = -0.12f; wd.y = 0.52f; wd.z = 30.76f;
		P w1 = wd; w1.z = 70.0f; write("wavedash_early", Move::Wavedash, w1);
		P w2 = wd; w2.y = -0.3f; w2.x = -0.3f; write("wavedash_flat", Move::Wavedash, w2);
		P ps; ps.a = 40; ps.x = 0.88f; ps.y = 0.9f;
		P p1 = ps; p1.a = 8; write("powerslide_short", Move::Powerslide, p1);
		P p2 = ps; p2.a = 90; write("powerslide_long", Move::Powerslide, p2);
		P rc; rc.x = 0.78f; rc.y = 0.05f; rc.z = 1.53f; rc.a = 1;
		P r1 = rc; r1.x = 0; r1.y = 0; r1.z = 0; r1.a = 0; write("recovery_nothing", Move::Recovery, r1);
		P r2 = rc; r2.z = 0; write("recovery_noyaw", Move::Recovery, r2);
		return 0;
	}
	struct Job { const char* id; Move mv; };
	const Job jobs[] = { { "half_flip", Move::HalfFlip }, { "speedflip", Move::Speedflip }, { "wavedash", Move::Wavedash }, { "powerslide_turn", Move::Powerslide }, { "recovery", Move::Recovery } };
	for (const Job& j : jobs)
	{
		if (!only.empty() && only != j.id) continue;
		P best = Search(arena, car, j.mv, 1234u, j.id);
		double bc = Run(arena, car, j.mv, best, false).cost;
		for (unsigned seed : { 77u, 4242u })
		{
			P p = Search(arena, car, j.mv, seed, j.id);
			const double c = Run(arena, car, j.mv, p, false).cost;
			if (c < bc) { bc = c; best = p; }
		}
		Res r = Run(arena, car, j.mv, best, true);
		printf("%s: cost %.1f, act %.3f s, dodge %.3f, land %.3f, done %.3f s, gain %.0f, end speed %.0f, nose dip %.0f | a %d b %d c %d d %d e %d f %d x %.2f y %.2f z %.2f\n",
			j.id, r.cost, r.act / 120.0, r.dodge / 120.0, r.land / 120.0, r.done / 120.0, r.gain, r.endSpeed, r.noseDip, best.a, best.b, best.c, best.d, best.e, best.f, best.x, best.y, best.z);
		fflush(stdout);
		for (size_t i = 0; i < r.frames.size(); i++) r.frames[i].tick = static_cast<int>(i);
		std::ofstream(out + "/" + j.id + "_model.csv") << Csv(r.frames);
	}
	return 0;
}
