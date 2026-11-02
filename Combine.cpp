// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <format>
#include <cctype>
#include <cstdio>

using namespace RocketSim;

namespace
{
	constexpr float kGravity = 650.0f;
	constexpr float kMaxSpeed = 2300.0f;        // a car never goes faster than this
	constexpr float kBallRadius = 92.75f;
	constexpr float kCarReach = 90.0f;          // centre of an Octane to the far corner of its hitbox, plus a little
	constexpr float kContact = kBallRadius + kCarReach;
	constexpr float kGoalLine = 5120.0f + kBallRadius;   // the whole ball over the line
	// Generous pushes: boost plus full throttle from a standstill on a surface; boost plus air throttle in the air
	constexpr float kSurfacePush = 1600.0f + 991.67f;
	constexpr float kAirPush = 991.67f + 66.67f;
	// Generous instant kicks: the dodge on a surface; a first jump, its hold and a second jump in the air
	constexpr float kSurfaceKick = 500.0f;
	constexpr float kAirKick = 875.0f;

	float Len(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

	// Highest a car can get its centre by time t from the floor: jump (and hold), second jump, boost straight up
	float JumpHeight(float t)
	{
		float z = 17.0f, vz = 291.67f;
		const float dt = 1.0f / 120.0f;
		for (float s = 0; s < t; s += dt)
		{
			float az = (s < 0.2f ? 1458.33f : 0.0f) + 991.67f - kGravity;
			if (s >= 0.2f && s < 0.2f + dt) vz += 291.67f;   // the second jump, right after the hold
			vz = std::min(vz + az * dt, kMaxSpeed);
			z += vz * dt;
		}
		return z;
	}
}

bool Combine::CanReach(const CarStart& car, const Vec3& q, float t)
{
	// Never faster than the speed cap
	const float straight = Len(q.x - car.pos.x, q.y - car.pos.y, q.z - car.pos.z) - kContact;
	if (straight > kMaxSpeed * t) return false;

	// In the air: drifts with its velocity and falls, and can push and kick any way it likes
	{
		const float dx = q.x - (car.pos.x + car.vel.x * t);
		const float dy = q.y - (car.pos.y + car.vel.y * t);
		const float dz = q.z - (car.pos.z + car.vel.z * t - 0.5f * kGravity * t * t);
		if (Len(dx, dy, dz) - kContact <= kAirKick * t + 0.5f * kAirPush * t * t) return true;
	}
	// Low in the air (a hop, a landing): it's back on the floor in a moment, and then it can drive
	if (!car.onSurface && !(car.pos.z < 300.0f)) return false;
	// On a surface: nothing pulls it off yet, and it can use throttle as well as boost. Low points are reached
	// along the floor, high ones by jumping (on a wall or ceiling the surface itself carries it).
	{
		const float dx = q.x - (car.pos.x + car.vel.x * t);
		const float dy = q.y - (car.pos.y + car.vel.y * t);
		const float dz = q.z - (car.pos.z + car.vel.z * t);
		const float reach = kSurfaceKick * t + 0.5f * kSurfacePush * t * t;
		const bool floor = car.pos.z < 300.0f;
		if (!floor) return Len(dx, dy, dz) - kContact <= reach;
		// Near a wall it can drive up the wall instead of jumping
		const bool nearWall = std::fabs(q.x) > 3400.0f || std::fabs(q.y) > 4500.0f;
		const bool highEnough = nearWall || q.z - kContact <= JumpHeight(t);
		if (nearWall && Len(dx, dy, dz) - kContact <= reach) return true;
		if (Len(dx, dy, 0) - kContact <= reach && highEnough) return true;
		// Or it keeps its speed and turns toward it (a car at speed carves or powerslides round without slowing):
		// the way there is the straight line plus a little for the turn, driven from its current speed
		if (!highEnough) return false;
		const float px = q.x - car.pos.x, py = q.y - car.pos.y;
		const float dist = std::max(0.0f, Len(px, py, 0) - kContact);
		const float s0 = Len(car.vel.x, car.vel.y, 0);
		float turn = 0;
		if (s0 > 100.0f && dist > 1.0f)
		{
			const float c = std::clamp((car.vel.x * px + car.vel.y * py) / (s0 * Len(px, py, 0)), -1.0f, 1.0f);
			turn = std::acos(c);
		}
		const float path = dist + 150.0f * turn;
		const float v1 = std::min(kMaxSpeed, s0 + kSurfaceKick);
		const float ta = (kMaxSpeed - v1) / kSurfacePush;
		const float run = t <= ta ? v1 * t + 0.5f * kSurfacePush * t * t : v1 * ta + 0.5f * kSurfacePush * ta * ta + kMaxSpeed * (t - ta);
		return run >= path;
	}
}

namespace
{
	Combine::ReachResult SaveReachIn(Arena* arena, const Combine::BallStart& b, const Combine::CarStart& car, int goalSign);
}

namespace
{
	// Distance covered in a straight line in time t from speed s0, full throttle and boost (real acceleration curve)
	float GroundRun(float s0, float t)
	{
		float v = std::clamp(s0, 0.0f, kMaxSpeed), d = 0;
		const float dt = 1.0f / 60.0f;
		for (float s = 0; s < t; s += dt)
		{
			const float thr = v < 1400.0f ? 1600.0f - 1440.0f * v / 1400.0f : v < 1410.0f ? 16.0f * (1410.0f - v) : 0.0f;
			v = std::min(kMaxSpeed, v + (thr + 991.67f) * dt);
			d += v * dt;
		}
		return d;
	}
	// Realistic fast-aerial height after rising for t (jump, hold, second jump, boost mostly upward)
	float RealRise(float t) { return 17.0f + 750.0f * t + 170.0f * t * t; }
}

bool Combine::CanReachReal(const CarStart& car, const Vec3& q, float t)
{
	if (!CanReach(car, q, t)) return false;   // never more than the perfect car
	constexpr float kTouch = kContact - 15.0f;
	// Facing: forward vector from pitch / yaw
	const float fx = std::cos(car.pitch) * std::cos(car.yaw), fy = std::cos(car.pitch) * std::sin(car.yaw), fz = std::sin(car.pitch);
	const float speed = Len(car.vel.x, car.vel.y, car.vel.z);
	// Covering a distance along the ground (or a wall) in the time left: from a given speed, with a dodge for speed
	// when there's time for one
	auto covers = [&](float s0, float tLeft, float path) {
		if (tLeft <= 0) return path <= 0;
		return GroundRun(s0 + (tLeft > 0.45f ? 450.0f : 0.0f), tLeft) >= path;
	};
	const bool floor = car.onSurface ? car.pos.z < 300.0f : car.pos.z < 300.0f;
	if (car.onSurface || floor)
	{
		const float px = q.x - car.pos.x, py = q.y - car.pos.y, pz = floor ? 0.0f : q.z - car.pos.z;
		const float d = Len(px, py, pz);
		const float dist = std::max(0.0f, d - kTouch);
		const float fl = std::max(1e-3f, Len(fx, fy, floor ? 0.0f : fz));
		const float theta = (car.hasFacing && d > 1) ? std::acos(std::clamp((fx * px + fy * py + (floor ? 0.0f : fz * pz)) / (fl * d), -1.0f, 1.0f)) : 0.0f;
		const float vAlong = d > 1 ? (car.vel.x * px + car.vel.y * py + (floor ? 0.0f : car.vel.z * pz)) / d : 0.0f;
		// The height it's at, if it's off the floor: a real aerial to get there, while driving under it
		auto highEnough = [&](float tLeft) {
			if (!floor) return true;
			const bool nearWall = std::fabs(q.x) > 3400.0f || std::fabs(q.y) > 4500.0f;
			return q.z - kTouch <= 17.0f || nearWall || RealRise(tLeft) >= q.z - kTouch;
		};
		// Turn, then go (small corrections are steered on the way, no time lost); a half flip when it's behind you
		{
			const float th = std::max(0.0f, theta - 0.35f);
			float tTurn, s0;
			if (theta > 2.3f) { tTurn = 0.75f; s0 = 500.0f; }
			else { tTurn = th / 3.0f + (vAlong < 0 ? -vAlong / 3500.0f : 0.0f); s0 = std::max(0.0f, vAlong); }
			const float tLeft = t - tTurn;
			if (covers(s0, tLeft, dist) && highEnough(tLeft)) return true;
		}
		// Or carve round at speed (a powerslide keeps most of it), a longer way but no stop
		if (speed > 500.0f)
		{
			const float path = dist + 400.0f * theta;
			if (covers(0.85f * speed, t, path) && highEnough(t)) return true;
		}
		if (car.onSurface) return false;
	}
	// In the air: point the nose at it, then boost (with a little from a dodge if there's one)
	const float rx = q.x - (car.pos.x + car.vel.x * t), ry = q.y - (car.pos.y + car.vel.y * t), rz = q.z - (car.pos.z + car.vel.z * t - 0.5f * kGravity * t * t);
	const float rl = Len(rx, ry, rz);
	float theta = 0;
	if (car.hasFacing && rl > 1) theta = std::acos(std::clamp((fx * rx + fy * ry + fz * rz) / rl, -1.0f, 1.0f));
	const float tl = t - std::max(0.0f, theta - 0.35f) / 3.5f;
	if (tl <= 0) return rl - kTouch <= 0;
	return rl - kTouch <= 0.5f * 991.67f * tl * tl + 400.0f * tl;
}

Combine::ReachResult Combine::SaveReach(const BallStart& b, const CarStart& car, int goalSign)
{
	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	return SaveReachIn(arena.get(), b, car, goalSign);
}

namespace
{
Combine::ReachResult SaveReachIn(Arena* arena, const Combine::BallStart& b, const Combine::CarStart& car, int goalSign)
{
	using namespace Combine;
	ReachResult r;
	BallState bs;
	bs.pos = Vec(b.pos.x, b.pos.y, b.pos.z);
	bs.vel = Vec(b.vel.x, b.vel.y, b.vel.z);
	bs.angVel = Vec(b.angVel.x, b.angVel.y, b.angVel.z);
	arena->ball->SetState(bs);
	const float dt = 1.0f / 120.0f;
	std::vector<Vec3> path;
	path.reserve(480);
	for (int i = 1; i <= 480; i++)
	{
		arena->Step(1);
		BallState s = arena->ball->GetState();
		path.push_back({ s.pos.x, s.pos.y, s.pos.z });
		if (goalSign * s.pos.y >= kGoalLine)
		{
			r.onTarget = std::fabs(s.pos.x) < 893.0f && s.pos.z < 643.0f;
			r.tCross = i * dt;
			break;
		}
	}
	if (r.tCross < 0) { r.tCross = 480 * dt; }   // never crossed: the whole 4 s count
	for (int i = 0; i < static_cast<int>(path.size()); i++)
	{
		const float t = (i + 1) * dt;
		if (CanReach(car, path[i], t)) { r.possible = true; r.tReach = t; r.slack = r.tCross - t; break; }
	}
	if (r.possible)
		for (int i = std::max(0, static_cast<int>(r.tReach * 120.0f) - 1); i < static_cast<int>(path.size()); i++)
		{
			const float t = (i + 1) * dt;
			if (CanReachReal(car, path[i], t)) { r.tReachReal = t; r.slackReal = r.tCross - t; break; }
		}
	return r;
}
}

// ---- Saves ----

void Combine::SaveLevelBand(int level, float& lo, float& hi)
{
	// Spare time by the realistic estimate (turning to face it, real acceleration, a dodge, real aerial rise), set from
	// 1,127 real saves in match replays: only 1.5% of them were made faster than the estimate allows, so it's fair,
	// not harsh. Pro saves (349) by it: half had 1.07 s or less, a quarter 0.57 s, 10% 0.33 s, 5% 0.25 s.
	static const float kBands[kSaveLevels + 1] = { 2.50f, 1.10f, 0.65f, 0.40f, 0.22f, 0.05f };
	level = std::clamp(level, 1, kSaveLevels);
	hi = kBands[level - 1];
	lo = kBands[level];
}

const char* Combine::SaveLevelName(int level)
{
	static const char* kNames[kSaveLevels] = { "Warm-up", "Steady", "Sharp", "Elite", "Legend" };
	return kNames[std::clamp(level, 1, kSaveLevels) - 1];
}

std::vector<Combine::SaveSituation> Combine::LoadSaveSituations(const std::string& text)
{
	std::vector<SaveSituation> out;
	size_t pos = text.find('\n');   // skip the header
	while (pos != std::string::npos && pos + 1 < text.size())
	{
		const size_t next = text.find('\n', pos + 1);
		const std::string line = text.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
		pos = next;
		if (line.size() < 10) continue;
		std::vector<float> v;
		char src = line[0];
		size_t a = line.find(',');
		while (a != std::string::npos)
		{
			const size_t b = line.find(',', a + 1);
			try { v.push_back(std::stof(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1))); } catch (...) { v.push_back(0); }
			a = b;
		}
		if (v.size() < 23) continue;
		SaveSituation s;
		s.source = src;
		s.ball = { {v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]} };
		s.car.pos = { v[9], v[10], v[11] };
		s.car.vel = { v[12], v[13], v[14] };
		s.carAngVel = { v[15], v[16], v[17] };
		s.car.pitch = v[18]; s.car.yaw = v[19]; s.car.roll = v[20]; s.car.hasFacing = true;
		s.boost = v[21];
		s.car.onSurface = v[22] > 0.5f;
		if (v.size() >= 26) { s.recvZ = v[23]; s.recvSpeed = v[24]; s.recvDt = v[25]; }
		out.push_back(s);
	}
	return out;
}

