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
	constexpr float kGroundShotMaxZ = 350.0f;   // ball center below this at the touch = a ground shot (jump, flip and low aerial shots included; raised from 260 on Oct 9 ratings)
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
	if (s.touchTick < 2 || s.touchTick + 2 >= n) return s;

	// Dribble: the ball rode on your roof into this touch. The touch can last several ticks while you flick,
	// so judge the shot from the moment the ball leaves your car.
	{
		int release = s.touchTick;
		while (release + 1 < n && fr[release + 1].touch) release++;
		DribbleSpan d = FindDribble(fr, release);
		if (d.valid && release + 3 < n)
		{
			s.dribble = true;
			s.dribbleStart = d.start;
			s.flickJumpTick = d.flickJump;
			int carryEnd = d.flickJump >= 0 ? d.flickJump : d.lastRoof;
			s.dribbleSec = (carryEnd - d.start) / 120.0f;
			float sum = 0;
			for (int i = d.start; i <= carryEnd; i++) sum += std::sqrt(fr[i].ballVX * fr[i].ballVX + fr[i].ballVY * fr[i].ballVY);
			s.dribblePace = sum / std::max(1, carryEnd - d.start + 1);
			s.touchTick = release;
		}
	}
	const int k = s.touchTick;

	// Air dribble: same idea, the final push can last a few ticks, so measure once it's over
	AirDribbleSpan air;
	int airRelease = k;
	if (!s.dribble)
	{
		air = FindAirDribble(fr, k);
		if (air.valid && (air.touches >= 3 || LastWallTap(fr, s.startTick, k - 1) < air.firstTouch))
			while (airRelease + 1 < n && airRelease - k < 12 && fr[airRelease + 1].touch) airRelease++;
		if (airRelease + 3 >= n) airRelease = k;
	}
	const int postTick = s.dribble ? k + 3 : airRelease > k ? airRelease + 3 : k + 2;

	const Frame& pre = fr[k - 1];
	const Frame& post = fr[postTick];   // a couple of ticks after contact, once the hit has settled
	V3 vin = BallVel(pre), vout = BallVel(post);

	// The touch happens partway through the tick before it's reported
	s.ballCenter = Add(BallPos(pre), Mul(vin, 0.5f / 120.0f));
	s.actualContact = Make(fr[k].touchX, fr[k].touchY, fr[k].touchZ);
	s.shotDir = Unit(vout);
	s.shotSpeed = Len(vout);
	// Dribbles can start anywhere on the field (free play), so the goal is the one the flick heads for
	s.goalSign = s.dribble ? (vout.y >= 0 ? 1.0f : -1.0f) : (s.ballCenter.y > 0 || vout.y > 0) ? 1.0f : -1.0f;
	s.flickLift = s.dribble ? vout.z : 0.0f;

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

	// Did it bounce on the floor on the way to the goal line? Watched in the recording when it gets that far;
	// otherwise judged from its path. Hitting the crossbar or a post first doesn't count (crossbar-downs are fine).
	{
		bool decided = false;
		for (int i = postTick + 1; i < n; i++)
		{
			const Frame& a = fr[i - 1];
			const Frame& b = fr[i];
			if (b.touch) break;   // you touched it again
			if (std::fabs(b.ballY) > kGoalY - 20.0f) { decided = true; break; }   // reached the line
			float dvx = b.ballVX - a.ballVX, dvy = b.ballVY - a.ballVY, dvz = b.ballVZ - a.ballVZ;
			bool hit = dvx * dvx + dvy * dvy + (dvz + kGravity / 120.0f) * (dvz + kGravity / 120.0f) > 250.0f * 250.0f;
			bool nearFrame = std::fabs(b.ballY) > kGoalY - 300.0f && std::fabs(b.ballX) < kGoalHalfW + 250.0f &&
				(b.ballZ > kGoalH - 250.0f || std::fabs(b.ballX) > kGoalHalfW - 250.0f);
			if (hit && nearFrame) { decided = true; break; }   // crossbar or post
			if (b.ballZ < kBallR + 50.0f && a.ballVZ < -50.0f && b.ballVZ > 0.0f)
			{
				s.bouncedIn = std::fabs(b.ballY) < kGoalY - 250.0f;   // a drop right on the line is the bar's doing
				decided = true;
				break;
			}
		}
		if (!decided) s.bouncedIn = s.dipsToFloor;
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
	if (s.earliestTick >= 0)
	{
		s.lateBy = (k - s.earliestTick) / 120.0f;
		// Where to hit it then: same target, at least a decent shot speed
		const Frame& e = fr[s.earliestTick];
		s.earliestBall = BallPos(e);
		V3 vI;
		if (LaunchTo(s.earliestBall, target, std::max(Len(vout), 1800.0f), vI))
		{
			V3 nI = Unit(Sub(vI, BallVel(e)));
			s.earliestContact = Sub(s.earliestBall, Mul(nI, kBallR));
			s.earliestValid = true;
		}
	}

	// Car at the touch, and what kind of shot this was
	const Frame& cf = fr[k - 1];
	// Wheels on the floor (not a wall: wall contact counts as "on ground" in the game)
	s.carGrounded = (cf.onGround && cf.carZ < 100.0f) || (fr[k].onGround && fr[k].carZ < 100.0f);
	V3 carVel = CarVel(cf);
	s.carSpeed = Len(carVel);
	V3 flatVel = Make(carVel.x, carVel.y, 0), flatPush = Make(s.idealDir.x, s.idealDir.y, 0);
	s.approachAngleDeg = (Len(flatVel) > 200.0f && Len(flatPush) > 0.1f) ? AngleDeg(flatVel, flatPush) : 0.0f;
	// Hit from the ground, or a low ball reached with a small jump or flip = ground shot; otherwise an aerial
	s.type = (s.carGrounded || s.ballCenter.z < kGroundShotMaxZ) ? ShotType::Ground : ShotType::Aerial;

	// Double tap: the ball came off the backboard and you hit it before it touched the floor
	int bb = LastWallTap(fr, s.startTick, k - 1);
	if (bb >= 0)
	{
		bool floorBetween = false;
		for (int i = bb + 1; i < k; i++) if (fr[i].ballZ < 110.0f && fr[i - 1].ballZ >= 110.0f) { floorBetween = true; break; }
		int setup = -1;
		for (int i = bb; i >= std::max(1, s.startTick); i--)
			if (fr[i].touch && !fr[i - 1].touch) { setup = i; break; }
		// A double tap is your own: you sent it to the backboard, then hit it again before it landed.
		// (A pack that throws the ball off the backboard is graded like any other shot.)
		if (!floorBetween && setup >= 0)
		{
			s.doubleTap = true;
			s.type = ShotType::DoubleTap;
			s.backboardTick = bb;
			s.backboardZ = fr[bb].ballZ;
			s.backboardX = fr[bb].ballX;
			s.tapSurface = WallTapSurface(fr[bb - 1], fr[bb + 1]);
			s.setupTouchTick = setup;
		}
	}
	if (s.dribble && !s.doubleTap) s.type = ShotType::Dribble;

	// Flip reset: the flip came back mid-air at the ball, before the final touch (wheels brushing the ball on
	// the shot itself isn't one)
	for (int i = std::max(1, s.startTick); i < k - 3; i++)
	{
		if (!IsFlipReset(fr[i - 1], fr[i])) continue;
		s.resets++;
		s.resetTick = i;
	}
	if (s.resets > 0)
	{
		s.flipReset = true;
		int t = s.resetTick - 3;
		while (t > 0 && !OnSurface(fr[t])) t--;
		s.resetTakeoff = t + 1;
		for (int i = s.resetTick + 1; i <= k; i++)
			if (fr[i].jump && !fr[i - 1].jump && !OnSurface(fr[i])) { s.resetDodge = i; break; }
		s.flipShot = s.resetDodge >= 0 && k - s.resetDodge >= 0 && k - s.resetDodge <= 40;
		AirDribbleSpan before = FindAirDribble(fr, s.resetTick - 1);
		s.resetAfterAirDribble = before.valid;
	}

	// Air dribble: a chain of touches in the air into the final one. A wall bounce before the chain started is
	// just the pop off the wall, and a backboard bounce in the middle of a long chain (3+ touches, ball kept
	// close) is part of carrying it in, so neither makes it a double tap.
	if (!s.dribble && !s.flipReset)
	{
		const AirDribbleSpan& a = air;
		if (a.valid && (!s.doubleTap || s.backboardTick < a.firstTouch || a.touches >= 3))
		{
			s.airDribble = true;
			s.doubleTap = false;
			s.type = ShotType::AirDribble;
			s.airStart = a.start;
			s.airFirstTouch = a.firstTouch;
			s.airTouches = a.touches;
			s.airSec = (k - a.start) / 120.0f;
			s.airImpulse = a.avgImpulse;
			s.airCloseFrac = a.closeFrac;
		}
	}
	if (s.flipReset && !s.dribble)
	{
		s.type = ShotType::FlipReset;
		s.doubleTap = false;
		s.airDribble = false;
	}

	// The flip behind the shot: your flick off a dribble, or the flip you used off a reset
	if (s.dribble && s.flickJumpTick >= 0)
	{
		for (int i = s.flickJumpTick + 1; i < std::min(n, s.flickJumpTick + 150); i++)
			if (fr[i].jump && !fr[i - 1].jump && !OnSurface(fr[i])) { s.flipTick = i; break; }
		if (s.flipTick >= 0) s.flipDelay = (s.flipTick - s.flickJumpTick) / 120.0f;
	}
	else if (s.type == ShotType::FlipReset)
		s.flipTick = s.resetDodge;
	if (s.flipTick >= 0) s.flipKind = ClassifyFlip(fr[s.flipTick], s.flipRight);

	s.valid = true;
	return s;
}

