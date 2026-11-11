// The engine, part 3: the step search itself, re-running a found line, and the pieces the mechanics' steps share.
// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "EngineInternal.h"

namespace EngineInternal
{
	// Where the ball goes from a state on its own (nobody touches it)
	std::vector<Vec> BallPathFrom(Arena* ballArena, const BallState& b, int ticks)
	{
		std::vector<Vec> path;
		path.reserve(ticks + 1);
		// The ball remembers its last contacts with walls: a tick parked in mid-air first, so the path never depends on
		// what this arena predicted before (each core has its own, so it has to come out the same on any of them)
		BallState park;
		park.pos = Vec(2500, 0, 1400);
		ballArena->ball->SetState(park);
		ballArena->Step(1);
		ballArena->ball->SetState(b);
		path.push_back(b.pos);
		for (int i = 0; i < ticks; i++) { ballArena->Step(1); path.push_back(ballArena->ball->GetState().pos); }
		return path;
	}

	// A bot plan from any save point: meet the ball (on its own path from there) at tick `base + ti` and send it
	// toward aimPt (a point the ball's center should pass through: the goal, or a spot on the backboard)
	bool PlanFrom(const SavePoint& sp, const std::vector<Vec>& ballPath, int ti, Vec aimPt, float reach, int jumpLead,
		bool dodge, bool doubleJump, bool power, Bot::Plan& plan)
	{
		const int horizon = static_cast<int>(ballPath.size()) - 1;
		if (ti < 6 || ti > horizon) return false;
		Vec B = ballPath[ti];
		Vec A = aimPt;
		float flight = (A - B).Length() / 2000.0f;
		A.z += 0.5f * 650.0f * flight * flight;
		Vec d = A - B;
		float dl = d.Length();
		if (dl < 1) return false;
		d = d / dl;
		Vec C = B - d * reach;
		const bool onWall = sp.car.isOnGround && sp.car.rotMat.up.z < 0.7f;   // on a wall (or the ceiling): jump off it
		const bool startAir = !sp.car.isOnGround || onWall;
		bool ground = C.z < 60.0f && !startAir;
		if (ground)
		{
			Vec dg = d; dg.z = 0;
			if (dg.Length() < 0.1f) return false;
			dg = dg / dg.Length();
			C = B - dg * reach;
			C.z = 17.0f;
		}
		float tau = ti / 120.0f;
		const Vec p0 = sp.car.pos;
		const Vec v0 = onWall ? sp.car.vel + sp.car.rotMat.up * 300.0f : sp.car.vel;   // the jump pushes you off the wall
		if (ground)
		{
			Vec h = C - p0; h.z = 0;
			if (h.Length() / tau > 2300.0f) return false;
		}
		else
		{
			Vec drift = p0 + v0 * tau + Vec(0, 0, -0.5f * 650.0f * tau * tau);
			Vec need = C - drift;
			// Close in (the ball is coming right at you, e.g. off the backboard): no time to move, just turn and hit it
			bool closeIn = (B - drift).Length() < 260.0f && ti <= 40;
			if (!closeIn && 2.0f * need.Length() / (tau * tau) > 1250.0f) return false;
		}
		plan.target = C;
		plan.hitDir = d;
		plan.arriveTick = sp.tick + ti;
		plan.jumpTick = onWall ? sp.tick : (ground || startAir) ? -1 : std::max(sp.tick, sp.tick + ti - jumpLead);
		if (!ground && !startAir && plan.jumpTick < 0) plan.jumpTick = sp.tick;
		plan.dodge = dodge;
		plan.doubleJump = doubleJump && !dodge;
		plan.dodgeLead = ground ? 14 : 8;
		plan.power = power;
		return true;
	}

	bool AddPlan(std::vector<PlanSpec>& out, const SavePoint& sp, const std::vector<Vec>& path, int ti, Vec aim, float reach, int jl,
		bool dodge, bool dj, bool power)
	{
		PlanSpec ps;
		if (!PlanFrom(sp, path, ti, aim, reach, jl, dodge, dj, power, ps.plan)) return false;
		ps.aimPt = aim; ps.ti = ti; ps.jl = jl; ps.reach = reach; ps.dodge = dodge; ps.dj = dj; ps.power = power;
		out.push_back(ps);
		return true;
	}