std::vector<Combine::SaveShot> Combine::MakeSaveSet(uint32_t seed, int level, int count, const std::vector<SaveSituation>* real)
{
	std::vector<SaveShot> out;
	float lo, hi;
	SaveLevelBand(level, lo, hi);
	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	std::mt19937 rng(seed * 2654435761u + static_cast<uint32_t>(level) * 97u);
	// mt19937's numbers are the same everywhere; the standard distributions aren't, so map them by hand (a code has to
	// give the same shots on every PC)
	auto U = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() >> 8) * (1.0f / 16777216.0f); };
	const float pi = 3.14159265f;
	if (real && !real->empty())
	{
		// Real saves: you start where the real defender was, with their speed, facing, spin and boost. Each one is
		// mirrored left-right half the time and nudged a little, so a set can't be memorized.
		std::vector<int> used;
		for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 600; tries++)
		{
			const int idx = static_cast<int>(rng() % real->size());
			if (std::find(used.begin(), used.end(), idx) != used.end()) continue;
			const SaveSituation& sit = (*real)[idx];
			const bool mirror = (rng() & 1) != 0;
			SaveShot s;
			s.real = true;
			s.ball = sit.ball;
			s.car = sit.car;
			s.carAngVel = sit.carAngVel;
			s.boost = sit.boost;
			// Nudges stay along the surface you're on (Oct 29): nudging a wall car in x or y lifted it off the wall (or into
			// it), so its wheels didn't grip and it slid for a second
			{
				const float cp = std::cos(sit.car.pitch), sp = std::sin(sit.car.pitch), cy = std::cos(sit.car.yaw), sy = std::sin(sit.car.yaw);
				const float cr = std::cos(sit.car.roll), sr = std::sin(sit.car.roll);
				const Vec3 up{ -cr * cy * sp - sr * sy, -cr * sy * sp + sr * cy, cp * cr };
				auto Flat = [&](Vec3 d) { const float k = d.x * up.x + d.y * up.y + d.z * up.z; return Vec3{ d.x - k * up.x, d.y - k * up.y, d.z - k * up.z }; };
				const Vec3 dp = Flat({ U(-60.0f, 60.0f), U(-60.0f, 60.0f), U(-60.0f, 60.0f) });
				const Vec3 dv = Flat({ U(-80.0f, 80.0f), U(-80.0f, 80.0f), U(-80.0f, 80.0f) });
				s.car.pos.x += dp.x; s.car.pos.y += dp.y; s.car.pos.z += dp.z;
				s.car.vel.x += dv.x; s.car.vel.y += dv.y; s.car.vel.z += dv.z;
				if (sit.car.onSurface && up.z < 0.9f)
				{
					// a wall car: no speed away from the wall, and a touch into it so the wheels grip from the first tick
					const float vn = s.car.vel.x * up.x + s.car.vel.y * up.y + s.car.vel.z * up.z;
					const float push = -40.0f - vn;
					s.car.vel.x += push * up.x; s.car.vel.y += push * up.y; s.car.vel.z += push * up.z;
				}
			}
			s.ball.vel.x += U(-40.0f, 40.0f); s.ball.vel.z += U(-30.0f, 30.0f);
			if (mirror)
			{
				s.ball.pos.x = -s.ball.pos.x; s.ball.vel.x = -s.ball.vel.x; s.ball.angVel = { s.ball.angVel.x, -s.ball.angVel.y, -s.ball.angVel.z };
				s.car.pos.x = -s.car.pos.x; s.car.vel.x = -s.car.vel.x; s.carAngVel = { s.carAngVel.x, -s.carAngVel.y, -s.carAngVel.z };
				s.car.yaw = pi - s.car.yaw; s.car.roll = -s.car.roll;
			}
			s.carYaw = s.car.yaw;
			// Only spawns that feel like the start of a save (Oct 29): you're on the floor or a wall, not mid-air or
			// mid-flip; the ball isn't already at the line; and the save needs you to do something first
			if (!sit.car.onSurface && sit.car.pos.z > 40.0f) continue;
			// ...and not already leaving it (a jump pressed just before the moment it was captured)
			if (sit.car.pos.z < 60.0f && sit.car.vel.z > 150.0f) continue;
			if (std::fabs(sit.car.pos.x) > 3900.0f && sit.car.pos.x * sit.car.vel.x < -250.0f * std::fabs(sit.car.pos.x)) continue;
			if (std::fabs(sit.car.pos.y) > 4950.0f && sit.car.pos.y * sit.car.vel.y < -250.0f * std::fabs(sit.car.pos.y)) continue;
			// Elite and Legend: no slow rollers
			if (level >= 4 && Len(sit.ball.vel.x, sit.ball.vel.y, sit.ball.vel.z) < 1000.0f) continue;
			if (Len(sit.carAngVel.x, sit.carAngVel.y, sit.carAngVel.z) > 3.5f) continue;
			const ReachResult r = SaveReachIn(arena.get(), s.ball, s.car, -1);
			if (!r.onTarget || !r.possible || r.slackReal < lo || r.slackReal >= hi) continue;
			if (r.tCross < 0.85f || r.tReachReal < 0.35f) continue;
			s.slack = r.slack;
			s.slackReal = r.slackReal;
			s.tCross = r.tCross;
			used.push_back(idx);
			out.push_back(s);
		}
		if (static_cast<int>(out.size()) >= count) return out;
	}
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 400; tries++)
	{
		SaveShot s;
		// Where it's hit from: anywhere in front of your goal, from the edge of the box to past midfield
		const float dist = U(1800.0f, 7000.0f);
		const float ang = U(-1.15f, 1.15f);              // 0 = straight down the middle
		s.ball.pos = { dist * std::sin(ang), -5120.0f + dist * std::cos(ang), U(93.0f, 1300.0f) };
		if (std::fabs(s.ball.pos.x) > 3900.0f || s.ball.pos.y > 4500.0f) continue;
		// Where it's going: somewhere in the goal mouth, at the pace of a real shot
		const Vec3 aim{ U(-820.0f, 820.0f), -5120.0f - 150.0f, U(100.0f, 580.0f) };
		const float speed = U(1300.0f, 3500.0f);
		const float dx = aim.x - s.ball.pos.x, dy = aim.y - s.ball.pos.y, dz = aim.z - s.ball.pos.z;
		const float T = std::max(0.35f, std::sqrt(dx * dx + dy * dy) / speed);
		s.ball.vel = { dx / T, dy / T, (dz + 0.5f * kGravity * T * T) / T };
		if (Len(s.ball.vel.x, s.ball.vel.y, s.ball.vel.z) > 6000.0f) continue;   // the ball's speed cap
		s.ball.angVel = { U(-3.0f, 3.0f), U(-3.0f, 3.0f), U(-3.0f, 3.0f) };
		// Where you are: around your box, sometimes out of position, facing any way, nearly still
		s.car.pos = { U(-2600.0f, 2600.0f), U(-5100.0f, -2600.0f), 17.0f };
		s.carYaw = U(-pi, pi);
		s.car.yaw = s.carYaw; s.car.hasFacing = true;
		const float v0 = U(0.0f, 400.0f);
		s.car.vel = { v0 * std::cos(s.carYaw), v0 * std::sin(s.carYaw), 0.0f };
		s.car.onSurface = true;
		const ReachResult r = SaveReachIn(arena.get(), s.ball, s.car, -1);
		if (!r.onTarget || !r.possible || r.slackReal < lo || r.slackReal >= hi) continue;
		s.slack = r.slack;
		s.slackReal = r.slackReal;
		s.tCross = r.tCross;
		out.push_back(s);
	}
	return out;
}

