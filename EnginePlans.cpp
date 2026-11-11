// The engine, part 4: each mechanic's list of steps for the search.
// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "EngineInternal.h"

namespace EngineInternal
{
	// ---- Each mechanic's step lists for the search (the rulebook's steps, with how to try them). Several lists =
	// several ways to play the mechanic (e.g. an air dribble with or without an extra carry touch). Empty = the
	// mechanic isn't step-searched yet, or the search starts too late for it. ----
	std::vector<std::vector<StepDef>> MechanicPlans(const ShotAnalysis& real, const std::vector<Frame>& fr, int s, float gs)
	{
		std::vector<std::vector<StepDef>> plans;
		if (!real.valid) return plans;

		if (real.type == ShotType::DoubleTap && real.setupTouchTick > s + 6)
		{
			std::vector<StepDef> steps;
			const int surface = real.tapSurface != 0 ? real.tapSurface : 1;
			// 1. Setup: send it into the backboard (the surface you used), somewhere a second touch can reach
			StepDef setup;
			setup.name = "Setup";
			setup.maxTicks = 760;
			setup.beam = 8;
			setup.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				const int horizon = std::min(300, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 6)
					for (float x : { -450.0f, 0.0f, 450.0f })
						for (float z : { 850.0f, 1150.0f })
							for (int jl : { 12, 24, 48, 84, 120 })
								for (int pw = 0; pw < 2; pw++)
								{
									AddPlan(out, sp, path, ti, Vec(x, gs * 5150.0f, z), 150.0f, jl, false, true, pw == 1);
								}
			};
			setup.ended = [surface](const SavePoint& at, int stepStart) {
				if (at.firstTouch < 0) return 0;
				if (at.touches > 1) return -1;                       // touched it again before the wall
				const int n = static_cast<int>(at.frames.size());
				if (n >= 3)
				{
					int surf = ShotGeometry::WallTapSurface(at.frames[n - 3], at.frames[n - 1]);
					if (surf == surface) return 1;
					if (surf != 0) return -4;                         // off a different surface
				}
				const Frame& f = at.frames.back();
				if (f.ballZ < 100.0f && f.ballVZ > 0.0f && at.tick - at.firstTouch > 10) return -2;   // hit the floor first
				if (at.tick - at.firstTouch > 420) return -3;   // long setups from midfield take a while to get there
				return 0;
			};
			// Your own setup first (you already know how to get it to the wall)
			setup.inputs = YourWayIn(fr, s, real.setupTouchTick);
			// After the setup touch: fly to where the ball will be a moment after it comes off the wall, lined up to hit
			// it at the goal (the "read")
			for (float after : { 0.25f, 0.45f, 0.65f })
				setup.follows.push_back([gs, after](int tick, const CarState& car, const BallState& b, Bot::Plan& f) {
					const float wallY = gs * (5120.0f - kBallR);
					if (b.vel.y * gs < 200.0f) return false;
					float t = (wallY - b.pos.y) / b.vel.y;
					if (t < 0.0f || t > 4.0f) return false;
					Vec P = b.pos + b.vel * t + Vec(0, 0, -325.0f * t * t);
					Vec vw = b.vel + Vec(0, 0, -650.0f * t);
					Vec vr(vw.x * 0.9f, -vw.y * 0.6f, vw.z * 0.9f);              // off the wall (rough bounce)
					Vec Q = P + vr * after + Vec(0, 0, -325.0f * after * after);
					if (Q.z < 140.0f) Q.z = 140.0f;
					Vec aim(0, gs * 5120.0f, 300.0f);
					Vec d = aim - Q; d = d / std::max(1.0f, d.Length());
					f.target = Q - d * 150.0f; f.hitDir = d;
					f.arriveTick = tick + static_cast<int>((t + after) * 120.0f);
					f.jumpTick = car.isOnGround ? tick : -1;
					f.dodge = false; f.doubleJump = false; f.power = false;
					return true;
				});
			setup.score = [](const SavePoint& at) {
				// A rebound you can get to: ball near the middle, not too high, car close, flip and boost left
				// High off the wall (time to read it before it drops), near the middle, car close and coming in slower than
				// the ball is coming out
				float dist = (at.ball.pos - at.car.pos).Length();
				float hi = 1.0f - std::min(2.0f, std::fabs(at.ball.pos.z - 800.0f) / 400.0f);   // high enough to read, low enough to come down
				return 1.5f * hi - dist / 1500.0f - std::fabs(at.ball.pos.x) / 1200.0f
					+ (at.car.HasFlipOrJump() ? 0.2f : 0.0f) + at.car.boost / 400.0f;
			};
			steps.push_back(setup);

			steps.push_back(ShotStep(gs, 170));
			plans.push_back(std::move(steps));
		}

