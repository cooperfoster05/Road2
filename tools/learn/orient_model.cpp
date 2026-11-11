// Model reps for the air-control lessons (Nov 3): air roll control (three orientation drills in one flight: a full
// roll stopped level, nose to the ball, wheels to the ball) and the tornado spin (jump and boost up to a high ball,
// holding air roll the whole way, nose on the ball). A PD attitude controller on RocketSim, gains picked by a small
// grid; written like move_model.cpp's models.
//
//   orient_model MESHES OUT_DIR
#include "third_party/RocketSim/src/RocketSim.h"
#include "RepData.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace RocketSim;

namespace
{
	constexpr float kR2U = 32768.0f / 3.14159265f;
	constexpr float kPi = 3.14159265f;

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
				0.0f, 0.0f, 0.0f, f.touch ? 1 : 0, f.touchX, f.touchY, f.touchZ);
		}
		return s;
	}

	float Ang(const Vec& a, const Vec& b) { return std::acos(std::clamp(a.Normalized().Dot(b.Normalized()), -1.0f, 1.0f)); }

	// Turn `axis` (a unit vector on the car, in world space) toward `want`; rollFixed keeps the roll input as given
	void Aim(const CarState& s, const Vec& axis, const Vec& want, float kp, float g, CarControls& c, bool rollFixed)
	{
		const Vec k = axis.Cross(want.Normalized());
		const float sn = k.Length(), th = std::atan2(sn, axis.Dot(want.Normalized()));
		const Vec wDes = sn > 1e-4f ? k * (kp * th / sn) : Vec(0, 0, 0);
		const Vec err = wDes - s.angVel;
		c.pitch = std::clamp(-g * err.Dot(s.rotMat.right), -1.0f, 1.0f);
		c.yaw = std::clamp(g * err.Dot(s.rotMat.up), -1.0f, 1.0f);
		if (!rollFixed) c.roll = std::clamp(-g * err.Dot(s.rotMat.forward), -1.0f, 1.0f);
	}

	struct Out { int done[3] = { -1, -1, -1 }; int touch = -1; float aimErr = 0, rollShare = 0; std::vector<Frame> frames; };

	// Air roll control: (1) roll a full turn and stop level, (2) nose to the ball, (3) wheels to the ball
	Out AirRoll(Arena* arena, float kp, float g, float rkp, float rkd, bool record)
	{
		Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
		CarState cs; cs.pos = { 0.0f, -2400.0f, 1400.0f }; cs.vel = { 0.0f, 250.0f, 600.0f };
		cs.rotMat = Angle(kPi * 0.5f, 0, 0).ToRotMat(); cs.isOnGround = false; cs.hasJumped = true; cs.boost = 0.0f;
		car->SetState(cs);
		BallState bs; bs.pos = { -500.0f, -1300.0f, 1700.0f }; bs.vel = { 0.0f, 0.0f, 500.0f }; arena->ball->SetState(bs);
		Out o;
		int step = 0, held = 0;
		float rolled = 0.0f;
		Vec upPrev = cs.rotMat.up;
		for (int t = 0; t < 480 && (step < 3 || t <= o.done[2] + 60); t++)
		{
			const CarState s = car->GetState();
			const BallState b = arena->ball->GetState();
			if (s.isOnGround) break;
			// how far it's rolled (about its nose), from the roof's turn
			const Vec cr = upPrev.Cross(s.rotMat.up);
			rolled += -std::atan2(s.rotMat.forward.Dot(cr), upPrev.Dot(s.rotMat.up));
			upPrev = s.rotMat.up;
			CarControls c;
			bool ok = false;
			const Vec toBall = b.pos - s.pos;
			if (step == 0)
			{
				const float rate = -s.angVel.Dot(s.rotMat.forward);
				c.roll = std::clamp(rkp * (2 * kPi - rolled) - rkd * rate, -1.0f, 1.0f);
				c.pitch = std::clamp(g * s.angVel.Dot(s.rotMat.right), -1.0f, 1.0f);
				c.yaw = std::clamp(-g * s.angVel.Dot(s.rotMat.up), -1.0f, 1.0f);
				ok = std::fabs(2 * kPi - rolled) < 0.35f && s.rotMat.up.z > std::cos(0.35f) && std::fabs(rate) < 1.0f;
			}
			else if (step == 1) { Aim(s, s.rotMat.forward, toBall, kp, g, c, false); ok = Ang(s.rotMat.forward, toBall) < 0.26f; }
			else { Aim(s, s.rotMat.up * -1.0f, toBall, kp, g, c, false); ok = Ang(s.rotMat.up * -1.0f, toBall) < 0.44f; }
			if (record) o.frames.push_back(ToFrame(t, s, b, c));
			held = ok ? held + 1 : 0;
			if (step < 3 && held >= (step == 0 ? 15 : 30)) { o.done[step] = t; step++; held = 0; }
			car->controls = c;
			arena->Step(1);
		}
		arena->RemoveCar(car);
		return o;
	}

	// The tornado spin (Nov 4, Coop: the first model "just looks like holding air roll and boosting"; then: "air rolling
	// right, the stick should be pushed exactly to the left"): directional air roll with the stick held the other way, so
	// the car turns about an axis about 30 degrees off its nose and the nose circles the ball. Jump, boost, point that
	// axis at the ball (aimT ticks), then hold air roll + the opposite stick and corkscrew into it. rollDir: +1 air roll
	// right (stick left), -1 air roll left (stick right).
	Out Tornado(Arena* arena, float kp, float g, int jumpT, float lead, bool record, int aimT = 0, float rollDir = 1.0f)
	{
		Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
		CarState cs; cs.pos = { 0.0f, -2700.0f, 17.0f }; cs.rotMat = Angle(kPi * 0.5f, 0, 0).ToRotMat(); cs.isOnGround = true; cs.boost = 100.0f;
		car->SetState(cs);
		BallState bs; bs.pos = { 0.0f, -1000.0f, 1000.0f }; bs.vel = { 0.0f, 0.0f, 320.0f }; arena->ball->SetState(bs);
		Out o;
		int air = 0, rollOn = 0, leave = -1;
		const uint64_t hits0 = car->GetState().ballHitInfo.tickCountWhenHit;
		// the spin axis in the car's own frame at full air roll + the opposite stick (measured in RocketSim: 0.87 forward,
		// 0.49 up, either way round)
		const float ca = 0.87f, cu = 0.49f;
		for (int t = 0; t < 360; t++)
		{
			const CarState s = car->GetState();
			const BallState b = arena->ball->GetState();
			CarControls c;
			c.throttle = 1.0f; c.boost = true;
			c.jump = t < jumpT;
			if (!s.isOnGround)
			{
				if (leave < 0) leave = t;
				const Vec aim = b.pos + b.vel * lead - s.pos;
				if (t < leave + aimT)
				{
					const Vec axis = s.rotMat.forward * ca + s.rotMat.up * cu;
					Aim(s, axis.Normalized(), aim, kp, g, c, false);
				}
				else { c.roll = rollDir; c.yaw = -rollDir; c.pitch = 0.0f; }
				air++; rollOn += std::fabs(c.roll) >= 0.7f;
			}
			Frame f = ToFrame(t, s, b, c);
			if (s.ballHitInfo.isValid && s.ballHitInfo.tickCountWhenHit != hits0 && o.touch < 0)
			{
				o.touch = t; f.touch = true; f.touchX = s.ballHitInfo.ballPos.x; f.touchY = s.ballHitInfo.ballPos.y; f.touchZ = s.ballHitInfo.ballPos.z;
			}
			if (record) o.frames.push_back(f);
			if (o.touch >= 0 && t >= o.touch + 90) break;
			if (o.touch < 0 && s.isOnGround && t > jumpT + 30) break;
			car->controls = c;
			arena->Step(1);
		}
		o.rollShare = air ? static_cast<float>(rollOn) / air : 0.0f;
		arena->RemoveCar(car);
		return o;
	}

	// The tornado hover (Nov 4, Coop: "shouldn't it be demonstrated by using it to hover, not go for the ball?"): jump,
	// point the spin axis straight up, then air roll + the opposite stick with boost. The nose circles about 30 degrees
	// off vertical, so the boost's push averages straight up: feather it and you hang in the air in one spot.
	// hoverZ: the height to hold. Records `hold` ticks of hovering after the spin starts (or until it falls).
	Out Hover(Arena* arena, float kp, float g, int jumpT, int aimT, float hoverZ, int hold, bool record, bool spin = true)
	{
		Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
		CarState cs; cs.pos = { 0.0f, -2000.0f, 17.0f }; cs.rotMat = Angle(kPi * 0.5f, 0, 0).ToRotMat(); cs.isOnGround = true; cs.boost = 100.0f;
		car->SetState(cs);
		BallState bs; bs.pos = { 3600.0f, 4400.0f, 92.75f }; arena->ball->SetState(bs);   // parked in a far corner
		Out o;
		int leave = -1, air = 0, rollOn = 0;
		const float ca = 0.87f, cu = 0.49f;
		for (int t = 0; t < 900; t++)
		{
			const CarState s = car->GetState();
			const BallState b = arena->ball->GetState();
			CarControls c;
			c.jump = t < jumpT;
			if (!s.isOnGround)
			{
				if (leave < 0) leave = t;
				const Vec up(0, 0, 1);
				if (t < leave + aimT)
				{
					const Vec axis = s.rotMat.forward * ca + s.rotMat.up * cu;
					Aim(s, axis.Normalized(), up, kp, g, c, false);
					c.boost = s.rotMat.forward.z > 0.5f;
				}
				else
				{
					if (spin) { c.roll = 1.0f; c.yaw = -1.0f; }
					else Aim(s, s.rotMat.forward, up, kp, g, c, false);   // the variant: nose straight up, no spin
					// feather the boost to hold the height
					const float wantVz = std::clamp((hoverZ - s.pos.z) * 1.5f, -300.0f, 400.0f);
					c.boost = s.vel.z < wantVz && s.boost > 0.0f;
				}
				c.steer = c.yaw;   // one stick: steer and yaw together, as a pad records them
				air++; rollOn += std::fabs(c.roll) >= 0.7f;
			}
			if (record) o.frames.push_back(ToFrame(t, s, b, c));
			if (leave >= 0 && t >= leave + aimT + hold) { o.done[0] = t; break; }
			if (s.isOnGround && t > jumpT + 30) break;
			car->controls = c;
			arena->Step(1);
		}
		o.rollShare = air ? static_cast<float>(rollOn) / air : 0.0f;
		o.touch = leave;
		arena->RemoveCar(car);
		return o;
	}

	Out TornadoOld(Arena* arena, float kp, float g, int jumpT, float lead, bool record)
	{
		Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
		CarState cs; cs.pos = { 0.0f, -2700.0f, 17.0f }; cs.rotMat = Angle(kPi * 0.5f, 0, 0).ToRotMat(); cs.isOnGround = true; cs.boost = 100.0f;
		car->SetState(cs);
		BallState bs; bs.pos = { 0.0f, -1200.0f, 950.0f }; bs.vel = { 0.0f, 0.0f, 300.0f }; arena->ball->SetState(bs);
		Out o;
		int air = 0, rollOn = 0, aimN = 0;
		const uint64_t hits0 = car->GetState().ballHitInfo.tickCountWhenHit;
		for (int t = 0; t < 360; t++)
		{
			const CarState s = car->GetState();
			const BallState b = arena->ball->GetState();
			CarControls c;
			c.throttle = 1.0f; c.boost = true;
			c.jump = t < jumpT;
			if (!s.isOnGround)
			{
				c.roll = 1.0f;   // air roll, held
				const Vec aim = b.pos + b.vel * lead - s.pos;
				Aim(s, s.rotMat.forward, aim, kp, g, c, true);
				air++; rollOn++;
				if (t > 36) { o.aimErr += Ang(s.rotMat.forward, b.pos - s.pos); aimN++; }
			}
			Frame f = ToFrame(t, s, b, c);
			if (s.ballHitInfo.isValid && s.ballHitInfo.tickCountWhenHit != hits0 && o.touch < 0)
			{
				o.touch = t; f.touch = true; f.touchX = s.ballHitInfo.ballPos.x; f.touchY = s.ballHitInfo.ballPos.y; f.touchZ = s.ballHitInfo.ballPos.z;
			}
			if (record) o.frames.push_back(f);
			if (o.touch >= 0 && t >= o.touch + 90) break;
			if (o.touch < 0 && s.isOnGround && t > jumpT + 30) break;
			car->controls = c;
			arena->Step(1);
		}
		o.aimErr = aimN ? o.aimErr / aimN : 9.0f;
		o.rollShare = air ? static_cast<float>(rollOn) / air : 0.0f;
		arena->RemoveCar(car);
		return o;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "orient_model MESHES OUT_DIR\n"); return 1; }
	RocketSim::Init(argv[1], true);
	Arena* arena = Arena::Create(GameMode::SOCCAR);
	const std::string out = argv[2];
	{
		float best = 1e9f, bk = 0, bg = 0, brp = 0, brd = 0;
		for (float kp : { 2.0f, 3.0f, 4.0f, 6.0f, 8.0f })
			for (float g : { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f })
				for (float rp : { 1.0f, 2.0f, 3.0f, 5.0f })
					for (float rd : { 0.1f, 0.2f, 0.35f, 0.5f })
					{
						Out o = AirRoll(arena, kp, g, rp, rd, false);
						if (o.done[2] < 0) continue;
						if (o.done[2] < best) { best = static_cast<float>(o.done[2]); bk = kp; bg = g; brp = rp; brd = rd; }
					}
		Out o = AirRoll(arena, bk, bg, brp, brd, true);
		printf("air_roll: steps done at %.3f %.3f %.3f s (kp %.1f g %.1f roll kp %.1f kd %.2f), %zu frames\n", o.done[0] / 120.0, o.done[1] / 120.0, o.done[2] / 120.0, bk, bg, brp, brd, o.frames.size());
		// let it fall a little past the last step (the watch clip), then write
		for (size_t i = 0; i < o.frames.size(); i++) o.frames[i].tick = static_cast<int>(i);
		std::ofstream(out + "/air_roll_model.csv") << Csv(o.frames);
		// variants for the lesson tests: a roll that doesn't stop, and no air control at all
		Out v1 = AirRoll(arena, bk, bg, brp, 0.0f, true);
		for (size_t i = 0; i < v1.frames.size(); i++) v1.frames[i].tick = static_cast<int>(i);
		std::ofstream(out + "/var_air_roll_overroll.csv") << Csv(v1.frames);
		Out v2 = AirRoll(arena, 0.0f, 0.0f, 0.0f, 0.0f, true);
		for (size_t i = 0; i < v2.frames.size(); i++) v2.frames[i].tick = static_cast<int>(i);
		std::ofstream(out + "/var_air_roll_nothing.csv") << Csv(v2.frames);
		printf("  variants: overroll done %d %d %d, nothing done %d %d %d\n", v1.done[0], v1.done[1], v1.done[2], v2.done[0], v2.done[1], v2.done[2]);
	}
	{
		// the longest spin that still hits the ball (a tornado spin is the spin, not the shortest path)
		float best = -1e9f, bk = 0, bg = 0, bl = 0, bp = 1.0f; int bj = 0, ba = 0;
		for (float kp : { 3.0f, 5.0f, 8.0f })
			for (float g : { 1.0f, 2.0f, 3.0f })
				for (int j : { 12, 18, 24 })
					for (float lead : { 0.0f, 0.2f, 0.4f })
						for (int aimT : { 24, 36, 48, 60 })
							for (float pin : { 1.0f, -1.0f })
							{
								Out o = Tornado(arena, kp, g, j, lead, false, aimT, pin);
								if (o.touch < 0) continue;
								const float spinT = static_cast<float>(o.touch - j - aimT);
								const float score = std::min(spinT, 120.0f) - o.touch * 0.2f;   // about a second of spin, then sooner is better
								if (score > best) { best = score; bk = kp; bg = g; bj = j; bl = lead; ba = aimT; bp = pin; }
							}
		// (Nov 4) The model is the hover now: the spin held about 3 seconds at about 700 high, in one spot
		{
			float bestDrift = 1e9f, hk = 0, hg = 0; int ha = 0;
			for (float kp : { 3.0f, 5.0f, 8.0f })
				for (float g : { 1.0f, 2.0f, 3.0f })
					for (int aimT : { 36, 48, 60, 72 })
					{
						Out h = Hover(arena, kp, g, 24, aimT, 700.0f, 360, true);
						if (h.done[0] < 0) continue;
						const Frame& f0 = h.frames[h.touch + aimT];
						float drift = 0, low = 1e9f;
						for (int t = h.touch + aimT; t <= h.done[0]; t++) { const Frame& f = h.frames[t]; drift = std::max(drift, std::hypot(f.carX - f0.carX, f.carY - f0.carY)); if (t > h.touch + aimT + 120) low = std::min(low, f.carZ); }
						if (getenv("DBG")) printf("  kp %.0f g %.0f aim %d: drift %.0f low %.0f\n", kp, g, aimT, drift, low);
						if (low < 300.0f) continue;
						if (drift < bestDrift) { bestDrift = drift; hk = kp; hg = g; ha = aimT; }
					}
			Out h = Hover(arena, hk, hg, 24, ha, 700.0f, 360, true);
			printf("tornado_spin (hover): spinning from %.2f s, hovered to %.2f s, drift %.0f (kp %.1f g %.1f aim %d), boost left %.0f\n", (h.touch + ha) / 120.0, h.done[0] / 120.0, bestDrift, hk, hg, ha, h.frames.empty() ? 0.0f : h.frames.back().boostAmount);
			for (size_t i = 0; i < h.frames.size(); i++) h.frames[i].tick = static_cast<int>(i);
			std::ofstream(out + "/tornado_spin_model.csv") << Csv(h.frames);
			Out v = Hover(arena, hk, hg, 24, ha, 700.0f, 360, true, false);
			for (size_t i = 0; i < v.frames.size(); i++) v.frames[i].tick = static_cast<int>(i);
			std::ofstream(out + "/var_tornado_nospin.csv") << Csv(v.frames);
		}
		Out o = Tornado(arena, bk, bg, bj, bl, true, ba, bp);
		printf("tornado_spin: touch at %.3f s, spinning from %.3f s, roll held %.0f%% (kp %.1f g %.1f jump %d lead %.1f aim %d roll %+.0f)\n", o.touch / 120.0, (bj + ba) / 120.0, o.rollShare * 100.0f, bk, bg, bj, bl, ba, bp);
		for (size_t i = 0; i < o.frames.size(); i++) o.frames[i].tick = static_cast<int>(i);
		std::ofstream(out + "/tornado_ball_old.csv") << Csv(o.frames);   // the earlier spin-into-the-ball model, kept for reference
		// a test variant: the old one, air roll with the nose held on the ball (a barrel roll, not a tornado)
		Out v = TornadoOld(arena, 8.0f, 3.0f, 24, 0.0f, true);
		for (size_t i = 0; i < v.frames.size(); i++) v.frames[i].tick = static_cast<int>(i);
		std::ofstream(out + "/var_tornado_barrelroll.csv") << Csv(v.frames);
	}
	return 0;
}