std::string Combine::SetCode(const char* event, int level, uint32_t seed)
{
	return std::format("{}{}-{:08X}", event, level, seed);
}

bool Combine::ParseSetCode(const std::string& code, std::string& event, int& level, uint32_t& seed)
{
	const size_t dash = code.find('-');
	if (dash == std::string::npos || dash < 2) return false;
	event.clear();
	size_t i = 0;
	while (i < dash && std::isalpha(static_cast<unsigned char>(code[i]))) event += static_cast<char>(std::toupper(static_cast<unsigned char>(code[i++])));
	if (event.empty() || i >= dash) return false;
	try { level = std::stoi(code.substr(i, dash - i)); seed = static_cast<uint32_t>(std::stoul(code.substr(dash + 1), nullptr, 16)); }
	catch (...) { return false; }
	return true;
}

// ---- Recovery ----

namespace
{
	// Rotation matrix columns (forward, right, up) from pitch / yaw / roll, as RocketSim builds them
	void Axes(float pitch, float yaw, float roll, Vec& fwd, Vec& right, Vec& up)
	{
		RotMat m = Angle(yaw, pitch, roll).ToRotMat();
		fwd = m.forward; right = m.right; up = m.up;
	}
}

namespace
{
	Combine::RecoveryCheck CheckRecoveryIn(Arena* arena, Car* car, const Combine::Vec3& pos, const Combine::Vec3& vel, const Combine::Vec3& angVel, float pitch, float yaw, float roll);
}

Combine::RecoveryCheck Combine::CheckRecovery(const Vec3& pos, const Vec3& vel, const Vec3& angVel, float pitch, float yaw, float roll)
{
	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
	return CheckRecoveryIn(arena.get(), car, pos, vel, angVel, pitch, yaw, roll);
}

