// The engine, part 2: the skills the step search plays (carry, dribble, flick, first touch, reset, flip shot) and running them from a save point.
// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "EngineInternal.h"

namespace EngineInternal
{
	// The push a ball needs to drop into T (a spot on the goal line) from where it'll be at ti, flying flat at least
	// 1300: returned as a point a little way along that push from the ball, the form plans take their aim in
	Vec LiftAim(const std::vector<Vec>& path, int ti, const Vec& T)
	{
		const int n = static_cast<int>(path.size());
		ti = std::clamp(ti, 1, n - 2);
		Vec B = path[ti], vB = (path[ti + 1] - path[ti - 1]) * 60.0f;
		Vec d = T - B;
		float flat = std::sqrt(d.x * d.x + d.y * d.y);
		float vflat = std::max(1300.0f, std::sqrt(vB.x * vB.x + vB.y * vB.y));
		float t = std::clamp(flat / vflat, 0.2f, 2.0f);
		Vec want(d.x / t, d.y / t, d.z / t + 325.0f * t);
		Vec push = want - vB;
		float pl = push.Length();
		Vec nrm = pl > 80.0f ? push / pl : want / std::max(1.0f, want.Length());
		return B + nrm * 300.0f;
	}

	// A save point at the search start, straight from your recording
	SavePoint StartPoint(Arena* arena, Car* car, const std::vector<Frame>& fr, int s)
	{
		SavePoint sp;
		ClearCarInternals(arena, car);
		SetClock(arena, s);
		LoadState(car, arena->ball, fr, s);
		sp.grip = RecordedGrip(fr[s]);
		SetGrip(car, sp.grip);
		ResetPads(arena);
		for (BoostPad* p : arena->GetBoostPads()) sp.pads.push_back(p->GetState());
		sp.car = car->GetState();
		sp.ball = arena->ball->GetState();
		sp.tick = s;
		sp.start = s;
		sp.frames.push_back(fr[s]);
		return sp;
	}

