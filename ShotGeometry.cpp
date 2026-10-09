#include "pch.h"
#include "ShotGeometry.h"

#include <algorithm>
#include <cmath>

namespace
{
	// Field and physics constants (Unreal units)
	constexpr float kBallR = 92.75f;
	constexpr float kGravity = 650.0f;
	constexpr float kGoalY = 5120.0f;
	constexpr float kGoalHalfW = 892.755f;
	constexpr float kGoalH = 642.775f;
	constexpr float kWinX = kGoalHalfW - kBallR;   // ball center must stay inside these to go in
	constexpr float kWinZLo = kBallR;
	constexpr float kWinZHi = kGoalH - kBallR;
	constexpr float kCarMaxSpeed = 2300.0f;
	// Fastest average acceleration seen toward the ball in recorded reps (98th percentile = 1300).
	constexpr float kReachAccel = 1300.0f;
	constexpr float kReachOffset = 140.0f;         // ball radius plus about half a car

	V3 Make(float x, float y, float z) { return V3{ x, y, z }; }
	V3 Add(V3 a, V3 b) { return Make(a.x + b.x, a.y + b.y, a.z + b.z); }
	V3 Sub(V3 a, V3 b) { return Make(a.x - b.x, a.y - b.y, a.z - b.z); }
	V3 Mul(V3 a, float s) { return Make(a.x * s, a.y * s, a.z * s); }
	float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	float Len(V3 a) { return std::sqrt(Dot(a, a)); }
	V3 Unit(V3 a) { float l = Len(a); return l > 1e-6f ? Mul(a, 1.0f / l) : Make(0, 0, 0); }
	float AngleDeg(V3 a, V3 b)
	{
		float c = Dot(Unit(a), Unit(b));
		c = std::max(-1.0f, std::min(1.0f, c));
		return std::acos(c) * 57.29578f;
	}

	V3 BallPos(const Frame& f) { return Make(f.ballX, f.ballY, f.ballZ); }
	V3 BallVel(const Frame& f) { return Make(f.ballVX, f.ballVY, f.ballVZ); }
	V3 CarPos(const Frame& f) { return Make(f.carX, f.carY, f.carZ); }
	V3 CarVel(const Frame& f) { return Make(f.carVX, f.carVY, f.carVZ); }

	// Straight ballistic flight (gravity only) to the goal line, with a rough floor bounce
	bool CrossGoalLine(V3 p, V3 v, float goalSign, float& x, float& z)
	{
		float y0 = p.y * goalSign, vy = v.y * goalSign;
		if (vy <= 1.0f) return false;
		float t = (kGoalY - y0) / vy;
		if (t < 0 || t > 4) return false;
		x = p.x + v.x * t;
		z = p.z + v.z * t - 0.5f * kGravity * t * t;
		if (z < kBallR)
		{
			float a = -0.5f * kGravity, b = v.z, c = p.z - kBallR;
			float disc = b * b - 4 * a * c;
			float tf = disc >= 0 ? (-b - std::sqrt(disc)) / (2 * a) : t;
			float vz2 = -(v.z - kGravity * tf) * 0.6f;
			float t2 = t - tf;
			z = std::max(kBallR, kBallR + vz2 * t2 - 0.5f * kGravity * t2 * t2);
		}
		return true;
	}

	float MissDistance(float x, float z)
	{
		float dx = std::max(0.0f, std::fabs(x) - kWinX);
		float dz = std::max({ 0.0f, kWinZLo - z, z - kWinZHi });
		return std::sqrt(dx * dx + dz * dz);
	}

	// Launch velocity with speed S from p that passes through target (lowest, fastest arc)
	bool LaunchTo(V3 p, V3 target, float S, V3& out)
	{
		V3 D = Sub(target, p);
		auto f = [&](float t) { return Len(Make(D.x / t, D.y / t, D.z / t + 0.5f * kGravity * t)) - S; };
		float tPrev = 0.05f, prev = f(tPrev);
		for (float t = 0.06f; t < 4.0f; t += 0.01f)
		{
			float cur = f(t);
			if (prev > 0 && cur <= 0)
			{
				float a = tPrev, b = t;
				for (int i = 0; i < 40; i++)
				{
					float m = 0.5f * (a + b);
					if (f(m) > 0) a = m; else b = m;
				}
				float tt = 0.5f * (a + b);
				out = Make(D.x / tt, D.y / tt, D.z / tt + 0.5f * kGravity * tt);
				return true;
			}
			prev = cur;
			tPrev = t;
		}
		// Not fast enough to get there on a straight arc (e.g. it only goes in off a bounce):
		// use the arc that needs the least speed, which still points the right way
		float bestT = 0.05f, bestF = f(bestT);
		for (float t = 0.06f; t < 4.0f; t += 0.01f)
		{
			float cur = f(t);
			if (cur < bestF) { bestF = cur; bestT = t; }
		}
		out = Make(D.x / bestT, D.y / bestT, D.z / bestT + 0.5f * kGravity * bestT);
		return true;
	}