namespace
{
Combine::RecoveryCheck CheckRecoveryIn(Arena* arena, Car* car, const Combine::Vec3& pos, const Combine::Vec3& vel, const Combine::Vec3& angVel, float pitch, float yaw, float roll)
{
	using namespace Combine;
	RecoveryCheck rc;
	// When it first touches something if you do nothing (RocketSim, real hitbox and arena)
	CarState cs;
	cs.pos = Vec(pos.x, pos.y, pos.z);
	cs.vel = Vec(vel.x, vel.y, vel.z);
	cs.angVel = Vec(angVel.x, angVel.y, angVel.z);
	cs.rotMat = Angle(yaw, pitch, roll).ToRotMat();
	cs.isOnGround = false;
	cs.hasJumped = true;
	car->SetState(cs);
	BallState bs;
	bs.pos = Vec(3500, 4800, 93);
	arena->ball->SetState(bs);
	for (int i = 1; i <= 600; i++)
	{
		arena->Step(1);
		CarState s = car->GetState();
		if (s.worldContact.hasContact || s.isOnGround)
		{
			rc.tHit = i / 120.0f;
			Vec n = s.worldContact.hasContact ? s.worldContact.contactNormal : Vec(0, 0, 1);
			rc.normal = { n.x, n.y, n.z };
			rc.land = { s.pos.x, s.pos.y, s.pos.z };
			rc.landVel = { s.vel.x, s.vel.y, s.vel.z };
			break;
		}
	}
	if (rc.tHit < 0) return rc;
	// How far the wheels have to turn: from the car's up now to the surface's normal, less the bit the game forgives
	Vec f, r, u;
	Axes(pitch, yaw, roll, f, r, u);
	const float dot = std::clamp(u.x * rc.normal.x + u.y * rc.normal.y + u.z * rc.normal.z, -1.0f, 1.0f);
	const float theta = std::max(0.0f, std::acos(dot) - 0.45f);
	// Bang-bang turn: speed up, then slow down, capped at 5.5 rad/s
	auto turnTime = [](float th, float alpha) {
		const float wMax = 5.5f;
		if (th <= 0) return 0.0f;
		if (std::sqrt(th * alpha) <= wMax) return 2.0f * std::sqrt(th / alpha);
		return th / wMax + wMax / alpha;
	};
	rc.tTurn = turnTime(theta, 38.34f);   // generous: any axis as fast as roll, starting spin ignored
	// Closer to real: the turn's axis in the car's own frame, each axis's own rate, and the spin it starts with
	{
		Vec ax = Vec(u.y * rc.normal.z - u.z * rc.normal.y, u.z * rc.normal.x - u.x * rc.normal.z, u.x * rc.normal.y - u.y * rc.normal.x);
		const float al = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);
		float alpha = 38.34f;
		float spinAgainst = 0;
		if (al > 1e-3f)
		{
			ax = Vec(ax.x / al, ax.y / al, ax.z / al);
			const float cr = ax.x * f.x + ax.y * f.y + ax.z * f.z;    // roll part
			const float cp = ax.x * r.x + ax.y * r.y + ax.z * r.z;    // pitch part
			const float cy = ax.x * u.x + ax.y * u.y + ax.z * u.z;    // yaw part
			alpha = 1.0f / std::sqrt((cr / 38.34f) * (cr / 38.34f) + (cp / 12.46f) * (cp / 12.46f) + (cy / 9.11f) * (cy / 9.11f));
			// Spin the wrong way has to be stopped first; spin the right way helps
			spinAgainst = -(angVel.x * ax.x + angVel.y * ax.y + angVel.z * ax.z);
		}
		float t = turnTime(theta, alpha);
		if (spinAgainst > 0) t += spinAgainst / alpha + spinAgainst * spinAgainst / (2.0f * alpha) / 5.5f;
		else t = std::max(0.0f, t - std::min(t * 0.5f, -spinAgainst / alpha));
		rc.tTurnReal = t;
	}
	// You keep your flip when thrown, and a flip turns you over faster than air roll: no throw needs more than about
	// 0.3 s of turning, so only one that slams you into a surface sooner than that is thrown out
	rc.possible = rc.tHit >= std::min(rc.tTurn, 0.30f);
	return rc;
}
}

void Combine::RecoveryLevelBand(int level, float& lo, float& hi)
{
	static const float kBands[kSaveLevels + 1] = { 3.0f, 1.0f, 0.65f, 0.40f, 0.20f, 0.02f };
	level = std::clamp(level, 1, kSaveLevels);
	hi = kBands[level - 1];
	lo = kBands[level];
}

