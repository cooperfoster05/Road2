// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <format>
#include <cctype>

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
			const float nx = U(-60.0f, 60.0f), ny = U(-60.0f, 60.0f);
			s.car.pos.x += nx; s.car.pos.y += ny;
			s.car.vel.x += U(-80.0f, 80.0f); s.car.vel.y += U(-80.0f, 80.0f);
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
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 400; tries++)
	{
		RecoveryThrow t;
		// Moments you'd really be in, anywhere on the field (Oct 29): coming out of an aerial, knocked about in a 50/50,
		// bailing off a wall, or just thrown. Higher levels spin harder and travel faster.
		const float kind = U(0.0f, 1.0f);
		const float spinLo = 0.4f + 0.7f * (lv - 1.0f), spinHi = std::min(5.5f, 2.6f + 0.75f * lv);
		const float w = U(spinLo, spinHi);
		if (kind < 0.35f)
		{
			// Out of an aerial: high, travelling up field (or across), nose up
			t.pos = { U(-3400.0f, 3400.0f), U(-2500.0f, 4600.0f), U(700.0f, 1750.0f) };
			const float a = U(0.4f, 2.7f), sp = U(900.0f, 1500.0f + 180.0f * lv), e = U(-0.3f, 0.6f);
			t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			t.pitch = U(0.2f, pi * 0.5f); t.yaw = std::atan2(t.vel.y, t.vel.x) + U(-0.8f, 0.8f) * lv * 0.5f; t.roll = U(-pi, pi) * (0.2f + 0.16f * lv);
		}
		else if (kind < 0.60f)
		{
			// Knocked about in a 50/50 or a bump: low, fast, spinning
			t.pos = { U(-3600.0f, 3600.0f), U(-4200.0f, 4600.0f), U(180.0f, 650.0f) };
			const float a = U(-pi, pi), sp = U(700.0f, 1300.0f + 180.0f * lv), e = U(0.0f, 0.7f);
			t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
			t.pitch = U(-pi * 0.5f, pi * 0.5f); t.yaw = U(-pi, pi); t.roll = U(-pi, pi);
		}
		else if (kind < 0.80f)
		{
			// Bailing off a side wall: near it, high, moving off it
			const float side = U(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
			t.pos = { side * U(3300.0f, 3800.0f), U(-4000.0f, 4500.0f), U(500.0f, 1800.0f) };
			const float sp = U(500.0f, 900.0f + 200.0f * lv);
			t.vel = { -side * U(200.0f, 700.0f), U(-1.0f, 1.0f) * sp, U(-300.0f, 400.0f) };
			t.pitch = U(-0.8f, 0.8f); t.yaw = U(-pi, pi); t.roll = side * U(0.6f, 1.6f) + U(-0.5f, 0.5f) * lv * 0.4f;
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
		if (t.margin < lo || t.margin >= hi) continue;
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
			t.par = rc.tHit + RealDriveTime(sp, along, theta, std::max(0.0f, d - kRecoveryTargetRadius));
		}
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