		// Dribble from the start: the engine finds the flick from about 1 s out (19 of 30) but not from 2 s (2), so a long
		// carry gets its own step: a ground-dribble skill tuned to bring the ball, on the roof, to where you were half a
		// second before your flick (at your speed), then the flick (your own flick inputs from there, and the bot's shots)
#ifndef ENGINE_DRIBBLE_STEPS
#define ENGINE_DRIBBLE_STEPS 1
#endif
		// On (Oct 16): the carry, started from where the tuning lands on his dribbles, gets to the handoff on 29 of 29 in
		// the lab; handing off with the ball centered on the roof and a tuned flick, the benchmark from the setup went
		// from 2 (0) to 4 (1). The flick from the engine's own carries is the weak part (in about 12 of 27).
		if (ENGINE_DRIBBLE_STEPS && real.type == ShotType::Dribble && real.touchTick - 120 > s + 60)
		{
			// Two carry legs: to where you were 1 s before your flick (the skill gets there on 18 of 29 of your dribbles),
			// then on to half a second before it (where your own flick inputs and the bot's shots take over)
			auto onRoof = [](const Frame& f) { return f.ballZ > 110.0f && Vec(f.ballX - f.carX, f.ballY - f.carY, 0).Length() < 190.0f; };
			auto leg = [&](const char* name, int hand, int legTicks) {
				const float handLine = 5120.0f - gs * fr[hand].ballY;
				const float handSpeed = Vec(fr[hand].ballVX, fr[hand].ballVY, 0).Length();
				StepDef carry;
				carry.name = name;
				carry.maxTicks = legTicks + 240;
				carry.beam = 4;
				carry.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
				carry.ended = [gs, handLine, onRoof](const SavePoint& at, int stepStart) {
					const Frame& f = at.frames.back();
					if (at.tick - stepStart > 10 && !onRoof(f)) return -1;   // lost it off the roof
					return 5120.0f - gs * f.ballY <= handLine ? 1 : 0;
				};
				// Got there: on time (not later than you) and at your speed
				// ...with the ball sitting on the roof like yours (about 70 from the car's center; carries that ended with it on
				// the hood, ~145 out, left nothing to flick: from those the flick went in 12 of 24 vs 26 of 29 from yours)
				auto handFit = [handSpeed, legTicks](const SavePoint& at, int stepStart) {
					const float spd = Vec(at.ball.vel.x, at.ball.vel.y, 0).Length();
					const float off = Vec(at.ball.pos.x - at.car.pos.x, at.ball.pos.y - at.car.pos.y, 0).Length();
					return 10000.0f - 2.0f * std::max(0, (at.tick - stepStart) - legTicks) - std::fabs(spd - handSpeed) / 2.0f - 15.0f * std::max(0.0f, off - 90.0f);
				};
				carry.score = [handFit](const SavePoint& at) {
					const int st = at.bounds.empty() ? at.start : at.bounds.back();
					return handFit(at, st) / 1000.0f;
				};
				// Started from where the tuning landed on his dribbles (ball well forward on the roof, firm position and steering
				// gains, cruising about 1,400): from there the normal budget carries 29 of 29 in the lab (from the old start 18)
				const Knobs dmu = { 70.0f, 5.5f, 1400.0f, 0.004f, 5.5f, 0.0f, 0.3f, 1.5f };
				carry.controllers.push_back(DribbleSkill(gs, dmu));
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = dmu;
				T->sd = { 12.0f, 0.6f, 150.0f, 0.0018f, 0.9f, 180.0f, 0.18f, 0.6f };
				T->lo = { -30.0f, 0.3f, 1000.0f, 0.001f, 0.5f, -700.0f, 0.0f, 0.0f };
				T->hi = { 120.0f, 9.0f, 2300.0f, 0.03f, 10.0f, 700.0f, 1.0f, 6.0f };
				T->pop = 24; T->iters = 6;
				T->make = [gs](const Knobs& x) { return DribbleSkill(gs, x); };
				T->fitness = [handFit](const SavePoint& at) { return handFit(at, at.bounds.empty() ? at.start : at.bounds.back()); };
				T->failFitness = [gs, handLine](const SavePoint& at, int st) {
					return static_cast<float>(at.tick - st) - std::max(0.0f, 5120.0f - gs * at.ball.pos.y - handLine) / 20.0f;
				};
				carry.tune = T;
				return carry;
			};
			const int hand = real.touchTick - 60;
			StepDef carry = leg("Dribble", real.touchTick - 120, real.touchTick - 120 - s);
			StepDef carry2 = leg("Dribble 2", hand, 60);

			StepDef flick = ShotStep(gs, 150);
			flick.name = "Flick";
			// The ball rides the roof, so roof contact isn't the shot, and neither is the contact as the car jumps: the shot
			// is the touch off the flip (the car in the air with its flip used), judged a moment later
			flick.ended = [](const SavePoint& at, int stepStart) {
				const int k = static_cast<int>(at.frames.size());
				int shotAt = -1;
				for (int i = std::max(1, stepStart - at.start + 1); i < k; i++)
					if (at.frames[i].touch && !at.frames[i - 1].onGround && !at.frames[i].hasFlip) { shotAt = i; break; }
				if (shotAt < 0) return at.tick - stepStart > 150 ? -1 : 0;
				return k - 1 - shotAt >= 10 ? 1 : 0;
			};
			{
				// Your flick, played from wherever the carry handed off (your inputs from half a second before your flick)
				std::vector<CarControls> yours = YourInputs(fr);
				const int from = hand;
				flick.inputs = [yours, from](const SavePoint& sp, std::vector<std::vector<CarControls>>& out) {
					const int n = static_cast<int>(yours.size());
					for (int shift : { 0, -4, 4, -8, 8 })
					{
						std::vector<CarControls> c(sp.tick + 260);
						for (int i = sp.tick; i < static_cast<int>(c.size()); i++)
							c[i] = yours[std::clamp(from + (i - sp.tick) + shift, 0, n - 1)];
						out.push_back(std::move(c));
					}
				};
			}
			{
				// The bot's own flick, tuned from the handoff: when to pop it, the flip's direction and timing, the aim. Scored on
				// what a dribble 4 needs: in, lifted (rising 520+ off the flick) and hard (2,400+)
				auto cross = flick.crossing;
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 10.0f, 8.0f, 16.0f, -0.6f, 0.0f, 0.0f, 0.0f, 0.0f };   // (30 x 6 tries: from your own spots it flicks in on 26 of 29)
				T->sd = { 8.0f, 4.0f, 8.0f, 0.5f, 0.6f, 250.0f, 0.0f, 0.0f };
				T->lo = { 0.0f, 2.0f, 4.0f, -1.0f, -1.0f, -700.0f, 0.0f, 0.0f };
				T->hi = { 50.0f, 20.0f, 45.0f, 1.0f, 1.0f, 700.0f, 0.0f, 0.0f };
				T->pop = 30; T->iters = 6;
				T->make = [gs](const Knobs& x) { return FlickSkill(gs, x); };
				T->fitness = [cross](const SavePoint& at) {
					Vec cr;
					const float sp = at.ball.vel.Length(), lift = at.ball.vel.z;
					if (!cross(at, cr)) return -2000.0f + sp * 0.1f;
					const bool in = std::fabs(cr.x) < 892.755f - kBallR && cr.z < 642.775f - kBallR + 10.0f;
					if (!in) return -300.0f - std::max(0.0f, std::fabs(cr.x) - 800.0f) - std::max(0.0f, cr.z - 550.0f);
					return 300.0f + std::min(sp, 3000.0f) * 0.2f + std::clamp(lift, 0.0f, 900.0f) * 0.3f + (sp >= 2400.0f && lift >= 520.0f ? 1000.0f : 0.0f);
				};
				T->failFitness = [](const SavePoint& at, int) { return -3000.0f + at.ball.vel.Length() * 0.1f; };
				flick.tune = T;
			}
			plans.push_back({ carry, carry2, flick });
		}

		// Air dribbles: from before the first touch (the full chain), or from a carry already in progress (the engine
		// line started mid-carry: the carry and the tuned finish pick it up from there)
		const bool adFromStart = real.type == ShotType::AirDribble && real.airFirstTouch > s + 4;
		bool adMidCarry = false;
		if (real.type == ShotType::AirDribble && !adFromStart && s < real.touchTick - 6 && s < static_cast<int>(fr.size()))
		{
			const Frame& f0 = fr[s];
			adMidCarry = !f0.onGround && f0.ballZ > 300.0f && (Vec(f0.ballX - f0.carX, f0.ballY - f0.carY, f0.ballZ - f0.carZ)).Length() < 500.0f;
		}
		if (adFromStart || adMidCarry)
		{
			// The setup in two segments (Coop's plan, Oct 14): measured on the held-out 30, the old single setup step ended
			// at the pop off the wall (ball ~550 high, rising ~1,100), so the carry started from a pop instead of a first
			// touch, and none of its setups was a good first touch; from your own first touch the same carry and
			// finish went in high on most reps.
			// 1. Pop: your way off the wall or floor with the ball (your inputs, and the bot's own pop), until the ball's
			// off the car and rising
			StepDef setup;
			setup.name = "Pop";
			setup.maxTicks = 420;
			setup.beam = 4;   // your pops (and small changes to them) are near-copies: a few is plenty
			setup.inputs = YourWayIn(fr, s, real.airFirstTouch);
			setup.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				// The bot's own pop: a soft touch up and toward the goal
				const int horizon = std::min(240, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 6)
					for (float up : { 1200.0f, 1800.0f })
						for (int jl : { 12, 24, 48, 84 })
						{
							Vec B = path[ti];
							Vec aim(B.x * 0.5f, gs * 5120.0f, B.z + up);
							AddPlan(out, sp, path, ti, aim, 160.0f, jl, false, true, false);
						}
			};
			setup.ended = [](const SavePoint& at, int stepStart) {
				if (at.firstTouch < 0 || at.tick - at.lastTouch < 8) return 0;   // not touched yet, or still pushing it
				const Frame& f = at.frames.back();
				if ((Vec(f.ballX, f.ballY, f.ballZ) - Vec(f.carX, f.carY, f.carZ)).Length() > 900.0f) return -1;
				if (f.ballZ > 250.0f) return 1;                // off the car and up
				return at.tick - at.lastTouch > 30 ? -1 : 0;   // never got up
			};
			setup.score = [gs](const SavePoint& at) {
				// Up to about 1,750 at the top of its arc, heading toward the goal, the car close with its jump still there
				const float vz = std::max(0.0f, at.ball.vel.z);
				const float apex = at.ball.pos.z + vz * vz / 1300.0f;
				const float dist = (at.ball.pos - at.car.pos).Length();
				return -std::fabs(apex - 1750.0f) / 500.0f + std::clamp(at.ball.vel.y * gs / 1000.0f, -1.0f, 1.0f) - dist / 1200.0f
					+ (at.car.HasFlipOrJump() ? 0.3f : -1.0f) + at.car.boost / 200.0f;
			};

			// 2. First touch: off the surface and up to the ball, a soft touch from under it that leaves it high, barely
			// rising, close to the nose and moving toward the goal (your inputs from your pop, and a skill tuned for it)
			StepDef first;
			first.name = "First touch";
			first.maxTicks = 260;
			first.beam = 8;
			{
				std::vector<CarControls> yours = YourInputs(fr);
				const int touch = real.airFirstTouch;
				first.inputs = [yours, touch](const SavePoint& sp, std::vector<std::vector<CarControls>>& out) {
					if (sp.tick >= touch - 2) return;
					for (int shift : { 0, -3, 3 })
					{
						std::vector<CarControls> c = yours;
						if (shift != 0)
							for (int i = sp.tick + 1; i < std::min(static_cast<int>(yours.size()), touch + 2); i++)
								c[i].jump = yours[std::clamp(i - shift, sp.tick, static_cast<int>(yours.size()) - 1)].jump;
						out.push_back(std::move(c));
					}
				};
			}
			first.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				const int horizon = std::min(200, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 8)
					for (float up : { 500.0f, 1100.0f })
						for (int jl : { 12, 30 })
						{
							Vec B = path[ti];
							if (B.z < 900.0f) continue;   // a first touch up high, not another pop
							Vec aim(B.x * 0.5f, gs * 5120.0f, B.z + up);
							AddPlan(out, sp, path, ti, aim, 150.0f, jl, false, false, false);
						}
			};
			first.ended = [](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (f.ballZ < 150.0f) return -1;
				if (at.tick - stepStart > 40 && f.onGround && !f.onWall && f.carZ < 300.0f) return -1;   // back on the floor
				// The first touch in the air: one after the step started, with the car off every surface
				int ft = -1;
				for (int i = std::max(1, stepStart - at.start + 1); i < static_cast<int>(at.frames.size()); i++)
					if (at.frames[i].touch && !at.frames[i - 1].onGround) { ft = i; break; }
				if (ft < 0) return 0;
				if (static_cast<int>(at.frames.size()) - 1 - ft < 10) return 0;   // judge it a moment after the touch
				return (Vec(f.ballX, f.ballY, f.ballZ) - Vec(f.carX, f.carY, f.carZ)).Length() < 450.0f ? 1 : -1;
			};
			first.score = [gs](const SavePoint& at) { return FirstTouchFit(at, gs) / 500.0f; };
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 900.0f, 120.0f, 40.0f, 2.5f, 0.2f, 10.0f, 12.0f };
				T->sd = { 300.0f, 400.0f, 30.0f, 50.0f, 1.0f, 0.15f, 10.0f, 5.0f };
				T->lo = { -700.0f, 0.0f, 80.0f, -60.0f, 1.0f, 0.0f, 0.0f, 3.0f };
				T->hi = { 700.0f, 2000.0f, 200.0f, 160.0f, 6.0f, 0.6f, 50.0f, 24.0f };
				T->pop = 16;
				T->iters = 4;
				T->make = [gs](const Knobs& x) { return FirstTouchSkill(gs, x); };
				T->fitness = [gs](const SavePoint& at) { return FirstTouchFit(at, gs); };
				first.tune = T;
			}

			// Off the floor your pop is often your first air touch already: the one-step setup (into the first touch in the
			// air) is kept as its own lists for those
			StepDef setup1;
			setup1.name = "Setup";
			setup1.maxTicks = 420;
			setup1.beam = 8;
			setup1.inputs = YourWayIn(fr, s, real.airFirstTouch);
			setup1.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				// The bot's own pop: a soft touch up and toward the goal
				const int horizon = std::min(240, static_cast<int>(path.size()) - 1);
				for (int ti = 12; ti <= horizon; ti += 6)
					for (float up : { 1200.0f, 1800.0f })
						for (int jl : { 12, 24, 48, 84 })
						{
							Vec B = path[ti];
							Vec aim(B.x * 0.5f, gs * 5120.0f, B.z + up);
							AddPlan(out, sp, path, ti, aim, 160.0f, jl, false, true, false);
						}
			};
			setup1.ended = [](const SavePoint& at, int stepStart) {
				if (at.firstTouch < 0 || at.tick - at.firstTouch < 6) return 0;
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;   // still on the floor: that's a pop, not the air touch yet
				return (Vec(f.ballX, f.ballY, f.ballZ) - Vec(f.carX, f.carY, f.carZ)).Length() < 450.0f ? 1 : -1;
			};
			setup1.score = [gs](const SavePoint& at) {
				float dist = (at.ball.pos - at.car.pos).Length();
				float toward = at.ball.vel.y * gs / 1000.0f;
				// A first touch already in the air starts the chain (one touching it off the wall or ground doesn't count)
				const int ti = at.firstTouch - at.start;
				bool air = ti >= 1 && ti < static_cast<int>(at.frames.size()) && !ShotGeometry::OnSurface(at.frames[ti - 1]);
				// A good first touch (measured on Coop's 151 air dribbles: after one, the carry finishes high 51% of the
				// time, after the rest 10%): the ball up high (above 1,425) and touched soft (rising slower than ~500),
				// the car's nose up under it
				const float z = at.ball.pos.z, vz = at.ball.vel.z;
				const bool good = z > 1425.0f && vz <= 506.0f && dist < 300.0f;
				const float firstTouch = (good ? 1.0f : 0.0f) + 0.5f * std::clamp((z - 1425.0f) / 300.0f, -1.0f, 1.0f)
					- std::max(0.0f, vz - 506.0f) / 500.0f + 0.3f * at.car.rotMat.forward.z;
				return 2.0f * firstTouch - dist / 600.0f + 0.5f * std::clamp(toward, -1.0f, 1.0f) + at.car.boost / 200.0f + (air ? 0.5f : 0.0f);
			};

			// 2. Carry: another soft touch, keeping the ball close and moving toward the goal
			StepDef carry;
			carry.name = "Carry";
			carry.maxTicks = 140;
			carry.beam = 8;
			carry.plans = [gs](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
				const int horizon = std::min(100, static_cast<int>(path.size()) - 1);
				for (int ti = 6; ti <= horizon; ti += 2)
					for (float up : { 200.0f, 500.0f, 900.0f, 1400.0f })
						for (float reach : { 130.0f, 150.0f, 170.0f })
						{
							Vec B = path[ti];
							Vec aim(B.x * 0.6f, gs * 5120.0f, B.z + up);
							AddPlan(out, sp, path, ti, aim, reach, 24, false, false, false);
						}
			};
			for (float back : { 110.0f, 140.0f })
				for (float down : { 30.0f, 60.0f })
					for (float k : { 2.0f, 3.5f })
						carry.controllers.push_back(CarrySkill(gs, back, down, k));
			// ...or carried on an arc that would drop it high into the net (keeps the ball up instead of letting it sink)
			for (float back : { 110.0f, 140.0f })
				for (float k : { 2.0f, 3.5f })
					carry.controllers.push_back(CarryAim(gs, 0.0f, 515.0f, back, k));
			carry.maxTicks = 360;
			carry.ended = [gs](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;   // landed on the floor: the air dribble's over
				if (f.ballZ < 150.0f) return -1;
				float dist = (Vec(f.ballX, f.ballY, f.ballZ) - Vec(f.carX, f.carY, f.carZ)).Length();
				if (dist > 650.0f) return -1;                         // lost it
				if (at.lastTouch <= stepStart || at.tick - at.lastTouch < 8) return 0;
				// Touches in this step
				int touches = 0, last = -100;
				for (int i = std::max(0, stepStart - at.start); i < static_cast<int>(at.frames.size()); i++)
					if (at.frames[i].touch) { if (i - last > 8) touches++; last = i; }
				float toLine = 5120.0f - f.ballY * gs;
				// Carried close enough to shoot (or a long carry): the step's done
				if ((toLine < 1600.0f && dist < 400.0f) || touches >= 5) return 1;
				return 0;
			};
			carry.score = [gs](const SavePoint& at) {
				float dist = (at.ball.pos - at.car.pos).Length();
				float toward = at.ball.vel.y * gs / 1000.0f;
				return -dist / 600.0f + std::clamp(toward, -1.0f, 1.0f) + at.car.boost / 200.0f;
			};

			StepDef shot = ShotStep(gs, 120, true);
			// ...or carry it on into the shot: the same carry, pushed at the top of the net or a top corner
			for (float x : { 0.0f, 520.0f, -520.0f })
				for (float back : { 110.0f, 140.0f })
					for (float k : { 2.5f, 4.0f })
						shot.controllers.push_back(CarrySkill(gs, back, 70.0f, k, Vec(x, 0, 640.0f)));
			// Carried into a spot high in the net: the middle up high, or a top corner (the 5)
			for (float x : { 0.0f, 620.0f, -620.0f })
				for (float z : { 500.0f, 530.0f })
					for (float back : { 110.0f, 140.0f })
						for (float k : { 2.5f, 4.0f })
							shot.controllers.push_back(CarryAim(gs, x, z, back, k));
			// The shot has to come out of the carry: still in the air, no landing in between. A carry into the net keeps
			// touching it, so the step isn't over at the first gap between touches: it's over once the ball's let go
			// heading into the net (or in), or after a longer gap (the carry's done either way).
			auto cross = shot.crossing;
			shot.maxTicks = 260;
			// ...and a carry tuned from each spot to finish high in the net (a top corner scores extra: the 5)
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 450.0f, 125.0f, 40.0f, 2.5f, 0.0f, 0.0f, 0.0f };
				T->sd = { 300.0f, 300.0f, 30.0f, 50.0f, 1.0f, 0.3f, 0.0f, 0.0f };
				T->lo = { -700.0f, 0.0f, 90.0f, -60.0f, 1.0f, -0.5f, 0.0f, 0.0f };
				T->hi = { 700.0f, 1600.0f, 200.0f, 160.0f, 6.0f, 1.0f, 0.0f, 0.0f };
				T->make = [gs](const Knobs& x) { return ParamCarry(gs, x); };
				T->fitness = [cross, gs](const SavePoint& at) {
					float cx = 0, cz = 0;
					const Frame& f = at.frames.back();
					if (f.ballY * gs > 5120.0f)
					{
						bool found = false;
						for (size_t i = at.frames.size() - 1; i > 0; i--)
						{
							const Frame& a = at.frames[i - 1];
							const Frame& b = at.frames[i];
							if (a.ballY * gs < 5120.0f && b.ballY * gs >= 5120.0f)
							{
								float u = (5120.0f - a.ballY * gs) / std::max(1e-3f, (b.ballY - a.ballY) * gs);
								cx = a.ballX + (b.ballX - a.ballX) * u; cz = a.ballZ + (b.ballZ - a.ballZ) * u;
								found = true;
								break;
							}
						}
						if (!found) return -2000.0f;
					}
					else
					{
						Vec cr;
						if (!cross(at, cr)) return -2000.0f;
						cx = cr.x; cz = cr.z;
					}
					const bool in = std::fabs(cx) < 892.755f - kBallR && cz < 642.775f - kBallR + 10.0f;
					const float off = std::fabs(cz - 515.0f);
					if (!in) return -300.0f - off - std::max(0.0f, std::fabs(cx) - 800.0f);
					return 300.0f - off + (cz >= 480.0f ? 1000.0f : 0.0f) + (cz >= 480.0f && std::fabs(cx) >= 580.0f ? 300.0f : 0.0f);
				};
				shot.tune = T;
			}
			shot.ended = [cross, gs](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (at.lastTouch <= stepStart && ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;
				if (f.ballY * gs > 5120.0f + kBallR) return 1;
				if (at.lastTouch <= stepStart) return 0;
				const int since = at.tick - at.lastTouch;
				if (since < 6) return 0;
				Vec cr;
				if (cross(at, cr) && std::fabs(cr.x) < 800.0f && cr.z > 200.0f) return 1;
				return since >= 24 ? 1 : 0;
			};

			// Travel: carry it from far out to a good spot about 2,000 out for the finish (tuned from each spot toward
			// that handoff; the finish alone only works from close in)
			StepDef travel;
			travel.name = "Travel";
			travel.maxTicks = 480;
			travel.beam = 8;
			travel.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
			travel.ended = [gs](const SavePoint& at, int stepStart) {
				const Frame& f = at.frames.back();
				if (at.tick - stepStart > 30 && ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;
				if (f.ballZ < 150.0f) return -1;
				if ((Vec(f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ)).Length() > 700.0f) return -1;
				return 5120.0f - f.ballY * gs < 2200.0f ? 1 : 0;
			};
			travel.score = [gs](const SavePoint& at) { return HandoffScore(at, gs) / 1000.0f; };
			travel.controllers.push_back(ParamCarry(gs, { 0.0f, 900.0f, 125.0f, 40.0f, 2.5f, 0.0f, 0.0f, 0.0f }));
			travel.controllers.push_back(ParamCarry(gs, { 0.0f, 1300.0f, 140.0f, 60.0f, 2.0f, 0.2f, 0.0f, 0.0f }));
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 900.0f, 125.0f, 40.0f, 2.5f, 0.0f, 0.0f, 0.0f };
				T->sd = { 300.0f, 400.0f, 30.0f, 50.0f, 1.0f, 0.3f, 0.0f, 0.0f };
				T->lo = { -700.0f, 0.0f, 90.0f, -60.0f, 1.0f, -0.5f, 0.0f, 0.0f };
				T->hi = { 700.0f, 2000.0f, 200.0f, 160.0f, 6.0f, 1.0f, 0.0f, 0.0f };
				T->make = [gs](const Knobs& x) { return ParamCarry(gs, x); };
				T->fitness = [gs](const SavePoint& at) { return HandoffScore(at, gs); };
				travel.tune = T;
			}

			if (adMidCarry)
			{
				// Picked up mid-carry: straight into the tuned finish, or a carry first (to the handoff spot when it's far out)
				plans.push_back({ shot });
				plans.push_back({ carry, shot });
				if (5120.0f - gs * fr[s].ballY > 2400.0f) plans.push_back({ travel, shot });
			}
			else
			{
			// A 4 needs a real chain (3+ touches in the air): the pop, a carry to a good spot, then the finish
			plans.push_back({ setup, first, travel, shot });
			plans.push_back({ setup, first, carry, shot });
			StepDef carry2 = carry;
			carry2.name = "Carry 2";
			plans.push_back({ setup, first, carry, carry2, shot });
			const Frame& off = fr[std::max(0, real.airStart - 1)];
			if (off.onGround && !off.onWall)
			{
				plans.push_back({ setup1, travel, shot });
				plans.push_back({ setup1, carry, shot });
			}
			}
		}

		// Flip reset (yours are off the wall, under the ball near the top of its arc, upside down): your takeoff, then
		// a reset skill tuned from where you are 40 ticks before your reset, then the shot (with the flip if you used it)
		// Your inputs replay closely through the takeoff but drift during the climb (about 140 off by 40 ticks before
		// your reset), so the hand-off to the reset skill is tried at three points, each its own list
		// Off for now (Oct 14): each piece works from your recorded states (the reset skill gets 27 of 30 from 40
		// ticks out, the flip shot scores about half the time after your resets), but chained in the search the
		// engine's own resets leave the ball where the shot can't follow yet