std::vector<Combine::RecoveryThrow> Combine::MakeRecoverySet(uint32_t seed, int level, int count)
{
	std::vector<RecoveryThrow> out;
	float lo, hi;
	RecoveryLevelBand(level, lo, hi);
	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	Car* car = arena->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
	std::mt19937 rng(seed * 2246822519u + static_cast<uint32_t>(level) * 131u + 7u);
	auto U = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() >> 8) * (1.0f / 16777216.0f); };
	const float pi = 3.14159265f;
	const float lv = static_cast<float>(std::clamp(level, 1, kSaveLevels));
	int stranded = 0;   // at most one "stranded high" throw a set: an occasional change of pace
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 400; tries++)
	{
		RecoveryThrow t;
		// Moments you'd really be in, anywhere on the field (Oct 29): coming out of an aerial, knocked about in a 50/50,
		// bailing off a wall, or just thrown. Higher levels spin harder and travel faster.
		const float kind = U(0.0f, 1.0f);
		const float spinLo = 0.4f + 0.7f * (lv - 1.0f), spinHi = std::min(5.5f, 2.6f + 0.75f * lv);
		const float w = U(spinLo, spinHi);
		bool high = false;
		if (kind < 0.30f)
		{
			// Out of an aerial: high, travelling up field (or across), nose up
			t.pos = { U(-3400.0f, 3400.0f), U(-2500.0f, 4600.0f), U(700.0f, 1750.0f) };
			const float a = U(0.4f, 2.7f), sp = U(900.0f, 1500.0f + 180.0f * lv), e = U(-0.3f, 0.6f);
			t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			t.pitch = U(0.2f, pi * 0.5f); t.yaw = std::atan2(t.vel.y, t.vel.x) + U(-0.8f, 0.8f) * lv * 0.5f; t.roll = U(-pi, pi) * (0.2f + 0.16f * lv);
		}
		else if (kind < 0.50f)
		{
			// Knocked about in a 50/50 or a bump: low, fast, spinning
			t.pos = { U(-3600.0f, 3600.0f), U(-4200.0f, 4600.0f), U(180.0f, 650.0f) };
			const float a = U(-pi, pi), sp = U(700.0f, 1300.0f + 180.0f * lv), e = U(0.0f, 0.7f);
			t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			t.pitch = U(-pi * 0.5f, pi * 0.5f); t.yaw = U(-pi, pi); t.roll = U(-pi, pi);
		}
		else if (kind < 0.65f)
		{
			// Bailing off a side wall: near it, high, moving off it
			const float side = U(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
			t.pos = { side * U(3300.0f, 3800.0f), U(-4000.0f, 4500.0f), U(500.0f, 1800.0f) };
			const float sp = U(500.0f, 900.0f + 200.0f * lv);
			t.vel = { -side * U(200.0f, 700.0f), U(-1.0f, 1.0f) * sp, U(-300.0f, 400.0f) };
			t.pitch = U(-0.8f, 0.8f); t.yaw = U(-pi, pi); t.roll = side * U(0.6f, 1.6f) + U(-0.5f, 0.5f) * lv * 0.4f;
		}
		else if (kind < 0.82f)
		{
			// Sent flying at a wall or your own goal (a bump, a missed challenge): you meet the wall or the back wall first
			const float pick = U(0.0f, 1.0f);
			const float sp = U(900.0f, 1300.0f + 150.0f * lv), e = U(-0.1f, 0.5f);
			if (pick < 0.55f)
			{
				const float side = U(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
				t.pos = { side * U(2000.0f, 3200.0f), U(-4000.0f, 4000.0f), U(250.0f, 1200.0f) };
				const float a = (side > 0 ? 0.0f : pi) + U(-0.6f, 0.6f);
				t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			}
			else
			{
				t.pos = { U(-2500.0f, 2500.0f), U(-4200.0f, -3000.0f), U(250.0f, 1100.0f) };
				const float a = -pi * 0.5f + U(-0.6f, 0.6f);
				t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			}
			t.pitch = U(-pi * 0.5f, pi * 0.5f); t.yaw = U(-pi, pi); t.roll = U(-pi, pi);
		}
		else if (kind < 0.90f && stranded == 0)
		{
			// Stranded high up (an aerial gone wrong): plenty of time to land, but the race is getting down fast
			high = true;
			t.pos = { U(-3000.0f, 3000.0f), U(-3500.0f, 4000.0f), U(1450.0f, 1500.0f + 90.0f * lv) };
			const float a = U(-pi, pi), sp = U(0.0f, 600.0f), e = U(-0.2f, 0.8f);
			t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			t.pitch = U(0.1f, pi * 0.5f); t.yaw = U(-pi, pi); t.roll = U(-pi, pi);
		}
		else
		{
			t.pos = { U(-3600.0f, 3600.0f), U(-4600.0f, 4600.0f), U(250.0f, 1700.0f) };
			const float sp = U(300.0f, 2200.0f);
			const float a = U(-pi, pi), e = U(-0.9f, 0.7f);
			t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			t.pitch = U(-pi * 0.5f, pi * 0.5f); t.yaw = U(-pi, pi); t.roll = U(-pi, pi);
		}
		const float wa = U(-pi, pi), we = U(-1.5f, 1.5f);
		t.angVel = { w * std::cos(we) * std::cos(wa), w * std::cos(we) * std::sin(wa), w * std::sin(we) };
		const RecoveryCheck rc = CheckRecoveryIn(arena.get(), car, t.pos, t.vel, t.angVel, t.pitch, t.yaw, t.roll);
		if (!rc.possible || rc.tHit < 0.25f) continue;
		if (rc.normal.z < -0.7f) continue;   // ceiling landings: not this event
		t.tHit = rc.tHit; t.normal = rc.normal; t.tTurn = rc.tTurn; t.tTurnReal = rc.tTurnReal;
		t.margin = rc.tHit - rc.tTurnReal;
		if (t.margin < lo || (!high && t.margin >= hi)) continue;   // stranded throws have plenty of turn time by design
		if (high && rc.normal.z < 0.7f) continue;
		t.land = rc.land; t.landVel = rc.landVel;
		// The target: your back post (the far one from where you land), at most 3000 away
		{
			const Vec3 bp{ rc.land.x >= 0 ? -700.0f : 700.0f, -4900.0f, 17.0f };
			const float dx = bp.x - rc.land.x, dy = bp.y - rc.land.y, d = Len(dx, dy, 0);
			const float cap = 2000.0f + 625.0f * (lv - 1.0f);   // 2000 at Warm-up up to 4500 at Legend
			const float k = d > cap ? cap / d : 1.0f;
			t.target = { std::clamp(rc.land.x + dx * k, -3700.0f, 3700.0f), std::clamp(rc.land.y + dy * k, -4950.0f, 4950.0f), 17.0f };
			if (Len(t.target.x - rc.land.x, t.target.y - rc.land.y, 0) < 1000.0f + 200.0f * lv) continue;   // too close to be a race
		}
		// Par: land well (keeping the speed along the surface), then drive there
		{
			const float px = t.target.x - rc.land.x, py = t.target.y - rc.land.y, pz = t.target.z - rc.land.z;
			const float d = Len(px, py, pz);
			const Vec3 v = rc.landVel;
			// the part of the landing speed along the surface
			const float vn = v.x * rc.normal.x + v.y * rc.normal.y + v.z * rc.normal.z;
			const Vec3 vs{ v.x - vn * rc.normal.x, v.y - vn * rc.normal.y, v.z - vn * rc.normal.z };
			const float sp = Len(vs.x, vs.y, vs.z);
			const float along = d > 1 ? (vs.x * px + vs.y * py + vs.z * pz) / d : 0.0f;
			const float theta = sp > 1 && d > 1 ? std::acos(std::clamp(along / sp, -1.0f, 1.0f)) : 0.0f;
			float tLand = rc.tHit;
			if (high)
			{
				// A good player doesn't wait to fall: nose down, boost to the floor, wheels down just before it
				const float fz = std::sin(t.pitch);   // the nose's height part (Unreal pitch)
				const float tAim = std::acos(std::clamp(-fz, -1.0f, 1.0f)) / 3.5f + 0.1f;
				float z = t.pos.z, vz = t.vel.z, tt = 0;
				for (; z > 17.0f && tt < 6.0f; tt += 1.0f / 120.0f)
				{
					const bool boost = tt > tAim && (z - 17.0f) > -vz * 0.3f + 0.5f * 650.0f * 0.09f;
					vz = std::max(-2300.0f, vz - (650.0f + (boost ? 991.67f : 0.0f)) / 120.0f);
					z += vz / 120.0f;
				}
				tLand = std::min(rc.tHit, tt);
			}
			t.par = tLand + RealDriveTime(sp, along, theta, std::max(0.0f, d - kRecoveryTargetRadius));
		}
		if (high) stranded++;
		out.push_back(t);
	}
	return out;
}

float Combine::RealDriveTime(float speed, float vAlong, float theta, float path)
{
	if (path <= 0) return 0;
	// The same two ways as the realistic save estimate: turn (or half flip) then go, or carve round at speed
	float best = 99.0f;
	{
		const float th = std::max(0.0f, theta - 0.35f);
		float tTurn, s0;
		if (theta > 2.3f) { tTurn = 0.75f; s0 = 500.0f; }
		else { tTurn = th / 3.0f + (vAlong < 0 ? -vAlong / 3500.0f : 0.0f); s0 = std::max(0.0f, vAlong); }
		for (float t = 1.0f / 60.0f; t < 12.0f; t += 1.0f / 60.0f)
			if (GroundRun(s0 + (t > 0.45f ? 450.0f : 0.0f), t) >= path) { best = std::min(best, tTurn + t); break; }
	}
	if (speed > 500.0f)
	{
		const float p = path + 400.0f * theta;
		for (float t = 1.0f / 60.0f; t < 12.0f; t += 1.0f / 60.0f)
			if (GroundRun(0.85f * speed + (t > 0.45f ? 450.0f : 0.0f), t) >= p) { best = std::min(best, t); break; }
	}
	return best;
}

void Combine::RecoveryPointCuts(int level, float& three, float& two, float& one)
{
	static const float kCuts[kSaveLevels][3] = {
		{ 1.25f, 1.50f, 1.85f },   // Warm-up
		{ 1.18f, 1.40f, 1.70f },   // Steady
		{ 1.12f, 1.30f, 1.55f },   // Sharp
		{ 1.07f, 1.20f, 1.40f },   // Elite
		{ 1.03f, 1.12f, 1.28f },   // Legend
	};
	const int i = std::clamp(level, 1, kSaveLevels) - 1;
	three = kCuts[i][0]; two = kCuts[i][1]; one = kCuts[i][2];
}

// ---- Reads ----

float Combine::ReadMinHeight(int level)
{
	// the ball's centre: the floor, then a low touch, a jump, an aerial, crossbar height
	static const float kH[kSaveLevels] = { 0.0f, 180.0f, 300.0f, 450.0f, 640.0f };
	return kH[std::clamp(level, 1, kSaveLevels) - 1];
}

void Combine::ReadLevelBand(int level, float& lo, float& hi)
{
	// Spare time inside the read window (off the wall to landing) by the realistic estimate
	static const float kBands[kSaveLevels + 1] = { 3.0f, 1.0f, 0.6f, 0.35f, 0.18f, 0.03f };
	level = std::clamp(level, 1, kSaveLevels);
	hi = kBands[level - 1];
	lo = kBands[level];
}

std::vector<Combine::ReadSetup> Combine::MakeReadSet(uint32_t seed, int level, int count)
{
	std::vector<ReadSetup> out;
	const int lvi = std::clamp(level, 1, kSaveLevels);
	const float lv = static_cast<float>(lvi);
	float lo, hi;
	ReadLevelBand(level, lo, hi);
	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	std::mt19937 rng(seed * 2246822519u + static_cast<uint32_t>(level) * 3571u + 17u);
	auto U = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() >> 8) * (1.0f / 16777216.0f); };
	const float pi = 3.14159265f;
	const float dt = 1.0f / 120.0f;
	int perKind[3] = { 0, 0, 0 };
	const int maxPerKind[3] = { (count * 4 + 9) / 10, (count * 3 + 9) / 10, (count * 3 + 9) / 10 };
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 500; tries++)
	{
		ReadSetup r;
		const float k = U(0.0f, 1.0f);
		r.kind = k < 0.4f ? 0 : k < 0.7f ? 1 : 2;
		if (perKind[r.kind] >= maxPerKind[r.kind]) continue;
		// Where it's hit from, and the point on the wall or backboard it's hit at
		Vec3 from, at;
		// (Oct 30: higher and harder at higher levels; Coop found Legend too tame, nothing above net height)
		if (r.kind == 0)
		{
			from = { U(-2800.0f, 2800.0f), U(-2000.0f, 1500.0f), U(93.0f, 400.0f + 100.0f * lv) };
			at = { U(-1600.0f, 1600.0f), -5120.0f, std::min(1900.0f, U(750.0f + 100.0f * lv, 1300.0f + 140.0f * lv)) };
		}
		else if (r.kind == 1)
		{
			const float side = U(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
			from = { side * U(-1500.0f, 2500.0f), U(-800.0f, 2500.0f), U(93.0f, 300.0f + 100.0f * lv) };
			at = { side * 4096.0f, U(1800.0f, 4500.0f), std::min(1800.0f, U(300.0f + 150.0f * lv, 900.0f + 200.0f * lv)) };
		}
		else
		{
			from = { U(-2800.0f, 2800.0f), U(0.0f, 2800.0f), U(93.0f, 400.0f + 100.0f * lv) };
			at = { U(-1600.0f, 1600.0f), 5120.0f, std::min(1900.0f, U(750.0f + 100.0f * lv, 1300.0f + 140.0f * lv)) };
		}
		const float speed = U(1300.0f + 200.0f * lv, 1900.0f + 250.0f * lv);
		const float dx = at.x - from.x, dy = at.y - from.y, dz = at.z - from.z;
		const float T = Len(dx, dy, dz) / speed;
		r.ball.pos = from;
		r.ball.vel = { dx / T, dy / T, dz / T + 0.5f * kGravity * T };
		if (Len(r.ball.vel.x, r.ball.vel.y, r.ball.vel.z) > 3500.0f) continue;
		// spin off the shot (more at higher levels): it changes how it comes off the wall
		{
			const float w = U(0.0f, 1.2f * lv), a = U(-pi, pi), e = U(-1.2f, 1.2f);
			r.ball.angVel = { w * std::cos(e) * std::cos(a), w * std::cos(e) * std::sin(a), w * std::sin(e) };
		}
		// Fly it: when it comes off the wall, and when it lands after that
		BallState bs;
		bs.pos = Vec(from.x, from.y, from.z);
		bs.vel = Vec(r.ball.vel.x, r.ball.vel.y, r.ball.vel.z);
		bs.angVel = Vec(r.ball.angVel.x, r.ball.angVel.y, r.ball.angVel.z);
		arena->ball->SetState(bs);
		std::vector<Vec3> path;
		std::vector<BallState> states;
		Vec prevV = bs.vel;
		int iWall = -1, iFloor = -1;
		bool bad = false;
		for (int i = 1; i <= 720 && iFloor < 0; i++)
		{
			arena->Step(1);
			const BallState s = arena->ball->GetState();
			path.push_back({ s.pos.x, s.pos.y, s.pos.z });
			states.push_back(s);
			if (std::fabs(s.pos.y) >= kGoalLine && std::fabs(s.pos.x) < 893.0f && s.pos.z < 643.0f) { bad = iWall < 0 || r.kind != 0; break; }   // straight in
			const Vec dv = s.vel - prevV + Vec(0, 0, kGravity * dt);
			if (iWall < 0)
			{
				if (dv.Length() > 300.0f && s.pos.z > 150.0f) iWall = i;
				else if (s.pos.z < 120.0f && prevV.z < -50.0f && s.vel.z > 0) { bad = true; break; }   // landed before the wall
			}
			else if (s.pos.z < 120.0f && prevV.z < -50.0f && s.vel.z > 0) iFloor = i;
			prevV = s.vel;
		}
		if (bad || iWall < 0 || iFloor < 0) continue;
		// The read window ends when it drops below the level's height (the floor at Warm-up, crossbar height at Legend):
		// at higher levels you have to meet it in the air
		{
			const float hMin = ReadMinHeight(level);
			for (int i = iWall + 1; i < iFloor; i++)
				if (path[i - 1].z < hMin && states[i - 1].vel.z < 0) { iFloor = i; break; }
		}
		// Higher levels start later in the ball's flight: from the shot at Warm-up, just before it hits the wall at Legend
		{
			static const float kLeadLo[kSaveLevels] = { 9.0f, 0.8f, 0.5f, 0.3f, 0.15f };
			static const float kLeadHi[kSaveLevels] = { 9.0f, 1.3f, 0.9f, 0.6f, 0.4f };
			const float lead = U(kLeadLo[lvi - 1], kLeadHi[lvi - 1]);
			const int skip = std::max(0, iWall - static_cast<int>(lead * 120.0f));
			if (skip > 0)
			{
				const BallState& s0 = states[skip - 1];
				r.ball.pos = { s0.pos.x, s0.pos.y, s0.pos.z };
				r.ball.vel = { s0.vel.x, s0.vel.y, s0.vel.z };
				r.ball.angVel = { s0.angVel.x, s0.angVel.y, s0.angVel.z };
				path.erase(path.begin(), path.begin() + skip);
				iWall -= skip;
				iFloor -= skip;
			}
		}
		r.tWall = iWall * dt;
		r.tFloor = iFloor * dt;
		if (r.tFloor - r.tWall < 0.35f) continue;
		// Where you start: back defending your backboard, or up in their half for the others
		// a way from where it comes down (further at higher levels): up the field from it when defending, back
		// toward the middle when attacking
		CarStart car;
		const Vec3& landAt = path[iFloor - 1];
		{
			const float base = r.kind == 0 ? pi * 0.5f : -pi * 0.5f;
			const float a = base + U(-1.2f, 1.2f);
			const float D = U(700.0f + 250.0f * lv, 1400.0f + 450.0f * lv);
			car.pos = { landAt.x + std::cos(a) * D, landAt.y + std::sin(a) * D, 17.0f };
			if (std::fabs(car.pos.x) > 3700.0f || std::fabs(car.pos.y) > 4700.0f) continue;
		}
		// facing roughly where it'll come down (less so at higher levels)
		car.yaw = std::atan2(landAt.y - car.pos.y, landAt.x - car.pos.x) + U(-1.0f, 1.0f) * (0.25f + 0.3f * (lv - 1.0f));
		const float sp = U(0.0f, 1100.0f);
		car.vel = { std::cos(car.yaw) * sp, std::sin(car.yaw) * sp, 0 };
		car.onSurface = true;
		car.hasFacing = true;
		r.car = car;
		r.boost = std::floor(U(30.0f, 100.0f));
		// The read window: off the wall to landing. Can a good player get a touch in it, and with how much to spare?
		bool possible = false;
		for (int i = iWall; i < iFloor; i++)
			if (CanReach(car, path[i - 1], i * dt)) { possible = true; break; }
		if (!possible) continue;
		// spare time: the most a good player could be early to any point of the ball's path in the window (they get
		// there, then wait for it)
		r.tReachReal = -1;
		r.slackReal = -9;
		for (int i = iWall; i < iFloor; i += 3)
		{
			const float ti = i * dt;
			if (!CanReachReal(car, path[i - 1], ti)) continue;
			float a = 0, b = ti;   // the earliest it could be there (binary search)
			for (int it = 0; it < 9; it++) { const float m = 0.5f * (a + b); if (CanReachReal(car, path[i - 1], m)) b = m; else a = m; }
			if (ti - b > r.slackReal) { r.slackReal = ti - b; r.tReachReal = ti; }
		}
		if (r.tReachReal < 0) continue;
		if (r.slackReal < lo || r.slackReal >= hi) continue;
		// mirrored left-right half the time
		if (U(0.0f, 1.0f) < 0.5f)
		{
			r.ball.pos.x = -r.ball.pos.x; r.ball.vel.x = -r.ball.vel.x;
			r.car.pos.x = -r.car.pos.x; r.car.vel.x = -r.car.vel.x; r.car.yaw = pi - r.car.yaw;
		}
		perKind[r.kind]++;
		out.push_back(r);
	}
	return out;
}