	// Air dribble carry: keep the car under and behind the ball (relative to the goal), matching its speed, so the
	// nose meets it softly and keeps pushing it toward the goal. back/down: where the car's center sits from the
	// ball's; k: how hard it closes the gap.
	Controller CarrySkill(float gs, float back, float down, float k, Vec goalAim)
	{
		return [gs, back, down, k, goalAim](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec goal(goalAim.x, gs * 5120.0f, goalAim.z);
			Vec gd = goal - ball.pos; gd.z *= 0.3f;
			gd = gd / std::max(1.0f, gd.Length());
			Vec aimPt = ball.pos - gd * back + Vec(0, 0, -down);
			Vec vDes = ball.vel + (aimPt - car.pos) * k;
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);   // plus a little to hold against gravity
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos; toBall = toBall / std::max(1.0f, toBall.Length());
			Vec fwd = need > 250.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// Air dribble carry aimed at a spot in the net: every tick, work out the velocity the ball needs to drop into
	// (target.x, the goal line, target.z) from where it is (flying flat at least minFlat), and sit the car on the
	// side of the ball that pushes it that way. Soft touches keep nudging the ball onto that arc, so the carry ends
	// with the ball heading high into the net instead of sinking into the bottom of it.
	Controller CarryAim(float gs, float tx, float tz, float back, float k, float minFlat)
	{
		return [gs, tx, tz, back, k, minFlat](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec T(tx, gs * 5120.0f, tz);
			Vec d = T - ball.pos;
			float flat = std::sqrt(d.x * d.x + d.y * d.y);
			float vflat = std::max(minFlat, std::sqrt(ball.vel.x * ball.vel.x + ball.vel.y * ball.vel.y));
			float t = std::clamp(flat / vflat, 0.25f, 2.0f);
			Vec want(d.x / t, d.y / t, d.z / t + 325.0f * t);
			Vec push = want - ball.vel;
			float pl = push.Length();
			Vec dir = pl > 60.0f ? push / pl : want / std::max(1.0f, want.Length());
			if (dir.z < -0.3f) { dir.z = -0.3f; dir = dir / std::max(1e-3f, dir.Length()); }   // never come down on top of it
			Vec aimPt = ball.pos - dir * back;
			Vec vDes = ball.vel + (aimPt - car.pos) * k;
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos; toBall = toBall / std::max(1.0f, toBall.Length());
			Vec fwd = need > 250.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	Controller ParamCarry(float gs, Knobs P)
	{
		return [gs, P](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec goal(P[0], gs * 5120.0f, P[1]);
			Vec gd = goal - ball.pos; gd.z *= 0.3f;
			gd = gd / std::max(1.0f, gd.Length());
			float down = P[3];
			if (ball.vel.y * gs > 200.0f)
			{
				float t = std::clamp((gs * 5120.0f - ball.pos.y) / ball.vel.y, 0.0f, 1.5f);
				float cz = ball.pos.z + ball.vel.z * t - 325.0f * t * t;
				down = std::clamp(down + P[5] * (515.0f - cz), -60.0f, 160.0f);
			}
			Vec aimPt = ball.pos - gd * P[2] + Vec(0, 0, -down);
			Vec vDes = ball.vel + (aimPt - car.pos) * P[4];
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos; toBall = toBall / std::max(1.0f, toBall.Length());
			Vec fwd = need > 250.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// Ground dribble carry: keep the car under the ball (the ball sitting P[0] ahead of the car's center) while it heads
	// for the goal. Knobs: ball ahead of center (uu), position gain, cruise speed, throttle gain, steering gain, aim x on
	// the goal line, how much it steers the ball toward the goal (0 = follow the ball's own line), lateral gain.
	// Measured on Coop's 30 held-out dribbles (Oct 14): tuned from the start of his dribble, it brings the ball on the
	// roof to where he was 1 s before his flick, at his speed, on 18 of 29 (the losses are the 4-5 s carries).
	Controller DribbleSkill(float gs, Knobs P)
	{
		return [gs, P](int, const CarState& car, const BallState& ball) {
			CarControls c;
			Vec b(ball.pos.x, ball.pos.y, 0), bv(ball.vel.x, ball.vel.y, 0), cp(car.pos.x, car.pos.y, 0), cv(car.vel.x, car.vel.y, 0);
			Vec goal(P[5], gs * 5120.0f, 0);
			Vec dg = goal - b; dg = dg / std::max(1.0f, dg.Length());
			Vec db = bv.Length() > 200.0f ? bv / bv.Length() : dg;
			Vec d = db * (1.0f - P[6]) + dg * P[6]; d = d / std::max(1e-3f, d.Length());
			Vec e = (b - d * P[0]) - cp;
			Vec vd = bv * 0.6f + d * (P[2] * 0.4f) + e * P[1];
			Vec f(car.rotMat.forward.x, car.rotMat.forward.y, 0); f = f / std::max(1e-3f, f.Length());
			Vec r(car.rotMat.right.x, car.rotMat.right.y, 0); r = r / std::max(1e-3f, r.Length());
			const float ang = std::atan2(vd.Dot(r), vd.Dot(f));
			c.steer = Bot::Clamp1(P[4] * ang + P[7] * (b - cp).Dot(r) / 100.0f);   // the ball off to one side pulls the car under it
			const float sf = cv.Dot(f), want = vd.Length();
			c.throttle = Bot::Clamp1((want - sf) * P[3]);
			c.boost = want - sf > 150.0f && sf < 2250.0f;
			return c;
		};
	}

	// Dribble flick: keep dribbling (the carry skill's settings) while turning toward the aim for P[0] ticks, jump
	// (held P[1] ticks), then P[2] ticks after the jump flip with the stick at (pitch P[3], yaw P[4]) to flick it.
	// P[5]: aim x on the goal line. Never touches anything after the flip.
	Controller FlickSkill(float gs, Knobs P)
	{
		const Knobs carryK = { 70.0f, 5.5f, 1400.0f, 0.004f, 5.5f, P[5], 0.6f, 1.5f };
		Controller carry = DribbleSkill(gs, carryK);
		struct Mem { int lastTick = -1000; int t = 0; int jumpAt = -1; };
		auto mem = std::make_shared<Mem>();
		return [carry, P, mem](int tick, const CarState& car, const BallState& ball) {
			Mem& memL = PerThread(mem);   // this thread's copy (the engine runs on several cores)
			if (tick <= memL.lastTick || tick > memL.lastTick + 1) { memL.t = 0; memL.jumpAt = -1; }
			memL.lastTick = tick;
			const int t = memL.t++;
			if (t < static_cast<int>(P[0])) return carry(tick, car, ball);
			CarControls c;
			if (memL.jumpAt < 0) memL.jumpAt = t;
			const int since = t - memL.jumpAt;
			if (since < static_cast<int>(P[1])) { c.jump = true; return c; }
			const int flipAt = std::max(static_cast<int>(P[1]) + 2, static_cast<int>(P[2]));
			if (since >= flipAt && since < flipAt + 3) { c.jump = true; c.pitch = Bot::Clamp1(P[3]); c.yaw = Bot::Clamp1(P[4]); return c; }
			if (since >= flipAt + 3) { c.pitch = Bot::Clamp1(P[3]); c.yaw = Bot::Clamp1(P[4]); }
			return c;
		};
	}

	// Air dribble first touch: from just after the pop (still on the wall or floor, or already up), leave the surface
	// after a set wait, then fly to where the ball will be a moment ahead and meet it softly from under and behind.
	// Knobs: where it aims the ball (gx, gz on the goal line), where the car sits (back, down), closing gain (k),
	// how far ahead it reads the ball (lead, s), how long it waits on the surface before jumping (ticks), and how long
	// it holds the jump (ticks). Never jumps a second time (the flip stays unused).
	Controller FirstTouchSkill(float gs, Knobs P)
	{
		struct Mem { int lastTick = -1000; int surf = 0; int held = 0; };
		auto mem = std::make_shared<Mem>();
		return [gs, P, mem](int tick, const CarState& car, const BallState& ball) {
			Mem& memL = PerThread(mem);   // this thread's copy (the engine runs on several cores)
			CarControls c;
			if (tick <= memL.lastTick || tick > memL.lastTick + 1) { memL.surf = 0; memL.held = 0; }
			memL.lastTick = tick;
			Vec toBall = ball.pos - car.pos;
			const float tb = std::max(1.0f, toBall.Length());
			if (car.isOnGround && memL.held == 0)
			{
				// On the wall or floor: keep driving at the ball until it's time to jump
				c.throttle = 1;
				c.steer = Bot::Clamp1(3.0f * car.rotMat.right.Dot(toBall / tb));
				if (memL.surf++ >= static_cast<int>(P[6])) { c.jump = true; memL.held = 1; }
				return c;
			}
			if (memL.held > 0 && memL.held < static_cast<int>(P[7])) { c.jump = true; memL.held++; }   // the first jump, held
			else if (memL.held > 0) memL.held = 1000;   // let go for good
			const float lead = std::clamp(P[5], 0.0f, 0.6f);
			Vec bp = ball.pos + ball.vel * lead + Vec(0, 0, -325.0f * lead * lead);
			Vec bv = ball.vel + Vec(0, 0, -650.0f * lead);
			Vec goal(P[0], gs * 5120.0f, P[1]);
			Vec gd = goal - bp; gd.z *= 0.3f;
			gd = gd / std::max(1.0f, gd.Length());
			Vec aimPt = bp - gd * P[2] + Vec(0, 0, -P[3]);
			Vec vDes = bv + (aimPt - car.pos) * P[4];
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.15f);
			float need = dv.Length();
			Vec fwd = need > 250.0f ? dv / need : toBall / tb;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// A good first touch (measured on Coop's air dribbles: after one, the carry finishes high 51% of the time, after
	// the rest 10%): the ball up high (above 1,425), touched soft (rising slower than ~506), close to the car's nose
	float FirstTouchFit(const SavePoint& at, float gs)
	{
		const float dist = (at.ball.pos - at.car.pos).Length();
		const float z = at.ball.pos.z, vz = at.ball.vel.z;
		const bool good = z > 1425.0f && vz <= 506.0f && dist < 300.0f;
		return (good ? 1000.0f : 0.0f) + 300.0f * std::clamp((z - 1425.0f) / 300.0f, -1.5f, 1.0f) - 0.6f * std::max(0.0f, vz - 506.0f)
			- std::max(0.0f, dist - 250.0f) + 150.0f * std::clamp(at.ball.vel.y * gs / 1000.0f, -1.0f, 1.0f) + 100.0f * at.car.rotMat.forward.z
			+ at.car.boost;
	}

	// Your reps are mostly in training or freeplay with unlimited boost on (your boost never drops while you hold it:
	// 2,455 of 2,587 reps that used boost, Oct 14). This gives the sim unlimited boost too (not switched on: see LineAt).
	bool UnlimitedBoost(const std::vector<Frame>& fr)
	{
		int held = 0;
		float low = 1000.0f;
		for (const Frame& f : fr) { held += f.boost ? 1 : 0; low = std::min(low, f.boostAmount); }
		return held > 30 && low >= 99.0f;
	}

	void MatchBoostRule(Arena* arena, const std::vector<Frame>& fr)
	{
		MutatorConfig mc = arena->GetMutatorConfig();
		mc.boostUsedPerSecond = UnlimitedBoost(fr) ? 0.0f : RLConst::BOOST_USED_PER_SECOND;
		arena->SetMutatorConfig(mc);
	}

	// The ball and car a moment (10 ticks) after an air dribble's first touch, from a run's frames (indexed by tick)
	bool FirstTouchAfter(const std::vector<Frame>& f, int firstTouch, float& z, float& vz, float& dist)
	{
		if (firstTouch < 0 || firstTouch >= static_cast<int>(f.size())) return false;
		const Frame& g = f[std::min(static_cast<int>(f.size()) - 1, firstTouch + 10)];
		z = g.ballZ; vz = g.ballVZ;
		dist = (Vec(g.ballX - g.carX, g.ballY - g.carY, g.ballZ - g.carZ)).Length();
		return true;
	}

	// The closest the car got to the ball from a tick on (for scoring tries that didn't make it)
	float ClosestFrom(const SavePoint& at, int fromTick)
	{
		float best = 1e5f;
		for (int i = std::max(0, fromTick - at.start); i < static_cast<int>(at.frames.size()); i++)
		{
			const Frame& f = at.frames[i];
			best = std::min(best, (Vec(f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ)).Length());
		}
		return best;
	}

	// A good spot to hand an air dribble to the finish (measured on Coop's 165 air dribbles, 1,000 to 2,800 out:
	// from spots like this the tuned finish goes in high 65% of the time, from the rest 21%): the ball moving 1,675+,
	// above 1,130, within 300 of the car, the car keeping pace with it (not running into it)
	float HandoffScore(const SavePoint& at, float gs, bool* goodOut)
	{
		const BallState& b = at.ball;
		const CarState& c = at.car;
		const float speed = b.vel.Length(), dist = (b.pos - c.pos).Length();
		const float relVF = (b.vel - c.vel).Dot(c.rotMat.forward);
		const bool good = speed > 1675.0f && b.pos.z > 1130.0f && dist < 300.0f && relVF > -68.0f;
		if (goodOut) *goodOut = good;
		return (good ? 1000.0f : 0.0f) + std::clamp((speed - 1675.0f) / 3.0f, -200.0f, 100.0f) + std::clamp((b.pos.z - 1130.0f) / 3.0f, -200.0f, 100.0f)
			- std::max(0.0f, dist - 250.0f) + std::clamp(relVF + 68.0f, -150.0f, 50.0f) + std::clamp(b.vel.y * gs - 1200.0f, -300.0f, 300.0f) / 3.0f;
	}

	// Flip reset: get the wheels onto the ball. Flies at a spot just off the ball nose first, then close in turns its
	// wheels to the ball (nose toward the goal) and settles onto it. Knobs: how far the contact leans toward the goal
	// (a) and sideways (b), position gain (k), how close it flies in nose first (sw), closing speed at contact (uc),
	// and how far ahead it reads the ball (lead, s).
	Controller ResetSkill(float gs, Knobs P)
	{
		// A little memory: whether the flip came back on the ball during this run (a new run is spotted by the tick
		// going backwards or jumping, so the same skill can be played again from any save point)
		struct Mem { int lastTick = -1000; bool prevFlip = true; bool done = false; };
		auto mem = std::make_shared<Mem>();
		return [gs, P, mem](int tick, const CarState& car, const BallState& ball) {
			Mem& memL = PerThread(mem);   // this thread's copy (the engine runs on several cores)
			CarControls c;
			if (tick <= memL.lastTick || tick > memL.lastTick + 1) { memL.done = false; memL.prevFlip = car.HasFlipOrJump(); }
			memL.lastTick = tick;
			const bool flip = car.HasFlipOrJump();
			if (flip && !memL.prevFlip && (ball.pos - car.pos).Length() < 220.0f) memL.done = true;
			memL.prevFlip = flip;
			// Reset done: let go of the ball, nose toward it, flip saved for the shot
			if (memL.done)
			{
				Vec toBall = ball.pos - car.pos;
				Bot::Orient(car, toBall / std::max(1.0f, toBall.Length()), Vec(0, 0, 1), false, c, 3.0f, 0.4f);
				return c;
			}
			const float lead = std::clamp(P[5], 0.0f, 0.4f);
			Vec bp = ball.pos + ball.vel * lead + Vec(0, 0, -325.0f * lead * lead);
			Vec bv = ball.vel + Vec(0, 0, -650.0f * lead);
			Vec gdir(0, gs, 0), side(1, 0, 0);
			Vec u = Vec(0, 0, 1) + gdir * P[0] + side * P[1];   // from the car to the ball at contact
			u = u / std::max(1e-3f, u.Length());
			// Until the ball nears the top of its arc (its climb slows below P[7]), hang P[6] under it; then go for it
			const bool wait = ball.vel.z > P[7];
			Vec target = bp - u * (112.0f + (wait ? P[6] : 0.0f));   // the car's center: ball radius plus the car's half height
			Vec vDes = bv + (target - car.pos) * P[2] + (wait ? Vec() : u * P[4]);
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.12f);
			float need = dv.Length();
			float dist = (target - car.pos).Length();
			if (wait || dist > P[3])
			{
				Vec fwd = need > 150.0f ? dv / need : (target - car.pos) / std::max(1.0f, dist);
				Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
				c.boost = need > 150.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			}
			else
			{
				Vec up = u * -1.0f;
				Vec fwd = gdir - up * gdir.Dot(up);
				fwd = fwd / std::max(1e-3f, fwd.Length());
				Bot::Orient(car, fwd, up, true, c, 4.0f, 0.45f);
				c.boost = car.rotMat.forward.Dot(dv) > 300.0f;
			}
			c.throttle = 0;
			// A reset only gives the flip back if it's gone: with one left, spend it as a plain double jump (stick
			// centered, so it doesn't flip) while climbing to the ball
			if (!car.isOnGround && car.HasFlipOrJump() && !car.isJumping && (target.z - car.pos.z) > 40.0f)
			{
				c.jump = true;
				c.pitch = 0; c.yaw = 0; c.roll = 0;
			}
			return c;
		};
	}

	// Flip shot (after a reset): stay lined up behind the ball along the shot (aimed at gx, gz on the goal line),
	// then flip into it once close. Knobs: aim (gx, gz), where it sits behind the ball (back) and how hard it closes
	// (k), the distance it flips from (trig), how far ahead it reads the ball (lead), flip direction mix (side).
	Controller FlipShot(float gs, Knobs P)
	{
		struct Mem { int lastTick = -1000; int dodgeAt = -1; };
		auto mem = std::make_shared<Mem>();
		return [gs, P, mem](int tick, const CarState& car, const BallState& ball) {
			Mem& memL = PerThread(mem);   // this thread's copy (the engine runs on several cores)
			CarControls c;
			if (tick <= memL.lastTick || tick > memL.lastTick + 1) memL.dodgeAt = -1;
			memL.lastTick = tick;
			if (memL.dodgeAt >= 0)
			{
				// Mid-flip: keep the stick where the flip went, nothing else to do
				c.pitch = -1.0f; c.yaw = Bot::Clamp1(P[6]);
				return c;
			}
			const float lead = std::clamp(P[5], 0.0f, 0.4f);
			Vec bp = ball.pos + ball.vel * lead + Vec(0, 0, -325.0f * lead * lead);
			Vec aim(P[0], gs * 5120.0f, P[1]);
			Vec d = aim - bp; d = d / std::max(1.0f, d.Length());
			// The ball keeps its own speed through the hit, so the car lines up behind the push the ball needs (to fly
			// from where it is into the aim spot at shot speed), not behind the straight line to the aim (P[7] blends
			// the two: 0 = straight line, 1 = the push)
			{
				Vec dd = aim - bp;
				const float flat = std::sqrt(dd.x * dd.x + dd.y * dd.y);
				const float t = std::clamp(flat / 2000.0f, 0.15f, 1.5f);
				Vec want(dd.x / t, dd.y / t, dd.z / t + 325.0f * t);
				Vec push = want - (ball.vel + Vec(0, 0, -650.0f * lead));
				const float pl = push.Length();
				if (pl > 100.0f)
				{
					const float w = std::clamp(P[7], 0.0f, 1.0f);
					Vec m = d * (1.0f - w) + (push / pl) * w;
					d = m / std::max(1e-3f, m.Length());
				}
			}
			Vec target = bp - d * P[2];
			Vec vDes = ball.vel + (target - car.pos) * P[3];
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.12f);
			float need = dv.Length();
			Vec toBall = ball.pos - car.pos;
			float dist = toBall.Length();
			toBall = toBall / std::max(1.0f, dist);
			Vec fwd = need > 200.0f ? dv / need : toBall;
			Bot::Orient(car, fwd, Vec(0, 0, 1), false, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(fwd) > 0.75f;
			c.throttle = 1;
			if (dist < P[4] && car.rotMat.forward.Dot(toBall) > 0.7f && car.HasFlipOrJump() && !car.isOnGround)
			{
				c.jump = true; c.pitch = -1.0f; c.yaw = Bot::Clamp1(P[6]); c.roll = 0; c.boost = false;
				memL.dodgeAt = tick;
			}
			return c;
		};
	}

	SavePoint RunFrom(Arena* arena, Car* car, const SavePoint& sp, const Bot::Plan& plan, int maxTicks, const StepEnd& ended, int& result,
		const AfterTouch* afterTouch, const std::vector<CarControls>* fixed, const Controller* ctrl)
	{
		tRunEpoch++;   // controllers start fresh
		SavePoint r = sp;
		if (sp.tick != sp.start) r.bounds.push_back(sp.tick);
		ClearCarInternals(arena, car);
		SetClock(arena, sp.tick);
		car->SetState(sp.car);
		SetGrip(car, sp.grip);
		arena->ball->SetState(sp.ball);
		{
			const auto& pads = arena->GetBoostPads();
			for (size_t i = 0; i < pads.size(); i++) pads[i]->SetState(i < sp.pads.size() ? sp.pads[i] : BoostPadState());
		}
		Bot::State st;
		if (!sp.car.isOnGround) { st.jumpStarted = sp.tick; st.noJump = true; st.doubleJumped = true; }
		Bot::Plan follow;
		Bot::State fst;
		bool following = false;
		result = -1;
		for (int k = 0; k < maxTicks; k++)
		{
			CarState before = car->GetState();
			CarControls c;
			if (afterTouch && st.touched >= 0 && r.tick >= st.touched)
			{
				if (!following || (r.tick - st.touched) % 10 == 0)
				{
					if (!following) { fst = Bot::State(); fst.jumpStarted = r.tick; fst.noJump = !before.isOnGround; fst.doubleJumped = true; }
					following = (*afterTouch)(r.tick, before, arena->ball->GetState(), follow) || following;
				}
				if (following) c = Bot::Drive(before, follow, r.tick, fst);
			}
			else if (ctrl) c = (*ctrl)(r.tick, before, arena->ball->GetState());
			else if (fixed) c = r.tick < static_cast<int>(fixed->size()) ? (*fixed)[r.tick] : CarControls();
			else c = Bot::Drive(before, plan, r.tick, st);
			if (static_cast<int>(r.ctl.size()) <= r.tick) r.ctl.resize(r.tick + 1);
			r.ctl[r.tick] = c;
			Frame& cur = r.frames.back();
			cur.throttle = c.throttle; cur.steer = c.steer; cur.pitch = c.pitch; cur.yaw = c.yaw; cur.roll = c.roll;
			cur.jump = c.jump; cur.boost = c.boost; cur.handbrake = c.handbrake;
			car->controls = c;
			uint64_t t0 = arena->tickCount;
			arena->Step(1);
			r.tick++;
			CarState cs = car->GetState();
			BallState bs = arena->ball->GetState();
			bool hit = cs.ballHitInfo.isValid && cs.ballHitInfo.tickCountWhenHit >= t0;
			r.frames.push_back(SimFrame(r.tick, cs, bs, hit, cs.ballHitInfo.ballPos + cs.ballHitInfo.relativePosOnBall));
			if (hit)
			{
				if (r.lastTouch < 0 || r.tick - r.lastTouch > 8) r.touches++;
				if (r.firstTouch < 0) r.firstTouch = r.tick;
				r.lastTouch = r.tick;
				if (st.touched < 0) st.touched = r.tick + 4;
			}
			int e = ended(r, sp.tick);
			if (e != 0) { result = e; break; }
		}
		r.car = car->GetState();
		r.grip = GetGrip(car);
		r.ball = arena->ball->GetState();
		r.pads.clear();
		for (BoostPad* p : arena->GetBoostPads()) r.pads.push_back(p->GetState());
		return r;
	}

	// Plays a found line out from its last save point (hands off: no inputs) until the shot is over, by the same end
	// rules as Run, and grades the whole rep: your frames up to the search start, the line, then the play-out. The
	// frames graded are exactly the ones the search produced, so what's shown is what was found.
	SimRun PlayOut(Arena* arena, Car* car, const std::vector<Frame>& fr, const SavePoint& f, int maxTick, const ShotReference& ref)
	{
		SimRun r;
		r.frames.assign(fr.begin(), fr.begin() + f.start);
		r.frames.insert(r.frames.end(), f.frames.begin(), f.frames.end());
		r.ctl = f.ctl;
		r.touchTick = f.firstTouch;
		ClearCarInternals(arena, car);
		SetClock(arena, f.tick);
		car->SetState(f.car);
		SetGrip(car, f.grip);
		arena->ball->SetState(f.ball);
		{
			const auto& pads = arena->GetBoostPads();
			for (size_t i = 0; i < pads.size(); i++) pads[i]->SetState(i < f.pads.size() ? f.pads[i] : BoostPadState());
		}
		const float boost0 = f.frames.empty() ? 0.0f : f.frames.front().boostAmount;
		int lastTouch = f.lastTouch, stopAt = -1;
		bool bounced = false;
		for (int i = f.tick; i < maxTick; i++)
		{
			Frame& cur = r.frames.back();
			CarControls c;
			cur.throttle = 0; cur.steer = 0; cur.pitch = 0; cur.yaw = 0; cur.roll = 0; cur.jump = false; cur.boost = false; cur.handbrake = false;
			car->controls = c;
			uint64_t t0 = arena->tickCount;
			arena->Step(1);
			CarState cs = car->GetState();
			BallState bs = arena->ball->GetState();
			bool hit = cs.ballHitInfo.isValid && cs.ballHitInfo.tickCountWhenHit >= t0;
			Frame g = SimFrame(i + 1, cs, bs, hit, cs.ballHitInfo.ballPos + cs.ballHitInfo.relativePosOnBall);
			if (hit) lastTouch = i + 1;
			r.frames.push_back(g);
			float dirY = bs.vel.y >= 0.0f ? 1.0f : -1.0f;
			float toLine = 5120.0f - bs.pos.y * dirY;
			float tLine = std::fabs(bs.vel.y) > 1.0f ? toLine / std::fabs(bs.vel.y) : 1e9f;
			bool rollingIn = std::fabs(bs.vel.y) > 300.0f && toLine > 0.0f && tLine < 6.0f && std::fabs(bs.pos.x + bs.vel.x * tLine) < 950.0f;
			if (!r.wentIn && std::fabs(bs.pos.y) > 5215.0f && std::fabs(bs.pos.x) < 893.0f && bs.pos.z < 643.0f)
			{
				r.wentIn = true; r.floorFirst = bounced; r.eventTick = i + 1; stopAt = i + 1 + 60;
				continue;
			}
			if (stopAt >= 0)
			{
				if (i + 1 >= stopAt && (r.wentIn || !rollingIn || i + 1 >= r.eventTick + 900)) break;
				continue;
			}
			if (lastTouch >= 0 && bs.pos.z < 100.0f && bs.vel.z > 0.0f && i + 1 - lastTouch > 10 && !ShotGeometry::BallWithCar(g))
			{
				bounced = true; r.eventTick = i + 1; stopAt = i + 1 + 60;
				continue;
			}
			if (lastTouch >= 0 && i + 1 - lastTouch > 480) { r.eventTick = i + 1; stopAt = i + 1 + 60; }
		}
		if (r.eventTick < 0) r.eventTick = static_cast<int>(r.frames.size()) - 1;
		r.boostUsed = boost0 - r.frames.back().boostAmount;
		r.shot = ShotGeometry::Analyze(r.frames);
		RepSegments segs = Segmenter::Split(r.frames, r.eventTick);
		r.grade = Grader::GradeRep(r.frames, segs, r.shot, r.wentIn, r.floorFirst, ref);
		return r;
	}
}