	// How far a car can travel in time t starting at speed v0, accelerating at a, capped at max speed
	float ReachDistance(float v0, float a, float t)
	{
		if (v0 >= kCarMaxSpeed) return kCarMaxSpeed * t;
		float tc = (kCarMaxSpeed - v0) / a;
		if (t <= tc) return v0 * t + 0.5f * a * t * t;
		return v0 * tc + 0.5f * a * tc * tc + kCarMaxSpeed * (t - tc);
	}
}

ShotAnalysis ShotGeometry::Analyze(const std::vector<Frame>& fr)
{
	ShotAnalysis s;
	const int n = static_cast<int>(fr.size());

	for (int i = 0; i < n; i++)
	{
		if (std::fabs(fr[i].ballVX) + std::fabs(fr[i].ballVY) + std::fabs(fr[i].ballVZ) > 1.0f) { s.startTick = i; break; }
	}
	// Grade the last touch: on multi-touch plays the earlier touches are setup
	for (int i = 2; i < n; i++)
	{
		if (fr[i].touch && !fr[i - 1].touch && i + 2 < n) s.touchTick = i;
	}
	const int k = s.touchTick;
	if (k < 2 || k + 2 >= n) return s;

	const Frame& pre = fr[k - 1];
	const Frame& post = fr[k + 2];   // a couple of ticks after contact, once the hit has settled
	V3 vin = BallVel(pre), vout = BallVel(post);

	// The touch happens partway through the tick before it's reported
	s.ballCenter = Add(BallPos(pre), Mul(vin, 0.5f / 120.0f));
	s.actualContact = Make(fr[k].touchX, fr[k].touchY, fr[k].touchZ);
	s.shotDir = Unit(vout);
	s.shotSpeed = Len(vout);
	s.goalSign = (s.ballCenter.y > 0 || vout.y > 0) ? 1.0f : -1.0f;

	// Where the shot was going
	float cx = 0, cz = 0;
	s.reachesGoalLine = CrossGoalLine(s.ballCenter, vout, s.goalSign, cx, cz);
	if (s.reachesGoalLine)
	{
		s.crossX = cx;
		s.crossZ = cz;
		s.missDist = MissDistance(cx, cz);
		s.cornerDist = std::sqrt((std::fabs(cx) - kWinX) * (std::fabs(cx) - kWinX) + (cz - kWinZHi) * (cz - kWinZHi));
		float tLine = (kGoalY - s.ballCenter.y * s.goalSign) / (vout.y * s.goalSign);
		s.dipsToFloor = s.ballCenter.z + vout.z * tLine - 0.5f * kGravity * tLine * tLine < kBallR;
		if (s.missDist > 0)
		{
			// Facing the goal at +Y, +X is to your left (it flips for the -Y goal)
			float leftX = cx * s.goalSign;
			if (std::fabs(cx) > kWinX) s.missDir = leftX > 0 ? "left" : "right";
			if (cz > kWinZHi) s.missDir += s.missDir.empty() ? "high" : " and high";
			else if (cz < kWinZLo) s.missDir += s.missDir.empty() ? "low" : " and low";
		}
	}

	// Closest on-target spot to where it was going (or the middle of the net if it never got there)
	float tx = 0.0f, tz = 300.0f;
	if (s.reachesGoalLine)
	{
		tx = std::max(-kWinX + 50.0f, std::min(kWinX - 50.0f, cx));
		tz = std::max(kWinZLo + 50.0f, std::min(kWinZHi - 50.0f, cz));
	}
	V3 target = Make(tx, s.goalSign * (kGoalY + kBallR), tz);

	V3 vIdeal;
	if (LaunchTo(s.ballCenter, target, Len(vout), vIdeal))
	{
		s.idealDir = Unit(vIdeal);
		s.aimErrDeg = AngleDeg(vout, vIdeal);

		// The ball gets pushed roughly along the line from the contact point through its center,
		// so the ideal contact point is on the far side of the push it needed
		V3 nIdeal = Unit(Sub(vIdeal, vin));
		s.idealContact = Sub(s.ballCenter, Mul(nIdeal, kBallR));

		V3 fwd = Unit(Make(target.x - s.ballCenter.x, target.y - s.ballCenter.y, 0));
		V3 left = Make(fwd.y, -fwd.x, 0);  // +X when facing +Y
		V3 d = Sub(s.actualContact, s.idealContact);
		s.contactErr = Len(d);
		s.contactUp = d.z;
		s.contactLeft = Dot(d, left);
	}

	// Earliest tick a fast, direct approach could have reached the ball
	const Frame& f0 = fr[s.startTick];
	V3 cp = CarPos(f0), cv = CarVel(f0);
	for (int i = s.startTick + 6; i <= k; i++)
	{
		float t = (i - s.startTick) / 120.0f;
		V3 dvec = Sub(BallPos(fr[i]), cp);
		float d = std::max(0.0f, Len(dvec) - kReachOffset);
		float v0 = std::max(0.0f, Dot(cv, Unit(dvec)));
		if (ReachDistance(v0, kReachAccel, t) >= d) { s.earliestTick = i; break; }
	}
	if (s.earliestTick >= 0) s.lateBy = (k - s.earliestTick) / 120.0f;

	s.valid = true;
	return s;
}