#ifndef ENGINE_FR_STEPS
#define ENGINE_FR_STEPS 0
#endif
		constexpr bool kFlipResetSteps = ENGINE_FR_STEPS != 0;
		std::vector<int> frHandoffs;
		if (kFlipResetSteps && real.type == ShotType::FlipReset && real.resetTick >= 0 && real.resetTakeoff >= 0)
			for (int h : { real.resetTakeoff + 20, real.resetTakeoff + 70, real.resetTick - 40, real.resetTick - 20 })
				if (h > s + 6 && h < real.resetTick - 10 && (frHandoffs.empty() || h > frHandoffs.back() + 15)) frHandoffs.push_back(h);
		for (const int frHandoff : frHandoffs)
		{
			StepDef setup;
			setup.name = "Setup";
			setup.maxTicks = frHandoff - s + 2;
			setup.beam = 8;
			setup.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
			setup.inputs = YourWayIn(fr, s, frHandoff);
			for (float k : { 3.0f, 6.0f }) setup.controllers.push_back(FollowYours(fr, k));
			setup.ended = [frHandoff](const SavePoint& at, int) {
				if (at.tick < frHandoff) return 0;
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f)) return -1;   // still on the wall
				return (Vec(f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ)).Length() < 700.0f ? 1 : -1;
			};
			setup.score = [](const SavePoint& at) { return -(at.ball.pos - at.car.pos).Length() / 500.0f; };

			StepDef reset;
			reset.name = "Reset";
			reset.maxTicks = std::max(180, real.resetTick - frHandoff + 160);   // the climb from an early hand-off takes a while
			reset.beam = 8;
			reset.plans = [](const SavePoint&, const std::vector<Vec>&, std::vector<PlanSpec>&) {};
			reset.ended = [](const SavePoint& at, int stepStart) {
				// Over once the reset happened and the car is off the ball again (the shot starts clear of it)
				const int k = static_cast<int>(at.frames.size());
				const Frame& f = at.frames.back();
				if (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) return -1;
				int resetAt = -1;
				for (int i = std::max(1, stepStart - at.start + 1); i < k; i++)
					if (ShotGeometry::IsFlipReset(at.frames[i - 1], at.frames[i])) { resetAt = i; break; }
				if (resetAt < 0) return 0;
				return k - resetAt >= 12 ? 1 : 0;                // a moment on: the shot takes it from here
			};
			auto afterReset = [gs](const SavePoint& at) {
				// A reset you can shoot from: soft (the ball keeps going where it was going, toward the goal), the ball
				// above the car and still close, high enough to have time
				float dist = (at.ball.pos - at.car.pos).Length();
				// Measured on the held-out 30 (Oct 14): resets a flip shot can score from have the ball near the top of its
				// arc (falling slower than ~250), moving at the goal, closer in, with boost left; the engine's own resets
				// came late (ball dropping ~460) with half the boost gone
				const float vz = at.ball.vel.z, toLine = 5120.0f - at.ball.pos.y * gs;
				return std::clamp(at.ball.vel.y * gs, -600.0f, 1500.0f) / 500.0f - dist / 600.0f
					+ std::clamp((at.ball.pos.z - at.car.pos.z) / 100.0f, -1.0f, 1.0f) + std::clamp((at.ball.pos.z - 700.0f) / 600.0f, -1.0f, 1.0f)
					- std::max(0.0f, -vz - 250.0f) / 300.0f - std::max(0.0f, vz - 200.0f) / 300.0f + at.car.boost / 200.0f
					- std::clamp((toLine - 2000.0f) / 2000.0f, 0.0f, 1.0f);
			};
			// When the reset came: as early as yours is the aim (from your own spots the skill matches your timing on 54 of
			// 60 tries when it's scored for it; scored only on the state after it, its resets came ~70 ticks late with the
			// ball already dropping fast, which the shot couldn't follow)
			const int yourReset = real.resetTick;
			auto resetLate = [yourReset](const SavePoint& at) {
				for (int i = static_cast<int>(at.frames.size()) - 1; i > 0; i--)
					if (ShotGeometry::IsFlipReset(at.frames[i - 1], at.frames[i]))
						return std::max(0.0f, static_cast<float>(at.start + i - yourReset)) + std::max(0.0f, -at.frames[i].ballVZ - 200.0f) / 10.0f;
				return 200.0f;
			};
			reset.score = [afterReset, resetLate](const SavePoint& at) { return afterReset(at) - resetLate(at) / 30.0f; };
			const Knobs rmu = { 0.0f, 0.0f, 3.0f, 250.0f, 150.0f, 0.15f, 150.0f, 100.0f };
			reset.controllers.push_back(ResetSkill(gs, rmu));
			reset.controllers.push_back(ResetSkill(gs, { 0.4f, 0.0f, 3.0f, 250.0f, 150.0f, 0.15f, 150.0f, 100.0f }));
			reset.controllers.push_back(ResetSkill(gs, { 0.0f, 0.0f, 4.5f, 120.0f, 250.0f, 0.1f, 250.0f, 0.0f }));
			{
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = rmu;
				T->sd = { 0.5f, 0.4f, 1.2f, 150.0f, 150.0f, 0.12f, 120.0f, 250.0f };
				T->lo = { -1.5f, -1.2f, 0.8f, 0.0f, -200.0f, 0.0f, 0.0f, -400.0f };
				T->hi = { 1.5f, 1.2f, 7.0f, 800.0f, 600.0f, 0.4f, 450.0f, 700.0f };
				T->pop = 24; T->iters = 6;
				T->make = [gs](const Knobs& x) { return ResetSkill(gs, x); };
				T->fitness = [afterReset, resetLate](const SavePoint& at) { return 1000.0f + 100.0f * afterReset(at) - 5.0f * resetLate(at); };
				T->failFitness = [](const SavePoint& at, int st) { return -ClosestFrom(at, st); };
				reset.tune = T;
			}

			StepDef shot = ShotStep(gs, 240);
			// The car may still be sitting on the ball after the reset: touches only count once it's come off it
			// (and, if you shot off the flip, once it has flipped)
			const bool needFlip = real.flipShot;
			shot.ended = [needFlip](const SavePoint& at, int stepStart) {
				const int k = static_cast<int>(at.frames.size());
				const int from = std::max(1, stepStart - at.start);
				// The touch that counts: the first after the flip (if you shot off the flip), else the first after the car
				// has come off the ball (four ticks without touching it)
				int after = -1;
				if (needFlip)
				{
					for (int i = from; i < k; i++) if (at.frames[i].jump && !at.frames[i - 1].jump && !at.frames[i - 1].onGround) { after = i; break; }
				}
				else
				{
					int free = 0;
					for (int i = from; i < k; i++) { free = at.frames[i].touch ? 0 : free + 1; if (free >= 4) { after = i; break; } }
				}
				const Frame& f = at.frames.back();
				if (after < 0) return (ShotGeometry::OnSurface(f) && f.carZ < 300.0f) ? -1 : 0;
				int shotAt = -1;
				for (int i = after; i < k; i++) if (at.frames[i].touch) { shotAt = i; break; }
				if (shotAt < 0) return k - after > 80 ? -1 : 0;
				return k - 1 - shotAt >= 6 ? 1 : 0;
			};
			{
				auto cross = shot.crossing;
				auto T = std::make_shared<StepDef::Tune>();
				T->mu = { 0.0f, 450.0f, 180.0f, 2.5f, 260.0f, 0.1f, 0.0f, 0.7f };
				T->sd = { 350.0f, 200.0f, 60.0f, 1.0f, 80.0f, 0.1f, 0.3f, 0.3f };
				T->lo = { -800.0f, 100.0f, 80.0f, 0.8f, 150.0f, 0.0f, -1.0f, 0.0f };
				T->hi = { 800.0f, 900.0f, 400.0f, 6.0f, 450.0f, 0.4f, 1.0f, 1.0f };
				T->pop = 30; T->iters = 6;
				T->make = [gs](const Knobs& x) { return FlipShot(gs, x); };
				T->failFitness = [](const SavePoint& at, int st) { return -1000.0f - ClosestFrom(at, st + 12); };
				T->fitness = [cross](const SavePoint& at) {
					Vec cr;
					if (!cross(at, cr)) return -2000.0f + std::clamp(at.ball.vel.y, -2000.0f, 2000.0f) * 0.0f;
					const bool in = std::fabs(cr.x) < 892.755f - kBallR && cr.z < 642.775f - kBallR + 10.0f;
					const float sp = at.ball.vel.Length();
					if (!in) return -300.0f - std::fabs(cr.z - 520.0f) - std::max(0.0f, std::fabs(cr.x) - 800.0f);
					return 300.0f - std::fabs(cr.z - 520.0f) + (cr.z >= 450.0f && sp >= 1700.0f ? 1000.0f : 0.0f)
						+ (cr.z >= 560.0f && sp >= 2400.0f ? 300.0f : 0.0f) + std::min(sp, 3000.0f) * 0.05f;
				};
				shot.tune = T;
			}
			{
				// You shot off the flip, so the line does too: only the plans that dodge into the ball
				auto all = shot.plans;
				shot.plans = [all](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
					std::vector<PlanSpec> tmp;
					all(sp, path, tmp);
					for (PlanSpec& p : tmp) if (p.dodge) out.push_back(std::move(p));
				};
			}
			plans.push_back({ setup, reset, shot });
		}
		return plans;
	}
}