bool ShotGeometry::BallOnRoof(const Frame& f)
{
	float dx = f.ballX - f.carX, dy = f.ballY - f.carY, above = f.ballZ - f.carZ;
	return dx * dx + dy * dy < 160.0f * 160.0f && above > 40.0f && above < 260.0f;
}

bool ShotGeometry::BallWithCar(const Frame& f)
{
	if (BallOnRoof(f)) return true;
	// Pushing it along: close, low, and moving at about your speed
	float dx = f.ballX - f.carX, dy = f.ballY - f.carY, above = f.ballZ - f.carZ;
	float rvx = f.ballVX - f.carVX, rvy = f.ballVY - f.carVY;
	return dx * dx + dy * dy < 280.0f * 280.0f && above > -40.0f && above < 260.0f && rvx * rvx + rvy * rvy < 650.0f * 650.0f;
}

bool ShotGeometry::OnSurface(const Frame& f)
{
	if (!f.onGround) return false;
	float x = std::fabs(f.carX), y = std::fabs(f.carY);
	bool openAir = f.carZ > 250.0f && f.carZ < 1900.0f && x < 3800.0f && y < 4900.0f && x + y < 7600.0f;
	return !openAir;
}

bool ShotGeometry::IsFlipReset(const Frame& before, const Frame& now)
{
	if (!now.hasFlip || before.hasFlip) return false;
	float x = std::fabs(now.carX), y = std::fabs(now.carY);
	bool openAir = now.carZ > 250.0f && now.carZ < 1900.0f && x < 3800.0f && y < 4900.0f && x + y < 7600.0f;
	float dx = now.ballX - now.carX, dy = now.ballY - now.carY, dz = now.ballZ - now.carZ;
	return openAir && dx * dx + dy * dy + dz * dz < 200.0f * 200.0f;
}