	bool AddLiftPlan(std::vector<PlanSpec>& out, const SavePoint& sp, const std::vector<Vec>& path, int ti, const Vec& T, float reach, int jl,
		bool dodge, bool power)
	{
		if (!AddPlan(out, sp, path, ti, LiftAim(path, ti, T), reach, jl, dodge, false, power)) return false;
		out.back().lift = true;
		out.back().liftT = T;
		return true;
	}

	// The search itself. budget = sim runs it may spend across all steps. Returns the survivors of the last step.
	// (From a set of save points, starting at step 'from': lists that share their first steps search them once.)
	std::vector<SavePoint> StepSearchFrom(Arena* arena, Car* car, Arena* ballArena, std::vector<SavePoint> frontier, size_t from, const std::vector<StepDef>& steps,
		int budget, std::mt19937& rng, const std::function<bool()>& stop, int& runs, std::vector<int>* passedPerStep, const Workers* wk)
	{
		if (passedPerStep && passedPerStep->size() != steps.size()) passedPerStep->assign(steps.size(), 0);
		for (size_t k = from; k < steps.size() && !frontier.empty(); k++)
		{
			const StepDef& st = steps[k];
			const int share = std::max(20, budget / static_cast<int>(steps.size() - from) / static_cast<int>(frontier.size()));
			std::vector<SavePoint> next;
			for (int fi = 0; fi < static_cast<int>(frontier.size()); fi++)
			{
				const SavePoint& sp = frontier[fi];
				std::vector<Vec> path = BallPathFrom(ballArena, sp.ball, 360);
				std::vector<PlanSpec> cands;
				st.plans(sp, path, cands);
				if (kStepDebug) fprintf(stderr, "step %zu: %zu candidate plans from tick %d\n", k, cands.size(), sp.tick);
				std::shuffle(cands.begin(), cands.end(), rng);
				if (static_cast<int>(cands.size()) > share) cands.resize(share);
				// Every try from this save point (the plans, your inputs with each follow-up, the step's controllers) is
				// independent: they run in parallel batches, and the results are taken in the same order as one by one
				Bot::Plan none;
				none.arriveTick = sp.tick + st.maxTicks + 1000;
				std::vector<std::vector<CarControls>> ins;
				if (st.inputs) st.inputs(sp, ins);
				std::vector<std::shared_ptr<std::vector<CarControls>>> inPtrs;
				for (const auto& in0 : ins) inPtrs.push_back(std::make_shared<std::vector<CarControls>>(in0));
				const int nf = static_cast<int>(st.follows.size()) + (st.afterTouch ? 1 : 0);
				struct Job { int kind; int i; int follow; };   // kind 0 a plan, 1 your inputs, 2 a controller
				std::vector<Job> jobs;
				for (int i = 0; i < static_cast<int>(cands.size()); i++) jobs.push_back({ 0, i, -1 });
				for (int i = 0; i < static_cast<int>(inPtrs.size()); i++) for (int f = 0; f <= nf; f++) jobs.push_back({ 1, i, f });
				for (int i = 0; i < static_cast<int>(st.controllers.size()); i++) jobs.push_back({ 2, i, -1 });
				auto runJob = [&](const Job& j, Arena* A, Car* C, int& res) -> SavePoint {
					if (j.kind == 0)
					{
						const AfterTouch* at = !st.follows.empty() ? &st.follows[j.i % st.follows.size()] : st.afterTouch ? &st.afterTouch : nullptr;
						return RunFrom(A, C, sp, cands[j.i].plan, st.maxTicks, st.ended, res, at);
					}
					if (j.kind == 1)
					{
						const AfterTouch* at = j.follow == 0 ? nullptr : j.follow <= static_cast<int>(st.follows.size()) ? &st.follows[j.follow - 1] : &st.afterTouch;
						return RunFrom(A, C, sp, none, st.maxTicks, st.ended, res, at, inPtrs[j.i].get());
					}
					return RunFrom(A, C, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[j.i]);
				};
				for (size_t j0 = 0; j0 < jobs.size() && !stop(); j0 += kBatch)
				{
					const int cnt = static_cast<int>(std::min(jobs.size() - j0, static_cast<size_t>(kBatch)));
					std::vector<SavePoint> rs(cnt);
					std::vector<int> resv(cnt, -1);
					RunOn(wk, cnt, [&](int b, int w) {
						rs[b] = runJob(jobs[j0 + b], wk ? wk->A(w) : arena, wk ? wk->C(w) : car, resv[b]);
					});
					for (int b = 0; b < cnt; b++)
					{
						const Job& j = jobs[j0 + b];
						SavePoint& r = rs[b];
						const int res = resv[b];
						runs++;
						if (kStepDebug) { gStepDbg[k * 8 + (r.firstTouch < 0 ? 0 : res == 1 ? 1 : res < 0 ? 1 - res : 7)]++; }
						if (kStepDebug && std::getenv("ENG_JOBDBG")) fprintf(stderr, "J k%zu fi%d kind%d i%d f%d res%d tick%d ball %.3f %.3f %.3f car %.3f %.3f %.3f\n", k, fi, j.kind, j.i, j.follow, res, r.tick, r.ball.pos.x, r.ball.pos.y, r.ball.pos.z, r.car.pos.x, r.car.pos.y, r.car.pos.z);
						if (res != 1) continue;
						SavePoint::ChainStep cs;
						if (j.kind == 0)
						{
							r.score = st.score(r);
							r.spec = cands[j.i]; r.viaPlan = true;
							cs.viaPlan = true; cs.spec = cands[j.i];
							cs.follow = !st.follows.empty() ? j.i % static_cast<int>(st.follows.size()) : (st.afterTouch ? 99 : -1);
						}
						else if (j.kind == 1)
						{
							r.score = st.score(r) + 0.05f;   // a nudge toward your own way in when it's as good
							r.note = "your setup, a better second touch";
							cs.viaPlan = false; cs.inputs = inPtrs[j.i];
							cs.follow = j.follow == 0 ? -1 : j.follow <= static_cast<int>(st.follows.size()) ? j.follow - 1 : 99;
						}
						else
						{
							r.score = st.score(r);
							cs.ctrl = j.i;
						}
						r.parent = fi;
						r.chain.push_back(cs);
						next.push_back(std::move(r));
					}
				}
				if (st.tune && !stop())
				{
					const StepDef::Tune& T = *st.tune;
					Knobs mu = T.mu, sd = T.sd;
					std::mt19937 trng(static_cast<unsigned>(sp.tick) * 2654435761u + static_cast<unsigned>(fi));
					std::normal_distribution<float> N01(0.0f, 1.0f);
					struct Cand { float f; Knobs x; SavePoint r; };
					std::vector<Cand> best;   // the best few seen, distinct settings
					for (int it = 0; it < T.iters && !stop(); it++)
					{
						std::vector<std::pair<float, Knobs>> sc;
						for (int k0 = 0; k0 < T.pop && !stop(); k0 += kBatch)
						{
							const int cnt = std::min(kBatch, T.pop - k0);
							std::vector<Knobs> xs(cnt);
							for (int b = 0; b < cnt; b++) for (int j = 0; j < kCarryKnobs; j++) xs[b][j] = std::clamp(mu[j] + sd[j] * N01(trng), T.lo[j], T.hi[j]);
							std::vector<SavePoint> rs(cnt);
							std::vector<int> resv(cnt, -1);
							RunOn(wk, cnt, [&](int b, int w) {
								Controller ctl = T.make(xs[b]);
								rs[b] = RunFrom(wk ? wk->A(w) : arena, wk ? wk->C(w) : car, sp, none, st.maxTicks, st.ended, resv[b], nullptr, nullptr, &ctl);
							});
							for (int b = 0; b < cnt; b++)
							{
								SavePoint& r = rs[b];
								const int res = resv[b];
								runs++;
								if (kStepDebug && std::getenv("ENG_JOBDBG")) fprintf(stderr, "T k%zu fi%d it%d b%d res%d tick%d ball %.3f %.3f %.3f car %.3f %.3f %.3f\n", k, fi, it, k0 + b, res, r.tick, r.ball.pos.x, r.ball.pos.y, r.ball.pos.z, r.car.pos.x, r.car.pos.y, r.car.pos.z);
								float f = res == 1 ? T.fitness(r) : T.failFitness ? -1e5f + T.failFitness(r, sp.tick) : -1e6f;
								sc.push_back({ f, xs[b] });
								if (res == 1 && (best.size() < static_cast<size_t>(T.keep) || f > best.back().f))
								{
									best.push_back({ f, xs[b], std::move(r) });
									std::sort(best.begin(), best.end(), [](const Cand& a, const Cand& b2) { return a.f > b2.f; });
									if (best.size() > static_cast<size_t>(T.keep)) best.pop_back();
								}
							}
						}
						if (sc.empty()) break;
						std::sort(sc.begin(), sc.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
						const int el = std::max(2, static_cast<int>(sc.size()) / 5);
						for (int j = 0; j < kCarryKnobs; j++)
						{
							float m = 0, v = 0;
							for (int e = 0; e < el; e++) m += sc[e].second[j];
							m /= el;
							for (int e = 0; e < el; e++) v += (sc[e].second[j] - m) * (sc[e].second[j] - m);
							mu[j] = m;
							sd[j] = std::max(std::sqrt(v / el), (T.hi[j] - T.lo[j]) * 0.02f);
						}
					}
					for (Cand& c : best)
					{
						SavePoint r = std::move(c.r);
						r.score = st.score(r) + c.f * 0.001f;
						r.parent = fi;
						r.note = st.name == std::string("Travel") ? "carried it in" : st.name == std::string("First touch") ? "a better first touch" : "carried in high";
						SavePoint::ChainStep cs;
						cs.tuned = std::make_shared<Controller>(T.make(c.x));
						r.chain.push_back(cs);
						next.push_back(std::move(r));
					}
				}
				if (stop()) break;
			}
			// Re-aim the best few (the shooting method): where did the ball really cross the goal line vs where the plan
			// aimed it? Aim off by that much the other way and play the step again from the same save point.
			if (st.crossing && st.aimFix > 0 && !next.empty())
			{
				std::sort(next.begin(), next.end(), [](const SavePoint& a, const SavePoint& b) { return a.score > b.score; });
				const int top = std::min<int>(st.aimFix, static_cast<int>(next.size()));
				std::vector<SavePoint> fixed;
				for (int i = 0; i < top && !stop(); i++)
				{
					if (!next[i].viaPlan || next[i].parent < 0) continue;
					SavePoint cur = next[i];
					const SavePoint& par = frontier[cur.parent];
					std::vector<Vec> path = BallPathFrom(ballArena, par.ball, 360);
					for (int iter = 0; iter < 3 && !stop(); iter++)
					{
						Vec cr;
						if (!st.crossing(cur, cr)) break;
						const Vec want = cur.spec.lift ? cur.spec.liftT : cur.spec.aimPt;
						Vec err(cr.x - want.x, 0, cr.z - want.z);
						if (err.Length() < 30.0f) break;
						PlanSpec ps = cur.spec;
						std::vector<PlanSpec> one;
						if (ps.lift)
						{
							// Aim the push at a spot moved by the miss (the shooting method, on the target)
							if (!AddLiftPlan(one, par, path, ps.ti, ps.liftT - err, ps.reach, ps.jl, ps.dodge, ps.power)) break;
						}
						else if (!AddPlan(one, par, path, ps.ti, ps.aimPt - err, ps.reach, ps.jl, ps.dodge, ps.dj, ps.power)) break;
						int res = -1;
						SavePoint r = RunFrom(arena, car, par, one[0].plan, st.maxTicks, st.ended, res, st.afterTouch ? &st.afterTouch : nullptr);
						runs++;
						if (res != 1) break;
						r.score = st.score(r);
						r.spec = one[0]; r.viaPlan = true; r.parent = cur.parent;
						{
							SavePoint::ChainStep cs; cs.viaPlan = true; cs.spec = one[0]; cs.follow = st.afterTouch ? 99 : -1;
							r.chain.push_back(cs);
						}
						cur = r;
						fixed.push_back(r);
					}
				}
				for (SavePoint& f : fixed) next.push_back(std::move(f));
			}
			if (passedPerStep) (*passedPerStep)[k] = static_cast<int>(next.size());
			std::sort(next.begin(), next.end(), [](const SavePoint& a, const SavePoint& b) { return a.score > b.score; });
			// Keep the best few, but not near-copies of each other (same end tick and ball spot)
			std::vector<SavePoint> keep;
			for (SavePoint& c : next)
			{
				bool dup = false;
				for (const SavePoint& q : keep)
					if (std::abs(q.tick - c.tick) <= 2 && (q.ball.pos - c.ball.pos).Length() < 60.0f && (q.ball.vel - c.ball.vel).Length() < 150.0f) { dup = true; break; }
				if (!dup) keep.push_back(std::move(c));
				if (static_cast<int>(keep.size()) >= (k + 1 == steps.size() ? st.beam * 5 : st.beam)) break;
			}
			frontier = std::move(keep);
		}
		return frontier;
	}

	std::vector<SavePoint> StepSearch(Arena* arena, Car* car, Arena* ballArena, const SavePoint& start, const std::vector<StepDef>& steps,
		int budget, std::mt19937& rng, const std::function<bool()>& stop, int& runs, std::vector<int>* passedPerStep, const Workers* wk)
	{
		if (passedPerStep) passedPerStep->assign(steps.size(), 0);
		return StepSearchFrom(arena, car, ballArena, { start }, 0, steps, budget, rng, stop, runs, passedPerStep, wk);
	}

	// Plays a found line again, closed-loop, from a nudged start with the timing shifted: the bot re-aims each step's
	// plan from wherever the car really is, and your own inputs are shifted. Returns the controls it ended up using
	// (empty if a step broke), so the caller can replay and grade them exactly.
	bool RerunChain(Arena* arena, Car* car, const std::vector<Frame>& fr, int s, const SavePoint& line,
		const std::vector<StepDef>& steps, int nudge, int shift, SavePoint& endAt)
	{
		SavePoint sp = StartPoint(arena, car, fr, s);
		if (nudge > 0)
		{
			// The same nudge Run() applies, so the replay that grades it starts from the exact same spot
			std::mt19937 rng(static_cast<unsigned>(nudge) * 7919u);
			std::uniform_real_distribution<float> u(-1.0f, 1.0f);
			sp.car.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			sp.car.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
			sp.ball.pos += Vec(u(rng), u(rng), u(rng)) * 2.0f;
			sp.ball.vel += Vec(u(rng), u(rng), u(rng)) * 8.0f;
		}
		static thread_local std::unique_ptr<Arena> ballArena;
		if (!ballArena) ballArena.reset(Arena::Create(GameMode::SOCCAR));
		for (size_t k = 0; k < line.chain.size() && k < steps.size(); k++)
		{
			const SavePoint::ChainStep& cs = line.chain[k];
			const StepDef& st = steps[k];
			const bool last = k + 1 == line.chain.size();
			const AfterTouch* at = cs.follow < 0 ? nullptr : cs.follow == 99 ? (st.afterTouch ? &st.afterTouch : nullptr)
				: cs.follow < static_cast<int>(st.follows.size()) ? &st.follows[cs.follow] : nullptr;
			Bot::Plan none;
			none.arriveTick = sp.tick + st.maxTicks + 1000;
			// The ways to play this step again: as found (re-planned from where things are now), and on the last step
			// a few small adjustments (a touch earlier or later, the other carry styles), like a player adjusting the shot
			std::vector<std::function<SavePoint(int&)>> tries;
			if (cs.tuned)
			{
				tries.push_back([&, c = cs.tuned](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, c.get()); });
				for (int ci = 0; ci < static_cast<int>(st.controllers.size()); ci++)
					tries.push_back([&, ci](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
			}
			else if (cs.ctrl >= 0 && cs.ctrl < static_cast<int>(st.controllers.size()))
			{
				tries.push_back([&, ci = cs.ctrl](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
				// A player adjusts the carry to how the touches are really going: the other carry styles are fair game
				for (int ci = 0; ci < static_cast<int>(st.controllers.size()); ci++)
						if (ci != cs.ctrl) tries.push_back([&, ci](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
			}
			else if (cs.viaPlan)
			{
				std::vector<Vec> path = BallPathFrom(ballArena.get(), sp.ball, 360);
				for (int dt : { 0, -3, 3, -6, 6 })
				{
					PlanSpec again;
					Bot::Plan p = cs.spec.plan;
					const int ti2 = std::max(4, cs.spec.ti + dt);
					const Vec aim2 = cs.spec.lift ? LiftAim(path, ti2, cs.spec.liftT) : cs.spec.aimPt;
					if (PlanFrom(sp, path, ti2, aim2, cs.spec.reach, cs.spec.jl, cs.spec.dodge, cs.spec.dj, cs.spec.power, again.plan)) p = again.plan;
					else if (dt != 0) continue;
					if (p.jumpTick >= 0) p.jumpTick = std::max(sp.tick, p.jumpTick + shift);
					if (p.dodge) p.dodgeLead = std::max(2, p.dodgeLead - shift);
					tries.push_back([&, p](int& res) { return RunFrom(arena, car, sp, p, st.maxTicks, st.ended, res, at); });
				}
				// ...or, where the step has carry skills, a player adjusting with one of those (as for a skill's step)
				if (!last)
					for (int ci = 0; ci < static_cast<int>(st.controllers.size()); ci++)
						tries.push_back([&, ci](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, nullptr, nullptr, &st.controllers[ci]); });
			}
			else
			{
				// Your own part is replayed as you did it (from the nudged start); the timing shift tests the engine's part
				const std::vector<CarControls>* c = cs.inputs.get();
				tries.push_back([&, c](int& res) { return RunFrom(arena, car, sp, none, st.maxTicks, st.ended, res, at, c); });
			}
			bool ok = false;
			SavePoint best;
			float bestScore = -1e9f;
			for (auto& t : tries)
			{
				int res = -1;
				SavePoint r = t(res);
				if (res != 1) continue;
				float sc = st.score(r);
				if (!ok || sc > bestScore) { best = std::move(r); bestScore = sc; ok = true; }
				if (!last) break;   // earlier steps: the first that works
			}
			if (!ok) { if (kStepDebug) fprintf(stderr, "rerun nudge %d shift %d: step %zu (%s) failed\n", nudge, shift, k, st.name); return false; }
			sp = std::move(best);
		}
		endAt = std::move(sp);
		return true;
	}

	// Where the ball is headed (straight line with gravity, no bounces): is it on target, how high, how hard
	void AimBall(const BallState& b, float gs, float& onTarget, float& height, float& speed)
	{
		Vec p = b.pos, v = b.vel;
		if (gs < 0) { p = Vec(-p.x, -p.y, p.z); v = Vec(-v.x, -v.y, v.z); }
		onTarget = 0; height = 0; speed = v.Length();
		if (v.y < 100.0f) return;
		float t = (5120.0f - p.y) / v.y;
		if (t > 3.0f) return;
		float x = p.x + v.x * t, z = std::max(93.0f, p.z + v.z * t - 325.0f * t * t);
		float miss = std::hypot(std::max(0.0f, std::fabs(x) - 780.0f), std::max(0.0f, z - 530.0f));
		onTarget = 1.0f - std::min(1.0f, miss / 200.0f);   // a near miss is still a miss
		height = z;
	}

	// Your recorded inputs, by tick
	std::vector<CarControls> YourInputs(const std::vector<Frame>& fr)
	{
		std::vector<CarControls> yours(fr.size());
		for (size_t i = 0; i < fr.size(); i++)
		{
			const Frame& f = fr[i];
			CarControls& c = yours[i];
			c.throttle = f.throttle; c.steer = f.steer; c.pitch = f.pitch; c.yaw = f.yaw; c.roll = f.roll;
			c.jump = f.jump; c.boost = f.boost; c.handbrake = f.handbrake;
		}
		return yours;
	}

	// Your way into a touch, as you did it and reshaped a little: jumps a couple of ticks earlier or later, the nose a
	// touch up/down/left/right into the touch, or less boost right before it. Only from the start of the search.
	std::function<void(const SavePoint&, std::vector<std::vector<CarControls>>&)> YourWayIn(const std::vector<Frame>& fr, int s, int touch)
	{
		std::vector<CarControls> yours = YourInputs(fr);
		return [yours, touch, s](const SavePoint& sp, std::vector<std::vector<CarControls>>& out) {
			if (sp.tick != s) return;
			const int n = static_cast<int>(yours.size());
			for (int shift : { 0, -2, 2, -4, 4 })
				for (int v = 0; v < 6; v++)
				{
					std::vector<CarControls> c = yours;
					if (shift != 0)
						for (int i = s + 1; i < std::min(n, touch + 2); i++)
						{
							int j = std::clamp(i - shift, s, n - 1);
							c[i].jump = yours[j].jump;
						}
					const int w0 = std::max(s, touch - 30), w1 = std::min(n, touch + 4);
					for (int i = w0; i < w1; i++)
					{
						if (v == 1) c[i].pitch = Bot::Clamp1(c[i].pitch + 0.15f);
						if (v == 2) c[i].pitch = Bot::Clamp1(c[i].pitch - 0.15f);
						if (v == 3) c[i].yaw = Bot::Clamp1(c[i].yaw + 0.15f);
						if (v == 4) c[i].yaw = Bot::Clamp1(c[i].yaw - 0.15f);
						if (v == 5 && i >= touch - 12) c[i].boost = false;
					}
					out.push_back(std::move(c));
				}
		};
	}

	// Your way up, closed-loop: your own inputs while you're on the wall or floor and through your jump, then the car
	// flies along the path you flew (your position, speed and car angle at each tick), correcting as it goes. Played
	// open-loop, your inputs drift about 140 off by 40 ticks before a flip reset; followed, the car stays on your path
	// and reaches the ball when you did. k: how hard it pulls back onto your path.
	Controller FollowYours(const std::vector<Frame>& fr, float k)
	{
		struct Pt { Vec p, v; RotMat rot; };
		auto pts = std::make_shared<std::vector<Pt>>();
		for (const Frame& f : fr)
			pts->push_back({ Vec(f.carX, f.carY, f.carZ), Vec(f.carVX, f.carVY, f.carVZ), Angle(f.carYaw * kU2R, f.carPitch * kU2R, f.carRoll * kU2R).ToRotMat() });
		auto yours = std::make_shared<std::vector<CarControls>>(YourInputs(fr));
		struct Mem { int lastTick = -1000; int air = 0; };
		auto mem = std::make_shared<Mem>();
		return [pts, yours, mem, k](int tick, const CarState& car, const BallState&) {
			Mem& memL = PerThread(mem);   // this thread's copy (the engine runs on several cores)
			const int n = static_cast<int>(pts->size());
			const int t = std::clamp(tick, 0, n - 1);
			if (tick <= memL.lastTick || tick > memL.lastTick + 1) memL.air = 0;
			memL.lastTick = tick;
			memL.air = car.isOnGround ? 0 : memL.air + 1;
			if (car.isOnGround || memL.air < 15) return (*yours)[t];   // on the surface and the jump itself: as you did it
			const Pt& q = (*pts)[std::min(n - 1, t + 1)];
			CarControls c;
			Vec vDes = q.v + (q.p - car.pos) * k;
			Vec dv = vDes - car.vel + Vec(0, 0, 650.0f * 0.12f);
			const float need = dv.Length();
			if (need > 250.0f) Bot::Orient(car, dv / need, q.rot.up, true, c, 3.5f, 0.4f);
			else Bot::Orient(car, q.rot.forward, q.rot.up, true, c, 3.5f, 0.4f);
			c.boost = need > 180.0f && car.rotMat.forward.Dot(dv / std::max(1.0f, need)) > 0.75f;
			c.throttle = 1;
			return c;
		};
	}

	// The shot: a touch into the goal from wherever the last step left off (top and low, middle and corners),
	// re-aimed from where the best tries really crossed the goal line
	StepDef ShotStep(float gs, int horizonTicks, bool highOnly)
	{
		StepDef shot;
		shot.name = "Shot";
		shot.maxTicks = horizonTicks + 50;
		shot.beam = 8;
		const float wx = kWx, zHi = kZHi;
		shot.plans = [gs, wx, zHi, horizonTicks, highOnly](const SavePoint& sp, const std::vector<Vec>& path, std::vector<PlanSpec>& out) {
			const int horizon = std::min(horizonTicks, static_cast<int>(path.size()) - 1);
			const Vec aims[6] = { Vec(0, gs * 5200.0f, zHi), Vec(wx, gs * 5200.0f, zHi), Vec(-wx, gs * 5200.0f, zHi),
				Vec(0, gs * 5200.0f, 300.0f), Vec(wx * 0.7f, gs * 5200.0f, 250.0f), Vec(-wx * 0.7f, gs * 5200.0f, 250.0f) };
			const int nAims = highOnly ? 3 : 6;   // where only high in the net counts (an air dribble's 4 and 5), aim high
			for (int ti = 4; ti <= horizon; ti += 3)
				for (int ai = 0; ai < nAims; ai++)
					for (const Vec& a : { aims[ai] })
					for (int v = 0; v < 4; v++)
						for (int jl : { 24, 60 })
							AddPlan(out, sp, path, ti, a, 150.0f, jl, (v & 1) == 1, true, (v & 2) != 0);
			// Off a carry the ball keeps most of its own speed, so a touch aimed at a point sends it low: aim the push
			// the ball actually needs to drop high into the net (the middle, or a top corner for the 5)
			if (highOnly)
				for (int ti = 4; ti <= horizon; ti += 3)
					for (float x : { 0.0f, 620.0f, -620.0f })
						for (float reach : { 140.0f, 165.0f })
							for (int pw = 0; pw < 2; pw++)
								AddLiftPlan(out, sp, path, ti, Vec(x, gs * 5120.0f, 515.0f), reach, 24, false, pw == 1);
		};
		shot.ended = [](const SavePoint& at, int stepStart) {
			if (at.lastTouch > stepStart && at.tick - at.lastTouch >= 6) return 1;
			return 0;
		};
		shot.aimFix = 16;
		shot.crossing = [gs](const SavePoint& at, Vec& cr) {
			Vec p = at.ball.pos, v = at.ball.vel;
			if (v.y * gs < 100.0f) return false;
			float t = (gs * 5120.0f - p.y) / v.y;
			if (t < 0.0f || t > 2.5f) return false;
			cr = Vec(p.x + v.x * t, gs * 5200.0f, std::max(93.0f, p.z + v.z * t - 325.0f * t * t));
			return true;
		};
		shot.score = [gs](const SavePoint& at) {
			float on, h, sp;
			AimBall(at.ball, gs, on, h, sp);
			return on * (1.0f + std::clamp((h - 150.0f) / 300.0f, 0.0f, 1.0f) + std::clamp((sp - 1400.0f) / 1400.0f, 0.0f, 1.0f));
		};
		return shot;
	}
}