// ---- First touches ----

bool Combine::TouchLevelFits(int level, float z, float s)
{
	switch (std::clamp(level, 1, kSaveLevels))
	{
	case 1: return z < 180.0f && s < 1500.0f;                // on the ground
	case 2: return z >= 180.0f && z < 450.0f && s < 2000.0f; // bouncing, a low touch
	case 3: return z >= 300.0f && z < 750.0f;                // lofted
	case 4: return z >= 600.0f && z < 1200.0f;               // aerial
	default: return z >= 950.0f || s >= 2700.0f;             // high, or fast
	}
}

const char* Combine::TouchLevelNote(int level)
{
	static const char* kNote[kSaveLevels] = { "passes on the ground", "bouncing passes", "lofted passes", "aerial passes", "high or fast passes" };
	return kNote[std::clamp(level, 1, kSaveLevels) - 1];
}

std::vector<Combine::TouchShot> Combine::MakeTouchSet(uint32_t seed, int level, int count, const std::vector<SaveSituation>& passes)
{
	std::vector<TouchShot> out;
	std::vector<int> pool;
	for (int i = 0; i < static_cast<int>(passes.size()); i++)
		if (TouchLevelFits(level, passes[i].recvZ, passes[i].recvSpeed)) pool.push_back(i);
	if (pool.empty()) return out;
	std::mt19937 rng(seed * 2654435761u + static_cast<uint32_t>(level) * 389u + 5u);
	auto U = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() >> 8) * (1.0f / 16777216.0f); };
	const float pi = 3.14159265f;
	std::vector<int> used;
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 50; tries++)
	{
		const int idx = pool[rng() % pool.size()];
		if (std::find(used.begin(), used.end(), idx) != used.end() && static_cast<int>(pool.size()) > count) continue;
		used.push_back(idx);
		const SaveSituation& p = passes[idx];
		TouchShot t;
		t.ball = p.ball; t.car = p.car; t.carAngVel = p.carAngVel; t.boost = p.boost;
		t.recvZ = p.recvZ; t.recvSpeed = p.recvSpeed; t.recvDt = p.recvDt; t.source = p.source;
		// a small nudge along the surface you're on, so a set can't be memorized
		{
			const RotMat m = Angle(p.car.yaw, p.car.pitch, p.car.roll).ToRotMat();
			const Vec u = m.up;
			Vec d(U(-40.0f, 40.0f), U(-40.0f, 40.0f), U(-40.0f, 40.0f));
			d = d - u * d.Dot(u);
			t.car.pos.x += d.x; t.car.pos.y += d.y; t.car.pos.z += d.z;
			if (p.car.onSurface && u.z < 0.9f)
			{
				// on a wall: no speed away from it, a touch into it, so the wheels grip from the first tick
				const float vn = t.car.vel.x * u.x + t.car.vel.y * u.y + t.car.vel.z * u.z;
				const float push = -40.0f - vn;
				t.car.vel.x += push * u.x; t.car.vel.y += push * u.y; t.car.vel.z += push * u.z;
			}
		}
		if (U(0.0f, 1.0f) < 0.5f)
		{
			t.ball.pos.x = -t.ball.pos.x; t.ball.vel.x = -t.ball.vel.x; t.ball.angVel = { t.ball.angVel.x, -t.ball.angVel.y, -t.ball.angVel.z };
			t.car.pos.x = -t.car.pos.x; t.car.vel.x = -t.car.vel.x; t.carAngVel = { t.carAngVel.x, -t.carAngVel.y, -t.carAngVel.z };
			t.car.yaw = pi - t.car.yaw; t.car.roll = -t.car.roll;
		}
		out.push_back(t);
	}
	return out;
}