AirDribbleSpan ShotGeometry::FindAirDribble(const std::vector<Frame>& fr, int finalTouch)
{
	constexpr int   kMergeTicks = 8;        // touch reports this close together are one touch
	constexpr float kMaxGap = 500.0f;       // ball never got further than this from your car between touches
	constexpr int   kMinTouches = 2;        // the final touch plus at least one before it
	constexpr float kCloseDist = 230.0f;
	AirDribbleSpan a;
	const int n = static_cast<int>(fr.size());
	if (finalTouch < 2 || finalTouch + 3 >= n) return a;
	auto airborne = [&](int i) { return !fr[i].onGround && !fr[i - 1].onGround; };
	auto dist = [&](const Frame& f) {
		float dx = f.ballX - f.carX, dy = f.ballY - f.carY, dz = f.ballZ - f.carZ;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	};
	if (!airborne(finalTouch)) return a;

	// Touches (merged) before the final one
	std::vector<int> touches;
	for (int i = 1; i < finalTouch; i++)
		if (fr[i].touch && !fr[i - 1].touch && (touches.empty() || i - touches.back() > kMergeTicks)) touches.push_back(i);
	while (!touches.empty() && finalTouch - touches.back() <= kMergeTicks) touches.pop_back();

	// Walk back while each earlier touch was in the air, you never landed, and the ball stayed close
	std::vector<int> chain{ finalTouch };
	for (int t = static_cast<int>(touches.size()) - 1; t >= 0; t--)
	{
		int tk = touches[t];
		if (!airborne(tk)) break;
		bool ok = true;
		for (int i = tk; i <= chain.front() && ok; i++)
			if ((i > tk && fr[i].onGround) || dist(fr[i]) > kMaxGap) ok = false;
		if (!ok) break;
		chain.insert(chain.begin(), tk);
	}
	if (static_cast<int>(chain.size()) < kMinTouches) return a;

	int start = chain.front();
	while (start > 0 && !fr[start].onGround) start--;
	start++;

	float impSum = 0;
	for (size_t c = 0; c + 1 < chain.size(); c++)
	{
		const Frame& b = fr[chain[c] - 1];
		const Frame& f = fr[std::min(chain[c] + 3, n - 1)];
		float dx = f.ballVX - b.ballVX, dy = f.ballVY - b.ballVY, dz = f.ballVZ - b.ballVZ;
		impSum += std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	int close = 0;
	float distSum = 0;
	for (int i = start; i <= finalTouch; i++)
	{
		float d = dist(fr[i]);
		distSum += d;
		if (d < kCloseDist) close++;
	}
	int len = std::max(1, finalTouch - start + 1);

	a.valid = true;
	a.start = start;
	a.firstTouch = chain.front();
	a.touches = static_cast<int>(chain.size());
	a.avgImpulse = impSum / static_cast<float>(chain.size() - 1);
	a.closeFrac = static_cast<float>(close) / len;
	a.meanDist = distSum / len;
	return a;
}

DribbleSpan ShotGeometry::FindDribble(const std::vector<Frame>& fr, int release)
{
	constexpr int kRecentTicks = 60;   // ball was still on your roof within 0.5 s of the final touch ending
	constexpr int kGapTicks = 30;      // short bobbles off the roof don't end the carry
	constexpr int kMinCarry = 60;      // carried for at least 0.5 s
	DribbleSpan d;
	const int n = static_cast<int>(fr.size());
	if (release < 1 || release >= n) return d;

	int lastRoof = -1;
	for (int i = release; i >= std::max(0, release - kRecentTicks); i--)
		if (BallOnRoof(fr[i])) { lastRoof = i; break; }
	if (lastRoof < 0) return d;

	int start = lastRoof, gap = 0, roofTicks = 0, grounded = 0;
	for (int i = lastRoof; i >= 0; i--)
	{
		if (BallOnRoof(fr[i]))
		{
			start = i;
			gap = 0;
			roofTicks++;
			if (fr[i].onGround && fr[i].carZ < 100.0f) grounded++;
		}
		else if (++gap > kGapTicks) break;
	}
	// A ground dribble: long enough, mostly on the roof, and mostly with your wheels down (not an air dribble)
	if (lastRoof - start < kMinCarry || roofTicks < kMinCarry * 3 / 4 || grounded * 10 < roofTicks * 7) return d;

	// The flick: your last jump off the ground during the carry
	for (int i = std::max(start + 1, 1); i <= release; i++)
		if (fr[i].jump && !fr[i - 1].jump && (fr[i].onGround || fr[i - 1].onGround)) d.flickJump = i;

	d.valid = true;
	d.start = start;
	d.lastRoof = lastRoof;
	d.release = release;
	return d;
}

FlipKind ShotGeometry::ClassifyFlip(const Frame& f, bool& right)
{
	// Stick forward (pitch -1) = front flip, back (+1) = backflip, sideways = side flip
	right = f.yaw > 0.0f;
	if (std::sqrt(f.pitch * f.pitch + f.yaw * f.yaw) < 0.3f) return FlipKind::None;
	float fromFront = std::fabs(std::atan2(f.yaw, -f.pitch)) * 57.29578f;   // 0 = straight front, 180 = straight back
	if (fromFront < 22.5f) return FlipKind::Front;
	if (fromFront < 67.5f) return FlipKind::DiagonalFront;
	if (fromFront < 112.5f) return FlipKind::Side;
	if (fromFront < 157.5f) return FlipKind::DiagonalBack;
	return FlipKind::Back;
}

std::string ShotGeometry::FlipName(FlipKind k, bool flick)
{
	const char* what = flick ? "flick" : "flip";
	switch (k)
	{
	case FlipKind::Front:         return std::string("front ") + what;
	case FlipKind::DiagonalFront: return std::string("diagonal front ") + what;
	case FlipKind::Side:          return flick ? "90 flick" : "side flip";
	case FlipKind::DiagonalBack:  return flick ? "45 flick" : "diagonal back flip";
	case FlipKind::Back:          return std::string("back ") + what;
	default:                      return flick ? "flick" : "flip";
	}
}

const char* ShotGeometry::TypeName(ShotType t)
{
	switch (t)
	{
	case ShotType::Ground:    return "Ground shot";
	case ShotType::DoubleTap: return "Double tap";
	case ShotType::Dribble:   return "Dribble";
	case ShotType::AirDribble: return "Air dribble";
	case ShotType::FlipReset: return "Flip reset";
	default:                  return "Aerial";
	}
}

int ShotGeometry::WallTapSurface(const Frame& before, const Frame& after)
{
	// Frames two ticks apart (a bounce often lands mid-tick). A bounce = the ball's velocity changed by more than
	// gravity explains, with no touch from you. Where it happened says what it bounced off.
	if (before.touch || after.touch) return 0;
	float evx = after.ballVX - before.ballVX, evy = after.ballVY - before.ballVY;
	float evz = after.ballVZ - (before.ballVZ - kGravity * (2.0f / 120.0f));
	if (evx * evx + evy * evy + evz * evz < 250.0f * 250.0f) return 0;
	float x = std::fabs(after.ballX), y = std::fabs(after.ballY), z = after.ballZ;
	bool inGoalMouth = x < kGoalHalfW && z < kGoalH && y > 4700.0f;
	if (z < 200.0f || inGoalMouth) return 0;                       // floor, or inside the goal
	if (z > 1700.0f) return 2;                                      // ceiling (and its curved edges)
	if (y > 4700.0f) return 1;                                      // backboard / back wall
	if (z > 1000.0f && (x > 3500.0f || x + y > 7500.0f)) return 3;  // top of a side wall or corner, into the ceiling curve
	return 0;
}

bool ShotGeometry::IsWallTap(const Frame& before, const Frame& after)
{
	return WallTapSurface(before, after) != 0;
}

int ShotGeometry::LastWallTap(const std::vector<Frame>& fr, int from, int to)
{
	int found = -1;
	for (int i = std::max(from, 1); i <= to && i + 1 < static_cast<int>(fr.size()); i++)
		if (IsWallTap(fr[i - 1], fr[i + 1]) && found != i - 1) found = i;
	return found;
}
