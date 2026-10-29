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
	return r;
}
}

// ---- Saves ----

void Combine::SaveLevelBand(int level, float& lo, float& hi)
{
	// Spare time for a perfect car, set from 1,127 real saves in match replays (349 from pro games). Pro saves:
	// half had 1.15 s or less, the hardest 10% 0.45 s or less, and no save on record had under 0.15 s, so
	// Legend stops there (the edge of what people have shown they can save, not the edge of what we can check).
	static const float kBands[kSaveLevels + 1] = { 2.20f, 1.25f, 0.80f, 0.50f, 0.30f, 0.15f };
	level = std::clamp(level, 1, kSaveLevels);
	hi = kBands[level - 1];
	lo = kBands[level];
}

const char* Combine::SaveLevelName(int level)
{
	static const char* kNames[kSaveLevels] = { "Warm-up", "Steady", "Sharp", "Elite", "Legend" };
	return kNames[std::clamp(level, 1, kSaveLevels) - 1];
}

std::vector<Combine::SaveShot> Combine::MakeSaveSet(uint32_t seed, int level, int count)
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
		// Where you are: around your box, sometimes out of position, facing any way, maybe already moving
		s.car.pos = { U(-2600.0f, 2600.0f), U(-5100.0f, -2600.0f), 17.0f };
		s.carYaw = U(-pi, pi);
		const float v0 = U(0.0f, 400.0f);   // nearly still, as in Olympics; the check is generous about turning
		s.car.vel = { v0 * std::cos(s.carYaw), v0 * std::sin(s.carYaw), 0.0f };
		s.car.onSurface = true;
		const ReachResult r = SaveReachIn(arena.get(), s.ball, s.car, -1);
		if (!r.onTarget || !r.possible || r.slack < lo || r.slack >= hi) continue;
		s.slack = r.slack;
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
	for (int tries = 0; static_cast<int>(out.size()) < count && tries < count * 300; tries++)
	{
		RecoveryThrow t;
		// Anywhere on the field, from just off the floor to near the ceiling; harder levels throw lower and faster
		t.pos = { U(-3600.0f, 3600.0f), U(-4600.0f, 4600.0f), U(250.0f, 1700.0f) };
		const float sp = U(300.0f, 2200.0f);
		const float a = U(-pi, pi), e = U(-0.9f, 0.7f);
		t.vel = { sp * std::cos(e) * std::cos(a), sp * std::cos(e) * std::sin(a), sp * std::sin(e) };
		t.pitch = U(-pi * 0.5f, pi * 0.5f); t.yaw = U(-pi, pi); t.roll = U(-pi, pi);
		const float w = U(0.0f, 5.5f);
		const float wa = U(-pi, pi), we = U(-1.5f, 1.5f);
		t.angVel = { w * std::cos(we) * std::cos(wa), w * std::cos(we) * std::sin(wa), w * std::sin(we) };
		const RecoveryCheck rc = CheckRecoveryIn(arena.get(), car, t.pos, t.vel, t.angVel, t.pitch, t.yaw, t.roll);
		if (!rc.possible || rc.tHit < 0.25f) continue;
		t.tHit = rc.tHit; t.normal = rc.normal; t.tTurn = rc.tTurn; t.tTurnReal = rc.tTurnReal;
		t.margin = rc.tHit - rc.tTurnReal;
		if (t.margin < lo || t.margin >= hi) continue;
		out.push_back(t);
	}
	return out;
}