// ---- Bot events: 50s and shadow defense ----

std::vector<Combine::Challenge> Combine::LoadChallenges(const std::string& text)
{
	std::vector<Challenge> out;
	size_t pos = 0;
	auto nums = [](const std::string& line, size_t from, std::vector<float>& v) {
		v.clear();
		size_t a = from;
		while (a != std::string::npos && a < line.size())
		{
			const size_t b = line.find(',', a);
			try { v.push_back(std::stof(line.substr(a, b == std::string::npos ? std::string::npos : b - a))); } catch (...) { v.push_back(0); }
			a = b == std::string::npos ? b : b + 1;
		}
	};
	std::vector<float> v;
	while (pos < text.size())
	{
		size_t next = text.find('\n', pos);
		if (next == std::string::npos) next = text.size();
		std::string line = text.substr(pos, next - pos);
		pos = next + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.size() < 4 || line[1] != ',') continue;
		if (line[0] == 'S')
		{
			// S,kind,label,boost,tc,ballEnd,botEnd, your car (13)
			if (line.size() < 8) continue;
			Challenge c;
			c.kind = line[2];
			c.source = line[4];
			nums(line, 6, v);
			if (v.size() < 17) continue;
			c.boost = v[0]; c.tContact = v[1]; c.ballEnd = v[2]; c.botEnd = v[3];
			c.you.pos = { v[4], v[5], v[6] }; c.you.vel = { v[7], v[8], v[9] }; c.you.angVel = { v[10], v[11], v[12] };
			c.you.q[0] = v[13]; c.you.q[1] = v[14]; c.you.q[2] = v[15]; c.you.q[3] = v[16];
			out.push_back(std::move(c));
		}
		else if (line[0] == 'B' && !out.empty())
		{
			nums(line, 2, v);
			if (v.size() < 14) continue;
			PathFrame f;
			f.t = v[0]; f.pos = { v[1], v[2], v[3] }; f.vel = { v[4], v[5], v[6] }; f.angVel = { v[7], v[8], v[9] };
			f.q[0] = v[10]; f.q[1] = v[11]; f.q[2] = v[12]; f.q[3] = v[13];
			out.back().bot.push_back(f);
		}
		else if (line[0] == 'L' && !out.empty())
		{
			nums(line, 2, v);
			if (v.size() < 10) continue;
			BallFrame f;
			f.t = v[0]; f.pos = { v[1], v[2], v[3] }; f.vel = { v[4], v[5], v[6] }; f.angVel = { v[7], v[8], v[9] };
			out.back().ball.push_back(f);
		}
	}
	// only complete ones
	out.erase(std::remove_if(out.begin(), out.end(), [](const Challenge& c) { return c.bot.size() < 10 || c.ball.size() < 5; }), out.end());
	return out;
}

int Combine::ChallengeLevel(char source)
{
	switch (source)
	{
	case '1': case '2': return 1;
	case '3': return 2;
	case '4': return 3;
	case '5': return 4;
	case '6': case 'P': return 5;
	default: return 0;
	}
}

const char* Combine::ChallengeLevelNote(int level)
{
	static const char* kNote[kSaveLevels] = { "Gold and Platinum players", "Diamond players", "Champion players", "Grand Champion players", "SSL players and pros" };
	return kNote[std::clamp(level, 1, kSaveLevels) - 1];
}


namespace
{
	// Replay frames come about 30 a second at uneven times, and a position is sometimes repeated then jumps double. So
	// rebuild each path at 120 a second: follow the recorded speed, pulled gently toward the recorded position (it
	// stays where the real player went, but moves smoothly).
	void Densify(Combine::Challenge& c)
	{
		std::vector<Combine::PathFrame> dense;
		const float dt = 1.0f / 120.0f;
		Combine::Vec3 p = c.bot.front().pos;
		for (float t = 0; t <= c.bot.back().t + 1e-4f; t += dt)
		{
			Combine::PathFrame f = Combine::BotPoseAt(c, t);   // blended from the raw frames
			if (!dense.empty())
			{
				const Combine::Vec3& v = dense.back().vel;
				p = { p.x + (v.x + f.vel.x) * 0.5f * dt, p.y + (v.y + f.vel.y) * 0.5f * dt, p.z + (v.z + f.vel.z) * 0.5f * dt };
				const float a = dt / 0.12f;
				p = { p.x + (f.pos.x - p.x) * a, p.y + (f.pos.y - p.y) * a, p.z + (f.pos.z - p.z) * a };
				f.pos = p;
			}
			dense.push_back(f);
		}
		std::vector<Combine::BallFrame> ballDense;
		Combine::Vec3 bp = c.ball.front().pos;
		for (float t = 0; t <= c.ball.back().t + 1e-4f; t += dt)
		{
			Combine::BallFrame f;
			if (!Combine::BallAt(c, t, f)) break;
			if (!ballDense.empty())
			{
				const Combine::Vec3& v = ballDense.back().vel;
				bp = { bp.x + (v.x + f.vel.x) * 0.5f * dt, bp.y + (v.y + f.vel.y) * 0.5f * dt, bp.z + (v.z + f.vel.z) * 0.5f * dt };
				const float a = dt / 0.12f;
				bp = { bp.x + (f.pos.x - bp.x) * a, bp.y + (f.pos.y - bp.y) * a, bp.z + (f.pos.z - bp.z) * a };
				f.pos = bp;
			}
			ballDense.push_back(f);
		}
		c.bot = std::move(dense);
		if (!ballDense.empty()) c.ball = std::move(ballDense);
	}
}

namespace
{
	// Kickoffs where the other taker never left their spawn (AFK; Coop found one in a set, Nov 2): 10 of the 1,100.
	// A fake kickoff still moves once the ball's been hit (690-910 uu in the data), so 300 uu only catches the AFKs.
	bool BotStayedPut(const Combine::Challenge& c)
	{
		if (c.bot.empty()) return true;
		float most = 0;
		for (const Combine::PathFrame& f : c.bot)
			most = std::max(most, std::hypot(f.pos.x - c.bot.front().pos.x, f.pos.y - c.bot.front().pos.y));
		return most < 300.0f;
	}
}

std::vector<Combine::Challenge> Combine::MakeChallengeSet(uint32_t seed, int level, int count, char kind, const std::vector<Challenge>& all)
{
	std::vector<Challenge> out;
	std::vector<int> pool;
	for (int i = 0; i < static_cast<int>(all.size()); i++)
		if (all[i].kind == kind && ChallengeLevel(all[i].source) == std::clamp(level, 1, kSaveLevels) && !(kind == 'K' && BotStayedPut(all[i])))
			pool.push_back(i);
	if (pool.empty()) return out;
	std::mt19937 rng(seed * 2654435761u + static_cast<uint32_t>(level) * 7u + (kind == 'F' ? 101u : 211u));
	std::vector<int> used;
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 40; tries++)
	{
		const int idx = pool[rng() % pool.size()];
		if (std::find(used.begin(), used.end(), idx) != used.end() && static_cast<int>(pool.size()) > count) continue;
		used.push_back(idx);
		Challenge c = all[idx];
		if (rng() & 1)
		{
			// mirrored left-right: x flips, spins flip in y and z. The rotation (w, x, y, z) becomes (z, -y, -x, w): the
			// mirror is a front-back flip plus a half turn about z. (Oct 30 fix: it was (w, x, -y, -z), which turns the
			// car to face away, so half of all 50s and shadow spawns faced backwards.)
			auto mp = [](PathFrame& f) {
				f.pos.x = -f.pos.x; f.vel.x = -f.vel.x; f.angVel = { f.angVel.x, -f.angVel.y, -f.angVel.z };
				const float w = f.q[0], x = f.q[1], y = f.q[2], z = f.q[3];
				f.q[0] = z; f.q[1] = -y; f.q[2] = -x; f.q[3] = w;
			};
			mp(c.you);
			for (PathFrame& f : c.bot) mp(f);
			for (BallFrame& f : c.ball) { f.pos.x = -f.pos.x; f.vel.x = -f.vel.x; f.angVel = { f.angVel.x, -f.angVel.y, -f.angVel.z }; }
		}
		Densify(c);
		out.push_back(std::move(c));
	}
	return out;
}

void Combine::QuatToPYR(const float q[4], float& pitch, float& yaw, float& roll)
{
	const float qw = q[0], qx = q[1], qy = q[2], qz = q[3];
	const float fx = 1 - 2 * (qy * qy + qz * qz), fy = 2 * (qx * qy + qw * qz), fz = 2 * (qx * qz - qw * qy);
	const float rz = 2 * (qy * qz + qw * qx);
	const float ux = 2 * (qx * qz + qw * qy), uy = 2 * (qy * qz - qw * qx), uz = 1 - 2 * (qx * qx + qy * qy);
	pitch = std::asin(std::clamp(fz, -1.0f, 1.0f));
	yaw = std::atan2(fy, fx);
	auto upErr = [&](float r) {
		const float sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw), sr = std::sin(r), cr = std::cos(r);
		const float ex = -(cr * sp * cy + sr * sy), ey = cy * sr - cr * sp * sy, ez = cr * cp;
		return std::fabs(ex - ux) + std::fabs(ey - uy) + std::fabs(ez - uz);
	};
	roll = std::atan2(-rz, uz);
	if (upErr(roll) > upErr(-roll)) roll = -roll;
}

Combine::PathFrame Combine::BotPoseAt(const Challenge& c, float t)
{
	if (c.bot.empty()) return PathFrame{};
	if (t <= c.bot.front().t) return c.bot.front();
	if (t >= c.bot.back().t) return c.bot.back();
	size_t i = 1;
	while (i < c.bot.size() && c.bot[i].t < t) i++;
	const PathFrame& a = c.bot[i - 1];
	const PathFrame& b = c.bot[i];
	const float k = (t - a.t) / std::max(1e-4f, b.t - a.t);
	auto L = [k](const Vec3& x, const Vec3& y) { return Vec3{ x.x + (y.x - x.x) * k, x.y + (y.y - x.y) * k, x.z + (y.z - x.z) * k }; };
	PathFrame f;
	f.t = t;
	// a curve through both frames that leaves and arrives at their recorded speeds (cubic Hermite): smoother than a
	// straight blend, since replay frames come at uneven times
	{
		const float h = std::max(1e-4f, b.t - a.t), k2 = k * k, k3 = k2 * k;
		const float h00 = 2 * k3 - 3 * k2 + 1, h10 = k3 - 2 * k2 + k, h01 = -2 * k3 + 3 * k2, h11 = k3 - k2;
		f.pos = { h00 * a.pos.x + h10 * h * a.vel.x + h01 * b.pos.x + h11 * h * b.vel.x,
				  h00 * a.pos.y + h10 * h * a.vel.y + h01 * b.pos.y + h11 * h * b.vel.y,
				  h00 * a.pos.z + h10 * h * a.vel.z + h01 * b.pos.z + h11 * h * b.vel.z };
	}
	f.vel = L(a.vel, b.vel); f.angVel = L(a.angVel, b.angVel);
	// slerp
	float d = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3];
	float bq[4] = { b.q[0], b.q[1], b.q[2], b.q[3] };
	if (d < 0) { d = -d; for (float& x : bq) x = -x; }
	float wa = 1 - k, wb = k;
	if (d < 0.9995f)
	{
		const float th = std::acos(std::clamp(d, -1.0f, 1.0f)), st = std::sin(th);
		wa = std::sin((1 - k) * th) / st; wb = std::sin(k * th) / st;
	}
	float n = 0;
	for (int j = 0; j < 4; j++) { f.q[j] = wa * a.q[j] + wb * bq[j]; n += f.q[j] * f.q[j]; }
	n = std::sqrt(std::max(1e-8f, n));
	for (float& x : f.q) x /= n;
	return f;
}

bool Combine::BallAt(const Challenge& c, float t, BallFrame& out)
{
	if (c.ball.empty() || t > c.ballEnd || t > c.ball.back().t + 1e-3f) return false;
	if (t <= c.ball.front().t) { out = c.ball.front(); return true; }
	size_t i = 1;
	while (i < c.ball.size() && c.ball[i].t < t) i++;
	if (i >= c.ball.size()) { out = c.ball.back(); return true; }
	const BallFrame& a = c.ball[i - 1];
	const BallFrame& b = c.ball[i];
	const float k = (t - a.t) / std::max(1e-4f, b.t - a.t);
	auto L = [k](const Vec3& x, const Vec3& y) { return Vec3{ x.x + (y.x - x.x) * k, x.y + (y.y - x.y) * k, x.z + (y.z - x.z) * k }; };
	out.t = t; out.pos = L(a.pos, b.pos); out.vel = L(a.vel, b.vel); out.angVel = L(a.angVel, b.angVel);
	return true;
}
