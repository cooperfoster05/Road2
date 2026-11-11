// The engine, part 5: the search for the best line from a moment of your rep (the public Engine functions).
// The simulator plumbing, skills, step search and mechanics' steps are in EngineCore, EngineSkills, EngineSteps and
// EnginePlans (.cpp); what they share is in EngineInternal.h.
// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "EngineInternal.h"

using namespace EngineInternal;


const char* Engine::StatusText(EngineStatus s)
{
	switch (s)
	{
	case EngineStatus::NoBall:       return "Nothing to play from here.";
	case EngineStatus::NothingFound: return "The engine couldn't find a goal from here either.";
	case EngineStatus::Cancelled:    return "Stopped.";
	default:                         return "";
	}
}

// The engine from one start tick (Line below picks the start, and may try a second one)
static EngineResult LineAt(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	auto clock0 = std::chrono::steady_clock::now();
	auto elapsed = [&]() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - clock0).count(); };
	// The search gets about two thirds of the time; the rest is kept for checking its best lines hold up
	// (if the search ran to the end of the clock, nothing would get checked and nothing would be shown)
	auto cancelled = [&]() { return cancel && cancel->load(); };
	auto stop = [&]() { return cancelled() || elapsed() > maxSeconds * 0.65f; };
	auto hardStop = [&]() { return cancelled() || elapsed() > maxSeconds; };
	float scale = 1.0f;   // shrinks the search to fit the time on slower machines

	EngineResult out;
	const int n = static_cast<int>(fr.size());
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	int s = std::clamp(startTick, 1, n - 2);
	out.startTick = s;
	{
		RepSegments realSegs = Segmenter::Split(fr, eventTick >= 0 ? eventTick : n - 1);
		RepGrade rg = Grader::GradeRep(fr, realSegs, real, realIn, floorBeforeGoal, ref);
		out.realGrade = real.valid ? rg.overall : 0;
	}
	// Which goal: the one your shot went at (or the way the ball is heading)
	const Frame& f0 = fr[s];
	float gs = real.valid ? real.goalSign : (f0.ballVY >= 0 ? 1.0f : -1.0f);

	// Where the ball goes on its own from here (nobody touches it)
	const int horizon = 480;
	std::vector<Vec> ballPath;
	{
		std::unique_ptr<Arena> ballOnly(Arena::Create(GameMode::SOCCAR));
		BallState bs;
		bs.pos = Vec(f0.ballX, f0.ballY, f0.ballZ);
		bs.vel = Vec(f0.ballVX, f0.ballVY, f0.ballVZ);
		bs.angVel = Vec(f0.ballAVX, f0.ballAVY, f0.ballAVZ);
		ballOnly->ball->SetState(bs);
		ballPath.push_back(bs.pos);
		for (int i = 0; i < horizon; i++) { ballOnly->Step(1); ballPath.push_back(ballOnly->ball->GetState().pos); }
	}

	std::unique_ptr<Arena> arena(Arena::Create(GameMode::SOCCAR));
	// MatchBoostRule(arena.get(), fr) is off for now (Oct 14): giving the sim your unlimited boost made the engine's
	// lines more fragile (ground shots from contact 28 (19) -> 23 (15) in a side-by-side run, double taps and flip
	// reset contact down too) with no gain anywhere yet. Kept for the flip reset climb, where it may matter.
	Car* car = arena->AddCar(Team::BLUE, Body(carBody));
	Workers wk(arena.get(), car, EngineThreads(), Body(carBody));
	const int maxTick = std::min(s + horizon + 240, s + 900);
	const Vec p0(f0.carX, f0.carY, f0.carZ), v0(f0.carVX, f0.carVY, f0.carVZ);
	const bool startAir = !f0.onGround;

	// Aim points in the goal (ball center): the middle up high, the top corners, the low corners
	const float wx = 892.755f - kBallR - 60.0f, zHi = 642.775f - kBallR - 50.0f, zLo = kBallR + 60.0f;
	std::vector<Vec> aims = { Vec(0, gs * 5200.0f, zHi), Vec(wx, gs * 5200.0f, zHi), Vec(-wx, gs * 5200.0f, zHi),
		Vec(0, gs * 5200.0f, 330.0f), Vec(wx, gs * 5200.0f, zLo), Vec(-wx, gs * 5200.0f, zLo) };

	// aimPt: the point (x, z on the goal line) the hit is pointed at; the search corrects it from where shots really go
	struct Option { Bot::Plan plan; Vec aimPt; float reach = 150; int jumpLead = 60; };
	auto makePlan = [&](int T, Vec aimPt, float reach, int jumpLead, bool dodge, bool doubleJump, Option& o, bool power = false) -> bool {
		int ti = T - s;
		if (ti < 6 || ti > horizon) return false;
		Vec B = ballPath[ti];
		if (std::fabs(B.y) > 5100.0f) return false;   // already at the goal
		Vec A = aimPt;
		// Aim a little above the point to allow for the drop on the way (about 2,000 uu/s off the hit)
		float flight = (A - B).Length() / 2000.0f;
		A.z += 0.5f * 650.0f * flight * flight;
		Vec d = A - B;
		float dl = d.Length();
		if (dl < 1) return false;
		d = d / dl;
		Vec C = B - d * reach;
		bool ground = C.z < 60.0f;
		// From the ground, a short jump into it (lifts the ball: height in the net is what makes ground 4s and 5s)
		bool jumpShot = ground && jumpLead <= 30;
		if (jumpShot)
		{
			Vec dg = d; dg.z = 0;
			if (dg.Length() < 0.1f) return false;
			dg = dg / dg.Length();
			C = B - dg * reach;
			// How far under the ball the car meets it decides the lift: the aim height (which the aim correction
			// moves toward where the shot really went) sets it, so a shot that crossed too low gets hit lower next time
			float under = std::clamp((aimPt.z - 300.0f) * 0.25f, -40.0f, 110.0f);
			C.z = std::max(30.0f, B.z - under);
			ground = false;
		}
		else if (ground)
		{
			Vec dg = d; dg.z = 0;
			if (dg.Length() < 0.1f) return false;
			dg = dg / dg.Length();
			C = B - dg * reach;   // on the ground the hit is mostly flat; the car's center sits behind the ball
			C.z = 17.0f;
		}
		float tau = ti / 120.0f;
		if (ground)
		{
			Vec h = C - p0; h.z = 0;
			if (h.Length() / tau > 2300.0f && !startAir) return false;
		}
		else
		{
			Vec need = C - p0 - v0 * tau - Vec(0, 0, -0.5f * 650.0f * tau * tau);
			if (2.0f * need.Length() / (tau * tau) > 1250.0f) return false;
		}
		o.plan.target = C;
		o.plan.hitDir = d;
		o.plan.arriveTick = T;
		o.plan.jumpTick = ground ? -1 : std::max(s, T - jumpLead);   // (starting in the air: after landing, if it does)
		o.plan.dodge = dodge;
		o.plan.doubleJump = doubleJump && !dodge && !jumpShot;
		o.plan.dodgeLead = ground ? 14 : 8;
		o.plan.power = power;
		o.aimPt = aimPt;
		o.jumpLead = jumpLead;
		o.reach = reach;
		return true;
	};

	// Shot-type lock (the rulebook in Mechanics.h): the engine plays the mechanic you went for, the way you went for it.
	// A ground shot stays a ground shot, an aerial stays a real aerial (flown, not a jump-and-tip), a flip reset
	// still gets the reset and uses the flip, however early it starts.
	auto styleOk = [&](const SimRun& r) {
		if (r.touchTick < 0) return false;   // it never touched the ball: whatever happened next isn't the engine's shot
		if (!real.valid) return true;
		return Mechanics::SameMechanic(real, fr, r.shot, r.frames, s);
	};
	auto styledQuality = [&](const SimRun& r) { return Quality(r, ref) - (styleOk(r) ? 0.0f : 100000.0f); };

	struct Tried { Option o; SimRun run; float q; };
	std::vector<Tried> tried;
	// Several options at once, in parallel; kept in order, exactly as if tried one by one
	auto tryOptions = [&](const std::vector<Option>& os) {
		const int base = out.tries;
		std::vector<SimRun> rs(os.size());
		wk.Run(static_cast<int>(os.size()), [&](int i, int w) { rs[i] = Run(wk.A(w), wk.C(w), fr, s, os[i].plan, maxTick, ref, 5000 + base + i); });
		for (size_t i = 0; i < os.size(); i++)
		{
			SimRun& r = rs[i];
			out.tries++;
			float q = styledQuality(r);
			r.frames.clear(); r.frames.shrink_to_fit(); r.ctl.clear(); r.ctl.shrink_to_fit();
			tried.push_back({ os[i], std::move(r), q });
		}
	};
	auto tryOption = [&](const Option& o) {
		SimRun r = Run(arena.get(), car, fr, s, o.plan, maxTick, ref, 5000 + out.tries);
		out.tries++;
		float q = styledQuality(r);
		r.frames.clear();
		r.frames.shrink_to_fit();
		r.ctl.clear();
		r.ctl.shrink_to_fit();
		tried.push_back({ o, std::move(r), q });
	};

	// ---- Coarse: when to meet it, and how to get there (aimed at the middle, up high) ----
	int budget = depth >= 2 ? 4500 : 1000;   // the from-scratch bot's share of the sim runs
	std::vector<Option> coarse;
	for (int T = s + 12; T <= s + horizon; T += 4)
		for (int jl : { 12, 18, 24, 48, 84, 120, 168, 230 })
			for (int v = 0; v < 4; v++)
			{
				Option o;
				if (!makePlan(T, aims[0], 150.0f, jl, (v & 1) == 1, true, o, (v & 2) != 0)) continue;
				bool dup = false;
				for (const Option& c : coarse)
					if (c.plan.arriveTick == o.plan.arriveTick && c.plan.jumpTick == o.plan.jumpTick && c.plan.dodge == o.plan.dodge && c.plan.power == o.plan.power) { dup = true; break; }
				if (!dup) coarse.push_back(o);
			}
	if (coarse.empty()) { out.status = EngineStatus::NoBall; out.ms = elapsed() * 1000.0f; return out; }
	// Spread the coarse budget over the whole range of meeting times
	int coarseBudget = budget * 40 / 100;
	int step = std::max(1, static_cast<int>(coarse.size()) / std::max(1, coarseBudget));
	{
		// Time the first few runs on this machine, and shrink the whole search to fit the clock if it's slow
		auto c0 = std::chrono::steady_clock::now();
		int timed = 0;
		for (size_t i = 0; i < coarse.size() && timed < 12 && !stop(); i += step, timed++) tryOption(coarse[i]);
		float perRun = std::chrono::duration<float>(std::chrono::steady_clock::now() - c0).count() / std::max(1, timed);
		float planned = static_cast<float>(budget + (depth >= 2 ? 2500 + 4000 : 500 + 700) + 250);
		const int baseBudget = budget;
		scale = std::clamp(maxSeconds * 0.6f / std::max(1e-4f, perRun * planned), 0.12f, 1.0f);
		budget = std::max(120, static_cast<int>(budget * scale));
		coarseBudget = budget * 40 / 100;
		size_t start = static_cast<size_t>(timed) * step;
		step = std::max(1, static_cast<int>(coarse.size()) / std::max(1, coarseBudget));
		for (size_t i = start; i < coarse.size() && !stop();)
		{
			std::vector<Option> batch;
			for (; i < coarse.size() && static_cast<int>(batch.size()) < kBatch; i += step) batch.push_back(coarse[i]);
			tryOptions(batch);
		}
		// The first few runs are the slow ones (warming up), so measure again over the whole coarse pass. With time to
		// spare, the rest of the search (aiming, nudging, refining, the mechanic steps) grows to use it: the wait you
		// already sit through buys a deeper look, up to a few times the normal size.
		if (ScaleCap(depth) > 1.0f && out.tries > 40 && !stop())
		{
			float perRun2 = std::chrono::duration<float>(std::chrono::steady_clock::now() - c0).count() / std::max(1, out.tries);
			float use = maxSeconds * (depth >= 2 ? 0.55f : 0.4f) - elapsed();
			float grown = std::clamp(use / std::max(1e-4f, perRun2 * planned), 0.12f, ScaleCap(depth));
			if (grown > scale)
			{
				scale = grown;
				budget = std::max(budget, static_cast<int>(baseBudget * scale));
			}
		}
	}

	// ---- Aim correction on the best few ("shooting"): play it, see where the ball really crosses the goal line,
	// move the aim point by the miss, play it again. Each line, aimed at the middle up high and both top corners. ----
	std::mt19937 rng(777u + static_cast<unsigned>(n));
	auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	auto sameOption = [](const Option& a, const Option& b) {
		return a.plan.arriveTick == b.plan.arriveTick && a.plan.jumpTick == b.plan.jumpTick && a.plan.dodge == b.plan.dodge && a.plan.power == b.plan.power &&
			(a.aimPt - b.aimPt).Length() < 15.0f && std::fabs(a.reach - b.reach) < 1.0f;
	};
	auto seenOption = [&](const Option& o) { for (const Tried& t : tried) if (sameOption(t.o, o)) return true; return false; };
	// (the old one-chain aim fix, "correct", was replaced by the parallel chains below; removed Nov 1)
	{
		std::vector<int> order;
		for (int i = 0; i < static_cast<int>(tried.size()); i++)
			if (tried[i].run.touchTick >= 0 && tried[i].run.shot.valid && tried[i].run.shot.shotDir.y * gs > 0.2f) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		int topK = depth >= 2 ? 40 : 16;
		std::vector<Option> bases;
		for (int i = 0; i < static_cast<int>(order.size()) && static_cast<int>(bases.size()) < topK; i++) bases.push_back(tried[order[i]].o);
		// Each (line, aim) chain runs on its own worker; the chains are merged in order afterward
		struct Chain { Option o; Vec want; std::vector<std::pair<Option, SimRun>> runs; };
		std::vector<Chain> chains;
		for (const Option& b : bases)
			for (int w : { 0, 1, 2 })
			{
				Option o;
				if (!makePlan(b.plan.arriveTick, aims[w], b.reach, b.jumpLead, b.plan.dodge, b.plan.doubleJump, o, b.plan.power)) continue;
				chains.push_back({ o, aims[w], {} });
			}
		for (size_t c0 = 0; c0 < chains.size() && !stop() && out.tries < budget; c0 += kBatch)
		{
			const size_t c1 = std::min(chains.size(), c0 + kBatch);
			const int base = out.tries;
			wk.Run(static_cast<int>(c1 - c0), [&](int k, int w) {
				Chain& ch = chains[c0 + k];
				Option o = ch.o;
				for (int it = 0; it < 4; it++)
				{
					bool dup = seenOption(o);
					for (const auto& pr : ch.runs) dup = dup || sameOption(pr.first, o);
					if (dup) break;
					SimRun r = Run(wk.A(w), wk.C(w), fr, s, o.plan, maxTick, ref, 5000 + base + k * 4 + it);
					const bool cont = r.touchTick >= 0 && r.shot.valid && r.shot.reachesGoalLine;
					const float ex = cont ? r.shot.crossX - ch.want.x : 0.0f, ez = cont ? r.shot.crossZ - ch.want.z : 0.0f;
					ch.runs.push_back({ o, std::move(r) });
					if (!cont || (std::fabs(ex) < 40.0f && std::fabs(ez) < 40.0f)) break;
					Vec next = o.aimPt - Vec(ex * 0.8f, 0, ez * 0.8f);
					next.x = std::clamp(next.x, -2500.0f, 2500.0f);
					next.z = std::clamp(next.z, -800.0f, 2500.0f);
					Option o2;
					if (!makePlan(o.plan.arriveTick, next, o.reach, o.jumpLead, o.plan.dodge, o.plan.doubleJump, o2, o.plan.power)) break;
					o = o2;
				}
			});
			for (size_t k = c0; k < c1; k++)
				for (auto& pr : chains[k].runs)
				{
					SimRun& r = pr.second;
					out.tries++;
					float q = styledQuality(r);
					r.frames.clear(); r.frames.shrink_to_fit(); r.ctl.clear(); r.ctl.shrink_to_fit();
					tried.push_back({ pr.first, std::move(r), q });
				}
		}
	}

	// ---- Then nudge the best ones around: a little earlier or later, reach, jump timing, flip or not ----
	int guard = 0;
	while (out.tries < budget && !stop() && guard < budget * 4)
	{
		// A batch of nudges from the current best eight, tried side by side
		std::vector<int> order(tried.size());
		for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
		size_t top = std::min<size_t>(8, order.size());
		std::partial_sort(order.begin(), order.begin() + top, order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		std::vector<Option> batch;
		for (int k = 0; k < kBatch * 2 && static_cast<int>(batch.size()) < std::min(kBatch, budget - out.tries) && guard < budget * 4; k++, guard++)
		{
			const Option& par = tried[order[pick(0, static_cast<int>(top) - 1)]].o;
			int T = par.plan.arriveTick, jl = par.jumpLead;
			Vec aimPt = par.aimPt;
			float reach = par.reach;
			bool dodge = par.plan.dodge, dj = par.plan.doubleJump, power = par.plan.power;
			switch (pick(0, 5))
			{
			case 0: aimPt = aimPt + Vec(pick(-4, 4) * 40.0f, 0, pick(-4, 4) * 40.0f); break;
			case 1: T += pick(-8, 8); break;
			case 2: reach = std::clamp(reach + pick(-3, 3) * 10.0f, 110.0f, 200.0f); break;
			case 3: jl = std::max(12, jl + pick(-24, 24)); break;
			case 4: dodge = !dodge; dj = !dodge; break;
			case 5: power = !power; break;
			}
			Option o;
			if (!makePlan(T, aimPt, reach, jl, dodge, dj, o, power)) continue;
			bool dup = seenOption(o);
			for (const Option& b : batch) dup = dup || sameOption(b, o);
			if (!dup) batch.push_back(o);
		}
		if (batch.empty()) continue;
		tryOptions(batch);
	}

	// ---- Refining a line (cross-entropy search): starting from a set of inputs, it reshapes the stick, steering,
	// throttle and boost over time and moves the jumps, keeping whatever plays out best. Run on your own line
	// (the best version of the shot you went for) and on the from-scratch bot's best lines (polishing them
	// toward a 4 or 5: more lift, more power, a cleaner line-up). ----
	struct CemTry { std::vector<CarControls> ctl; SimRun run; float q; int source; std::function<SimRun(Arena*, Car*, int, int)> rerun; std::string note; std::vector<int> bounds; };   // source 0 your line, 1 the bot's, 2 the step search
	std::vector<CemTry> cemBest;   // the best few goals, inputs kept

	// ---- Multi-step mechanics: the step search (its lines join the refined ones and go through the same checks) ----
	std::string stepNote;
	std::vector<std::vector<StepDef>> mechPlans = MechanicPlans(real, fr, s, gs);   // kept alive: found lines are re-played from it
	if (!mechPlans.empty() && !stop())
	{
		std::unique_ptr<Arena> ballArena(Arena::Create(GameMode::SOCCAR));
		std::mt19937 srng(777u + static_cast<unsigned>(n));
		int runs = 0, replayed = 0, scored = 0;
		std::string per;
		// Lists that open with the same steps (an air dribble's pop and first touch) search them once and share them
		size_t shared = 0;
		if (mechPlans.size() > 1 && mechPlans[0].size() > 2 && std::string(mechPlans[0][0].name) == "Pop" && std::string(mechPlans[0][1].name) == "First touch")
		{
			shared = 2;
		}
		auto usesPrefix = [&](const std::vector<StepDef>& l) { return shared > 0 && l.size() > 2 && std::string(l[0].name) == "Pop" && std::string(l[1].name) == "First touch"; };
		// The step search's time, counted from when it starts (the quick looks before it can take a few seconds; a
		// window counted from the engine's start was often spent before the first list got going)
		const float t0 = elapsed();
		const float window = std::max(maxSeconds * 0.45f - t0, maxSeconds * 0.35f);
		std::vector<SavePoint> prefix;
		std::vector<int> prefixPassed;
		const float prefixShare = shared ? 0.3f : 0.0f;
		if (shared)
		{
			const float untilP = t0 + window * prefixShare;
			auto prefixStop = [&]() { return cancelled() || elapsed() > untilP; };
			std::vector<StepDef> head(mechPlans[0].begin(), mechPlans[0].begin() + shared);
			prefix = StepSearch(arena.get(), car, ballArena.get(), StartPoint(arena.get(), car, fr, s), head,
				std::max(200, static_cast<int>((depth >= 2 ? 12000 : 3000) * scale * 0.4f)), srng, prefixStop, runs, &prefixPassed, &wk);
			if (prefix.size() > 10) prefix.resize(10);   // the best few first touches go on to every list
		}
		for (size_t pi = 0; pi < mechPlans.size(); pi++)
		{
			const std::vector<StepDef>& mechSteps = mechPlans[pi];
			SavePoint sp0 = StartPoint(arena.get(), car, fr, s);
			std::vector<int> passed;
			const float until = t0 + window * (prefixShare + (1.0f - prefixShare) * (pi + 1) / mechPlans.size());
			auto stepStop = [&]() { return cancelled() || elapsed() > until; };
			const int listBudget = std::max(200, static_cast<int>((depth >= 2 ? 12000 : 3000) * scale / mechPlans.size()));
			std::vector<SavePoint> finals;
			if (usesPrefix(mechSteps))
			{
				passed.assign(mechSteps.size(), 0);
				for (size_t k = 0; k < shared && k < prefixPassed.size(); k++) passed[k] = prefixPassed[k];
				finals = StepSearchFrom(arena.get(), car, ballArena.get(), prefix, shared, mechSteps, listBudget, srng, stepStop, runs, &passed, &wk);
			}
			else finals = StepSearch(arena.get(), car, ballArena.get(), sp0, mechSteps, listBudget, srng, stepStop, runs, &passed, &wk);
			Bot::Plan none;
			none.arriveTick = maxTick;
			for (SavePoint& f : finals)
			{
				if (hardStop()) break;
				std::vector<CarControls> c = f.ctl;
				c.resize(maxTick + 1);   // after the last step: hands off
				SimRun r = PlayOut(arena.get(), car, fr, f, maxTick, ref);
				replayed++;
				if (kStepDebug && f.tick < static_cast<int>(r.frames.size()))
				{
					for (int t = s; t <= f.tick && t - s < static_cast<int>(f.frames.size()); t++)
					{
						const Frame& a = f.frames[t - s];
						const Frame& b2 = r.frames[t];
						float d = (Vec(a.ballX, a.ballY, a.ballZ) - Vec(b2.ballX, b2.ballY, b2.ballZ)).Length() + (Vec(a.carX, a.carY, a.carZ) - Vec(b2.carX, b2.carY, b2.carZ)).Length();
						if (d > 0.5f)
						{
							std::string bs;
							for (int b : f.bounds) bs += std::to_string(b) + " ";
							fprintf(stderr, "  first divergence at tick %d (start %d), bounds %s; touch a %d b %d onGround a %d b %d\n", t, s, bs.c_str(), (int)a.touch, (int)b2.touch, (int)a.onGround, (int)b2.onGround);
							break;
						}
					}
					const Frame& g = r.frames[f.tick];
					fprintf(stderr, "replay check: ball off by %.0f, car off by %.0f at tick %d; replay in %d grade %d\n",
						(Vec(g.ballX, g.ballY, g.ballZ) - f.ball.pos).Length(), (Vec(g.carX, g.carY, g.carZ) - f.car.pos).Length(), f.tick, (int)r.wentIn, r.grade.overall);
				}
				if (kStepDebug && !r.wentIn)
					fprintf(stderr, "final miss: reaches %d cross %.0f %.0f miss %.0f %s | air touches %d impulse %.0f close %.2f type %d\n", (int)r.shot.reachesGoalLine, r.shot.crossX, r.shot.crossZ,
						r.shot.missDist, r.shot.missDir.c_str(), r.shot.airTouches, r.shot.airImpulse, r.shot.airCloseFrac, (int)r.shot.type);
				if (kStepDebug && r.wentIn)
					fprintf(stderr, "final goal at %.0f %.0f speed %.0f impulse %.0f\n", r.shot.crossX, r.shot.crossZ, r.shot.shotSpeed, r.shot.airImpulse);
				if (kStepDebug && r.wentIn)
				{
					std::string why;
					bool same = Mechanics::SameMechanic(real, fr, r.shot, r.frames, s, &why);
					fprintf(stderr, "final goal: grade %d (you %d) type %d same %d %s | air touches %d close %.2f (you %.2f) | %s\n", r.grade.overall, out.realGrade,
						(int)r.shot.type, (int)same, why.c_str(), r.shot.airTouches, r.shot.airCloseFrac, real.airCloseFrac, r.grade.overallWhy.c_str());
				}
				float q = styledQuality(r);
				if (!r.wentIn) continue;
				scored++;
				r.frames.clear();
				r.frames.shrink_to_fit();
				// Robustness for these lines = the plan still works from a nudged start with the timing off (re-aimed
				// closed-loop like a player would), not that the exact same button presses still work
				auto line = std::make_shared<SavePoint>(f);
				auto rerun = [&, line, pi](Arena* A, Car* C, int nudge, int shift) {
					if (nudge == 0 && shift == 0) return PlayOut(A, C, fr, *line, maxTick, ref);   // the line itself
					SavePoint endAt;
					if (!RerunChain(A, C, fr, s, *line, mechPlans[pi], nudge, shift, endAt)) { SimRun miss; miss.touchTick = -1; return miss; }
					return PlayOut(A, C, fr, endAt, maxTick, ref);
				};
				// An air dribble played from your own pop (your inputs in the first step) is labeled "your setup"
				std::string nt = f.note;
				if (real.type == ShotType::AirDribble) nt = !f.chain.empty() && f.chain[0].inputs ? "your setup" : "";
				cemBest.push_back({ std::move(c), std::move(r), q, 2, rerun, nt, f.bounds });
			}
			if (kStepDebug) { for (int k = 0; k < 6; k++) fprintf(stderr, "list %zu step%d: no touch %d, passed %d, fail1 %d, fail2 %d, fail3 %d, fail4 %d, ran out %d\n", pi, k, gStepDbg[k*8], gStepDbg[k*8+1], gStepDbg[k*8+2], gStepDbg[k*8+3], gStepDbg[k*8+4], gStepDbg[k*8+5], gStepDbg[k*8+7]); for (int& x : gStepDbg) x = 0; }
			per += pi ? " | " : "";
			for (size_t k = 0; k < passed.size(); k++) per += std::format("{}{} {}", k ? ", " : "", mechSteps[k].name, passed[k]);
		}
		out.tries += runs + replayed;
		std::sort(cemBest.begin(), cemBest.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
		stepNote = std::format("Step search: {} tries; passed {}; {} of {} finished lines scored.", runs, per, scored, replayed);
	}
	std::mt19937 crng(4242u + static_cast<unsigned>(n));
	std::normal_distribution<float> N01(0.0f, 1.0f);
	std::vector<CemTry> polished;   // the 5 polish's goals (below)
	auto refine = [&](const std::vector<CarControls>& seed, int touch, int cemBudget, int source, bool robust = false) {
		const int m = static_cast<int>(seed.size());
		const int w0 = std::max(s, touch - 180), w1 = std::min(m - 1, touch + 24);
		if (w1 - w0 < 20) return;
		const int knotGap = 15, K = (w1 - w0) / knotGap + 2;
		std::vector<std::pair<int, int>> jumps;   // jump presses in the window: start, length
		for (int i = std::max(w0, 1); i < w1 && jumps.size() < 4; i++)
			if (seed[i].jump && !seed[i - 1].jump) { int len = 0; while (i + len < m && seed[i + len].jump) len++; jumps.push_back({ i, len }); }
		const int D = K * 6 + static_cast<int>(jumps.size());
		std::vector<float> mean(D, 0.0f), sigma(D);
		for (int d = 0; d < D; d++) sigma[d] = d >= K * 6 ? 6.0f : (d % 6 == 5 ? 0.6f : 0.35f);
		auto build = [&](const std::vector<float>& x) {
			std::vector<CarControls> c = seed;
			for (size_t j = 0; j < jumps.size(); j++)
				for (int i = jumps[j].first; i < std::min(m, jumps[j].first + jumps[j].second); i++) c[i].jump = false;
			for (size_t j = 0; j < jumps.size(); j++)
			{
				int at = std::clamp(jumps[j].first + static_cast<int>(std::lround(x[K * 6 + j])), s + 1, m - 2), len = jumps[j].second;
				for (int i = at; i < std::min(m, at + len); i++) c[i].jump = true;
				if (at - 1 >= w0 && c[at - 1].jump) c[at - 1].jump = false;   // keep it a fresh press
			}
			for (int i = w0; i < w1; i++)
			{
				float f = static_cast<float>(i - w0) / knotGap;
				int k0 = std::min(K - 2, static_cast<int>(f));
				float t = f - k0;
				auto off = [&](int dim) { return x[k0 * 6 + dim] * (1 - t) + x[(k0 + 1) * 6 + dim] * t; };
				c[i].pitch = Bot::Clamp1(c[i].pitch + off(0));
				c[i].yaw = Bot::Clamp1(c[i].yaw + off(1));
				c[i].roll = Bot::Clamp1(c[i].roll + off(2));
				c[i].steer = Bot::Clamp1(c[i].steer + off(3));
				c[i].throttle = Bot::Clamp1(c[i].throttle + off(4));
				float b = off(5);
				if (b > 0.4f) c[i].boost = true;
				if (b < -0.4f) c[i].boost = false;
			}
			return c;
		};
		Bot::Plan none;
		none.arriveTick = touch + 120;
		const int pop = depth >= 2 ? 64 : 40, elites = 8;
		int cemTries = 0;
		std::vector<float> bestX(D, 0.0f);
		float bestQ = -1e12f;
		while (cemTries < cemBudget && !stop())
		{
			std::vector<std::pair<float, std::vector<float>>> gen;
			// One generation: draw it, play it out in parallel batches, then take the results in order
			for (int mm0 = 0; mm0 < pop && cemTries < cemBudget && !stop(); mm0 += kBatch)
			{
				const int cnt = std::min({ kBatch, pop - mm0, cemBudget - cemTries });
				std::vector<std::vector<float>> xs(cnt, std::vector<float>(D));
				for (int k = 0; k < cnt; k++)
				{
					if (mm0 + k == 0) xs[k] = bestX;   // keep the best so far in every round
					else
						for (int d = 0; d < D; d++)
						{
							float lim = d >= K * 6 ? 30.0f : 1.6f;
							xs[k][d] = std::clamp(mean[d] + sigma[d] * N01(crng), -lim, lim);
						}
				}
				const int base = out.tries;
				std::vector<std::vector<CarControls>> cs(cnt);
				std::vector<SimRun> rs(cnt);
				std::vector<float> rq(cnt, 0.0f);
				wk.Run(cnt, [&](int k, int w) {
					cs[k] = build(xs[k]);
					if (robust)
					{
						// The 5 polish (Oct 23): score each try by its worse of two plays, the exact start (what you'd be
						// shown) and a nudged one, so it only climbs toward lines that are a 5 for real, not by luck
						rs[k] = Run(wk.A(w), wk.C(w), fr, s, none, maxTick, ref, 0, &cs[k]);
						SimRun r2 = Run(wk.A(w), wk.C(w), fr, s, none, maxTick, ref, 1000 + base + k, &cs[k]);
						rq[k] = std::min(styledQuality(rs[k]), styledQuality(r2));
						return;
					}
					// Each try from a slightly different start (the nudge), so the search favors lines that hold up.
					// (Timing jitter here is off, Oct 18: it cost flip resets from late 23 (17) -> 19 (12).)
#ifdef ENGINE_JITTER
					const int jit = ((base + k) % 4 == 1) ? -1 : ((base + k) % 4 == 3) ? 1 : 0;
#else
					const int jit = 0;
#endif
					std::vector<CarControls> cj;
					if (jit != 0) { cj = cs[k]; for (int i = s; i < static_cast<int>(cs[k].size()); i++) cj[i] = cs[k][std::clamp(i - jit, s, static_cast<int>(cs[k].size()) - 1)]; }
					rs[k] = Run(wk.A(w), wk.C(w), fr, s, none, maxTick, ref, 1000 + base + k, jit != 0 ? &cj : &cs[k]);
				});
				for (int k = 0; k < cnt; k++)
				{
					SimRun& r = rs[k];
					cemTries++;
					out.tries += robust ? 2 : 1;
					float q = robust ? rq[k] : styledQuality(r);
					if (q > bestQ) { bestQ = q; bestX = xs[k]; }
					if (r.wentIn)
					{
						r.frames.clear();
						r.frames.shrink_to_fit();
						// The polish keeps its own list, so it never pushes out a line the search already had
						std::vector<CemTry>& into = robust ? polished : cemBest;
						into.push_back({ std::move(cs[k]), std::move(r), q, source });
						std::sort(into.begin(), into.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
						if (into.size() > (robust ? 12u : 40u)) into.pop_back();
					}
					gen.push_back({ q, std::move(xs[k]) });
				}
			}
			if (gen.size() < static_cast<size_t>(elites)) break;
			std::sort(gen.begin(), gen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (int d = 0; d < D; d++)
			{
				float mu = 0, v = 0;
				for (int e = 0; e < elites; e++) mu += gen[e].second[d];
				mu /= elites;
				for (int e = 0; e < elites; e++) v += (gen[e].second[d] - mu) * (gen[e].second[d] - mu);
				mean[d] = mu;
				float floorSigma = d >= K * 6 ? 1.0f : 0.04f;
				sigma[d] = std::max(floorSigma, 0.9f * std::sqrt(v / elites));
			}
		}
	};
	// Your line
	if (real.valid && !stop())
	{
		std::vector<CarControls> realCtl(n);
		for (int i = 0; i < n; i++)
		{
			const Frame& f = fr[i];
			realCtl[i].throttle = f.throttle; realCtl[i].steer = f.steer; realCtl[i].pitch = f.pitch; realCtl[i].yaw = f.yaw;
			realCtl[i].roll = f.roll; realCtl[i].jump = f.jump; realCtl[i].boost = f.boost; realCtl[i].handbrake = f.handbrake;
		}
		refine(realCtl, real.touchTick, std::max(60, static_cast<int>((depth >= 2 ? 2500 : 500) * scale)), 0);
	}
	// The bot's best two lines that reached the ball, polished
	{
		std::vector<int> order;
		for (int i = 0; i < static_cast<int>(tried.size()); i++) if (tried[i].run.touchTick >= 0) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](int a, int b) { return tried[a].q > tried[b].q; });
		for (int i = 0; i < std::min<int>(2, static_cast<int>(order.size())) && !stop(); i++)
		{
			SimRun seedRun = Run(arena.get(), car, fr, s, tried[order[i]].o.plan, maxTick, ref, 0);
			out.tries++;
			if (seedRun.touchTick < 0 || seedRun.ctl.empty()) continue;
			std::vector<CarControls> seed = seedRun.ctl;
			seed.resize(std::max<size_t>(seed.size(), static_cast<size_t>(maxTick) + 1));
			refine(seed, seedRun.touchTick, std::max(40, static_cast<int>((depth >= 2 ? 2000 : 350) * scale)), 1);
		}
	}

	// ---- The 5 polish (Oct 23): the search's 5s were lucky plays (each try runs from a slightly nudged start, and
	// the best of a thousand nudged plays is the luckiest), so the exact line came back a 4. Re-polish the best few
	// 5s (and the best 4, when there's no 5) on plays that have to be a 5 both from the exact start and a nudged one.
	if (real.valid && !stop())
	{
		std::vector<int> seeds;
		for (int i = 0; i < static_cast<int>(cemBest.size()) && seeds.size() < 2; i++) if (cemBest[i].run.grade.overall >= 5 && !cemBest[i].rerun) seeds.push_back(i);
		if (seeds.empty()) for (int i = 0; i < static_cast<int>(cemBest.size()); i++) if (cemBest[i].run.grade.overall == 4 && !cemBest[i].rerun) { seeds.push_back(i); break; }
		struct Seed { std::vector<CarControls> ctl; int touch, source; };
		std::vector<Seed> sv;
		for (int i : seeds) sv.push_back({ cemBest[i].ctl, cemBest[i].run.touchTick, cemBest[i].source });
		for (auto& [ctl, touch, src] : sv)
		{
			if (stop()) break;
			std::vector<CarControls> seed = ctl;
			seed.resize(std::max<size_t>(seed.size(), static_cast<size_t>(maxTick) + 1));
			refine(seed, touch, std::max(40, static_cast<int>((depth >= 2 ? 240 : 120) * scale)), src, true);
		}
		for (CemTry& c : polished) cemBest.push_back(std::move(c));
		std::sort(cemBest.begin(), cemBest.end(), [](const CemTry& a, const CemTry& b) { return a.q > b.q; });
	}

	// ---- Pick the best goal (from either engine) that also works from slightly nudged starts ----
	struct Pick { float q; int botIdx; int cemIdx; };
	std::vector<Pick> picks;
	for (int i = 0; i < static_cast<int>(tried.size()); i++) if (tried[i].run.wentIn) picks.push_back({ tried[i].q, i, -1 });
	for (int i = 0; i < static_cast<int>(cemBest.size()); i++) picks.push_back({ cemBest[i].q, -1, i });
	// Best grade first, then quality (a 3 with a higher quality score was checked, and shipped, ahead of the 4s)
	auto pickGrade = [&](const Pick& p) { return p.botIdx >= 0 ? tried[p.botIdx].run.grade.overall : cemBest[p.cemIdx].run.grade.overall; };
#ifdef ENGINE_OLD_PICK
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.q > b.q; });
#else
	std::sort(picks.begin(), picks.end(), [&](const Pick& a, const Pick& b) { const int ga = pickGrade(a), gb = pickGrade(b); return ga != gb ? ga > gb : a.q > b.q; });
#endif
	// shift: a human's timing is never frame-perfect, so the checks also jump / press everything a tick or two early or late
	auto replayOn = [&](Arena* A, Car* C, const Pick& pk, int nudge, int shift) {
		if (pk.botIdx >= 0)
		{
			Bot::Plan p = tried[pk.botIdx].o.plan;
			if (p.jumpTick >= 0) p.jumpTick = std::max(s, p.jumpTick + shift);
			return Run(A, C, fr, s, p, maxTick, ref, nudge);
		}
		if (cemBest[pk.cemIdx].rerun) return cemBest[pk.cemIdx].rerun(A, C, nudge, shift);
		Bot::Plan none;
		none.arriveTick = (real.valid ? real.touchTick : s) + 120;
		if (shift == 0) return Run(A, C, fr, s, none, maxTick, ref, nudge, &cemBest[pk.cemIdx].ctl, cemBest[pk.cemIdx].bounds.empty() ? nullptr : &cemBest[pk.cemIdx].bounds);
		const std::vector<CarControls>& c0 = cemBest[pk.cemIdx].ctl;
		std::vector<CarControls> c = c0;
		for (int i = s; i < static_cast<int>(c.size()); i++) { int j = std::clamp(i - shift, s, static_cast<int>(c0.size()) - 1); c[i] = c0[j]; }
		return Run(A, C, fr, s, none, maxTick, ref, nudge, &c);
	};
	static const int kShift[kNudges] = { -2, 2, -1, 1, 0, 0 };

	// ---- The simplifier (Oct 24, Phase 3 item 8): the winning line as inputs a person could copy ----
	// The engine nudges the sticks every tick. This turns its inputs into holds (the stick held in one place for at
	// least 0.1 s, snapped to a few positions), cleans up boost and powerslide taps, and keeps jumps exactly. Then it
	// re-plays them: the simplified line has to score at the same grade and hold up from the nudged starts, like any
	// line. Where it drifts from the engine's line, the engine's own fine inputs go back in just before the drift
	// ("fine aim here"), and it tries again; after a few patches it also starts from a finer snap. If nothing works,
	// the perfect line is shown, as before. It never changes a grade.
	struct Simple
	{
		bool ok = false, reproduced = false;
		SimRun run;
		std::vector<CarControls> ctl;
		int held = 0, changes = 0, changesBefore = 0, fineSpots = 0, rounds = 0, level = 0;
		std::vector<std::pair<int, int>> fineSpans;   // stretches (first and last tick) where the engine's fine inputs stay
	};
	auto simplify = [&](const SimRun& w, const Pick& pk) -> Simple {
		Simple res;
		const int n = static_cast<int>(w.frames.size());
		const int end = std::min(n - 1, w.eventTick >= 0 ? w.eventTick : n - 1);
		if (end - s < 12 || (pk.cemIdx >= 0 && cemBest[pk.cemIdx].rerun)) return res;   // step-search lines re-aim as they replay
		std::vector<CarControls> orig(end + 1);
		for (int i = s; i <= end; i++)
		{
			const Frame& f = w.frames[i];
			CarControls& c = orig[i];
			c.throttle = f.throttle; c.steer = f.steer; c.pitch = f.pitch; c.yaw = f.yaw; c.roll = f.roll;
			c.jump = f.jump; c.boost = f.boost; c.handbrake = f.handbrake;
		}
		const std::vector<int>* resets = (pk.cemIdx >= 0 && !cemBest[pk.cemIdx].bounds.empty()) ? &cemBest[pk.cemIdx].bounds : nullptr;
		Bot::Plan none;
		none.arriveTick = (real.valid ? real.touchTick : s) + 120;
		if (pk.botIdx >= 0) none.arriveTick = tried[pk.botIdx].o.plan.arriveTick;
		auto play = [&](const std::vector<CarControls>& c, int nudge, int shift) {
			if (shift == 0) return Run(arena.get(), car, fr, s, none, maxTick, ref, nudge, &c, resets);
			std::vector<CarControls> sh = c;
			for (int i = s; i < static_cast<int>(sh.size()); i++) sh[i] = c[std::clamp(i - shift, s, static_cast<int>(c.size()) - 1)];
			return Run(arena.get(), car, fr, s, none, maxTick, ref, nudge, &sh);
		};
		auto good = [&](const SimRun& r) { return r.wentIn && r.grade.overall >= w.grade.overall && styleOk(r); };
		// The perfect inputs have to replay as the line first (otherwise there's nothing to compare against)
		{
			SimRun base = play(orig, 0, 0);
			res.rounds++;
			if (!good(base)) return res;
			res.reproduced = true;
		}
		// How many times the inputs change: the stick (as one), throttle, roll, boost and powerslide toggles, jump presses
		auto countChanges = [&](const std::vector<CarControls>& c, float eps) {
			int k = 0;
			for (int i = s + 1; i <= end; i++)
			{
				const CarControls& a = c[i - 1], &b = c[i];
				k += std::fabs(a.steer - b.steer) > eps || std::fabs(a.pitch - b.pitch) > eps || std::fabs(a.yaw - b.yaw) > eps;
				k += std::fabs(a.throttle - b.throttle) > eps;
				k += std::fabs(a.roll - b.roll) > eps;
				k += a.boost != b.boost;
				k += a.handbrake != b.handbrake;
				k += b.jump && !a.jump;
			}
			return k;
		};
		res.changesBefore = countChanges(orig, 0.02f);

		static const float kTol[4] = { 0.30f, 0.18f, 0.10f, 0.05f };       // how far a hold may stray from the engine's stick
		static const float kGrid[4] = { 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.0f };   // the positions a hold snaps to (0 = no snap)
		constexpr int kMinHold = 12;   // 0.1 s
		std::vector<char> fine(end + 1, 0);
		auto build = [&](int level) {
			std::vector<CarControls> c = orig;
			auto fit = [&](std::initializer_list<float CarControls::*> axes) {
				int i = s;
				bool havePrev = false;
				while (i <= end)
				{
					if (fine[i] == 2) { i++; havePrev = false; continue; }   // the engine's own inputs, exactly
					// Fine stretches: short, precise holds (no snapping); everywhere else the level's holds
					const bool precise = fine[i] == 1;
					const float tol = precise ? 0.04f : kTol[level], grid = precise ? 0.0f : kGrid[level];
					const int minHold = precise ? 3 : kMinHold;
					auto snap = [&](float v) { return grid > 0.0f ? std::clamp(std::round(v / grid) * grid, -1.0f, 1.0f) : v; };
					// Extend the hold while every axis stays within the tolerance
					std::vector<float> lo, hi;
					for (auto ax : axes) { lo.push_back(orig[i].*ax); hi.push_back(orig[i].*ax); }
					int j = i;
					while (j + 1 <= end && fine[j + 1] == fine[i])
					{
						bool fits = true;
						size_t a = 0;
						for (auto ax : axes)
						{
							const float v = orig[j + 1].*ax;
							if (std::max(hi[a], v) - std::min(lo[a], v) > 2.0f * tol) { fits = false; break; }
							a++;
						}
						if (!fits) break;
						a = 0;
						for (auto ax : axes) { lo[a] = std::min(lo[a], orig[j + 1].*ax); hi[a] = std::max(hi[a], orig[j + 1].*ax); a++; }
						j++;
					}
					if (j - i + 1 < minHold && havePrev)
						for (int k = i; k <= j; k++) for (auto ax : axes) c[k].*ax = c[i - 1].*ax;   // too short to do by hand: keep holding
					else
					{
						size_t a = 0;
						for (auto ax : axes)
						{
							float mean = 0.0f;
							for (int k = i; k <= j; k++) mean += orig[k].*ax;
							mean = snap(mean / (j - i + 1));
							for (int k = i; k <= j; k++) c[k].*ax = mean;
							a++;
						}
					}
					havePrev = true;
					i = j + 1;
				}
			};
			fit({ &CarControls::steer, &CarControls::yaw, &CarControls::pitch });   // the left stick
			fit({ &CarControls::throttle });
			fit({ &CarControls::roll });
			// Boost and powerslide: fill gaps under 8 ticks, drop taps under 5 (jumps stay exactly as the engine did them)
			auto clean = [&](bool CarControls::* b) {
				for (int pass = 0; pass < 2; pass++)
					for (int i = s; i <= end;)
					{
						if (fine[i] == 2 || (c[i].*b) != (pass == 0 ? false : true)) { i++; continue; }
						int j = i;
						while (j + 1 <= end && fine[j + 1] != 2 && (c[j + 1].*b) == (c[i].*b)) j++;
						const bool inside = i > s && j < end && (c[i - 1].*b) != (c[i].*b) && (c[j + 1].*b) != (c[i].*b);
						if (inside && (pass == 0 ? j - i + 1 < 8 : j - i + 1 < 5))
							for (int k = i; k <= j; k++) c[k].*b = !(c[k].*b);
						i = j + 1;
					}
			};
			clean(&CarControls::boost);
			clean(&CarControls::handbrake);
			return c;
		};
		// The first moment a run drifts from the engine's line (car 30 uu or 8 degrees off, or the ball 30 uu off)
		auto drift = [&](const SimRun& r) {
			const int m = std::min({ static_cast<int>(r.frames.size()), n, end + 1 });
			auto ang = [](int a, int b) { int d = std::abs(a - b) % 65536; return std::min(d, 65536 - d); };
			for (int i = s + 1; i < m; i++)
			{
				const Frame& a = r.frames[i], &b = w.frames[i];
				const float dc = std::hypot(a.carX - b.carX, a.carY - b.carY, a.carZ - b.carZ);
				const float db = std::hypot(a.ballX - b.ballX, a.ballY - b.ballY, a.ballZ - b.ballZ);
				if (dc > 30.0f || db > 30.0f || ang(a.carYaw, b.carYaw) > 1456 || ang(a.carPitch, b.carPitch) > 1456 || ang(a.carRoll, b.carRoll) > 1456) return i;
			}
			return m - 1;
		};
		int level = 0, patches = 0;
		for (int round = 0; round < 16 && !cancelled(); round++)
		{
			std::vector<CarControls> c = build(level);
			SimRun r = play(c, 0, 0);
			res.rounds++;
			if (good(r))
			{
				int held = 0;
				for (int j = 1; j <= kNudges; j++)
				{
					SimRun r2 = play(c, 300 + j, kShift[j - 1]);
					res.rounds++;
					held += r2.wentIn && r2.grade.overall >= w.grade.overall - 1;
					if (j - held > 2) break;
				}
				if (held >= 4)
				{
					res.ok = true; res.run = std::move(r); res.ctl = std::move(c); res.held = held; res.level = level;
					break;
				}
				if (level < 3) { level++; continue; }   // it scores but it's fragile: a finer snap
				break;
			}
			// Fine inputs for the 0.25 s before it drifted: precise holds first, the engine's exact inputs if it drifts there again
			const int d = drift(r);
			const char mark = fine[std::min(end, d)] >= 1 ? 2 : 1;
			for (int k = std::max(s, d - 30); k <= std::min(end, d); k++) fine[k] = std::max(fine[k], mark);
			if (++patches % 4 == 0 && level < 3) level++;
		}
		if (res.ok)
		{
			res.changes = countChanges(res.ctl, 0.001f);
			for (int i = s; i <= end; i++)
				if (fine[i] && (i == s || !fine[i - 1]))
				{
					int j = i;
					while (j + 1 <= end && fine[j + 1]) j++;
					res.fineSpots++;
					res.fineSpans.push_back({ i, j });
				}
		}
		return res;
	};
	// Your takeoff (only if you started on the ground) and boost (from the button, since free play refills it)
	const bool startedGrounded = ShotGeometry::OnSurface(fr[s]);
	int yourTakeoff = -1;
	if (startedGrounded)
		for (int i = s + 1; i < n; i++) if (fr[i].jump && !fr[i - 1].jump && fr[i - 1].onGround) { yourTakeoff = i; break; }
	auto boostTicks = [](const std::vector<Frame>& f, int from, int to) {
		int b = 0;
		for (int i = std::max(0, from); i < std::min(static_cast<int>(f.size()), to); i++) b += f[i].boost ? 1 : 0;
		return b * (100.0f / 3.0f) / 120.0f;   // a full tank lasts 3 s
	};
	float yourBoost = real.valid ? boostTicks(fr, s, real.touchTick) : 0.0f;
	// The checks run in parallel: a batch of candidates (in pick order) is checked at once, one per worker, and the
	// loop below still takes the first one in order that holds up, so the pick is the same as checking one by one.
	struct CheckRes { bool done = false; int held = 0; int tries = 0; SimRun win; };
	std::vector<CheckRes> cres(picks.size());
	// A cleaner 4 (Oct 23, Coop's call): when you already scored a 4, the engine almost never finds a 5 (0 of 15 on the
	// benchmark), so a line that's still a 4 but clearly further toward a 5 also counts: higher in the net, harder,
	// earlier, more on line, closer to a top corner (the partial credit toward the next grade, Quality's credit, plus
	// speed). It has to stay a 4 from the nudged starts, not just stay in.
	float realQ = -1e9f;
	if (real.valid && realIn && out.realGrade == 4)
	{
		SimRun rr;
		rr.touchTick = real.touchTick; rr.shot = real; rr.wentIn = true; rr.grade.overall = out.realGrade;
		realQ = Quality(rr, ref);
	}
	auto cleaner = [&](int grade, float q) { return out.realGrade == 4 && grade == 4 && q - realQ >= kCleanerMargin; };
	auto beats = [&](int grade, float q) { return grade > out.realGrade || cleaner(grade, q); };
	auto eligible = [&](const Pick& pk) { return pk.q >= -50000.0f && beats(pickGrade(pk), pk.q); };
	auto checkOne = [&](size_t i, int w) {
		const Pick& pk = picks[i];
		const int expect = pickGrade(pk);
		CheckRes& c = cres[i];
		for (int j = 1; j <= kNudges; j++)
		{
			SimRun r = replayOn(wk.A(w), wk.C(w), pk, 300 + j, kShift[j - 1]);
			c.tries++;
			c.held += r.wentIn && r.grade.overall >= (expect > out.realGrade ? expect - 1 : expect);
			if (j - c.held > 2) break;
		}
		if (c.held >= 4) c.win = replayOn(wk.A(w), wk.C(w), pk, 0, 0);
		c.done = true;
	};
	auto ensureChecked = [&](size_t i) {
		if (cres[i].done) return;
		std::vector<size_t> batch;
		for (size_t k = i; k < picks.size() && static_cast<int>(batch.size()) < kBatch; k++) if (!cres[k].done && eligible(picks[k])) batch.push_back(k);
		wk.Run(static_cast<int>(batch.size()), [&](int b, int w) { checkOne(batch[b], w); });
	};
	int checked = 0;
	int dbgKind = 0, dbgFragile = 0, dbgWin = 0, dbgUnreached = 0, dbgCand = 0;
	int kindBeat = 0;   // lines graded above yours that played a different kind of shot (for the "nothing better" words)
	if (kStepDebug)
	{
		int types[8] = {}, inT[8] = {}, tip = 0;
		for (const Tried& t : tried) if (t.run.touchTick >= 0) { int ty = std::clamp(static_cast<int>(t.run.shot.type), 0, 7); types[ty]++; if (t.run.wentIn) inT[ty]++; tip += t.run.wentIn && t.run.shot.shotSpeed < Mechanics::MinShotSpeed(real.type); }
		fprintf(stderr, "TYPES you %d | touched by type:", (int)real.type);
		for (int i = 0; i < 8; i++) if (types[i]) fprintf(stderr, " t%d=%d(in %d)", i, types[i], inT[i]);
		fprintf(stderr, " | slow goals %d\n", tip);
		for (const Tried& t : tried) if (t.run.touchTick >= 0 && t.run.shot.type != real.type) { fprintf(stderr, "  example: line touch %d airTouches %d airFirst %d dribble %d | you: touch %d airTouches %d airFirst %d s %d\n", t.run.touchTick, t.run.shot.airTouches, t.run.shot.airFirstTouch, (int)t.run.shot.dribble, real.touchTick, real.airTouches, real.airFirstTouch, s); break; }
	}
	for (const Pick& pk : picks) { int g = pk.botIdx >= 0 ? tried[pk.botIdx].run.grade.overall : cemBest[pk.cemIdx].run.grade.overall; dbgCand += g >= 4; }
	int pi2 = -1;
	for (const Pick& pk : picks)
	{
		pi2++;
		// The check budget: the old cap of 60 ran out on near-copies of fragile lines while thousands of others went
		// unchecked (aerials from the setup: 14 of 25 reps that had 4-5 candidates never got to them). Keep checking while
		// there's time. (Skipping near-copies of a failed line lost robust variants of it, so every line gets its check.)
		if (checked >= (depth >= 2 ? 600 : 300) || cancelled() || (hardStop() && checked >= 3) || (checked >= 60 && elapsed() > maxSeconds * 0.85f))
		{
			if (kStepDebug) for (size_t q = pi2; q < picks.size(); q++) { const Pick& p2 = picks[q]; int g = p2.botIdx >= 0 ? tried[p2.botIdx].run.grade.overall : cemBest[p2.cemIdx].run.grade.overall; dbgUnreached += g >= 4; }
			break;
		}
		const int g4 = pk.botIdx >= 0 ? tried[pk.botIdx].run.grade.overall : cemBest[pk.cemIdx].run.grade.overall;
		if (pk.q < -50000.0f) { dbgKind += g4 >= 4; kindBeat += g4 > out.realGrade; continue; }   // not the kind of shot you went for
		int expect = pk.botIdx >= 0 ? tried[pk.botIdx].run.grade.overall : cemBest[pk.cemIdx].run.grade.overall;
		if (!beats(expect, pk.q)) continue;   // only lines that beat your shot (or a cleaner 4 than yours)
		checked++;
		ensureChecked(static_cast<size_t>(pi2));
		CheckRes& cr = cres[pi2];
		out.tries += cr.tries;
		int held = cr.held;
		if (kStepDebug && pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2) fprintf(stderr, "verify step-search line: expect %d (you %d), held %d\n", expect, out.realGrade, held);
		if (held < 4) { dbgFragile += expect >= 4; continue; }
		SimRun win = std::move(cr.win);
		if (kStepDebug && pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2) fprintf(stderr, "  win: in %d grade %d style %d touch %d\n", (int)win.wentIn, win.grade.overall, (int)styleOk(win), win.touchTick);
		if (!win.wentIn || !beats(win.grade.overall, styledQuality(win)) || !styleOk(win)) { dbgWin += expect >= 4; continue; }
		const bool sameGrade = win.grade.overall == out.realGrade;
		if (kStepDebug) { for (size_t q = pi2 + 1; q < picks.size(); q++) { const Pick& p2 = picks[q]; int g = p2.botIdx >= 0 ? tried[p2.botIdx].run.grade.overall : cemBest[p2.cemIdx].run.grade.overall; dbgUnreached += g >= 4; } }
		if (kStepDebug) fprintf(stderr, "PICKSTAT shipped grade %d expect %d | 4-5 candidates %d: wrong kind %d, fragile %d, replay/style %d, after the pick or unchecked %d | checked %d\n", win.grade.overall, expect, dbgCand, dbgKind, dbgFragile, dbgWin, dbgUnreached, checked);
		// The same line as inputs a person could copy, if it still holds up (the shown line, then; the perfect one is kept).
		// Off since Nov 1 (Coop: the effort goes into the engine itself; the input ghost shows the perfect line, smoothed
		// for display): kSimplify brings it back.
		constexpr bool kSimplify = false;
		Simple simp = kSimplify ? simplify(win, pk) : Simple{};
		if (std::getenv("ENG_SIMPDBG"))
			fprintf(stderr, "SIMP ok %d reproduced %d grade %d level %d rounds %d changes %d -> %d fine %d held %d (perfect held %d) src %s\n", (int)simp.ok, (int)simp.reproduced,
				win.grade.overall, simp.level, simp.rounds, simp.changesBefore, simp.changes, simp.fineSpots, simp.held, held,
				pk.botIdx >= 0 ? "bot" : (cemBest[pk.cemIdx].rerun ? "step" : "cem"));
		out.inputChangesPerfect = simp.changesBefore;
		if (simp.ok && simp.changes < simp.changesBefore)
		{
			out.simplified = true;
			out.inputChanges = simp.changes;
			out.fineSpans = simp.fineSpans;
			out.perfectFrames = win.frames;
			win.frames = std::move(simp.run.frames);
			held = simp.held;
		}
		out.status = EngineStatus::Found;
		out.grade = win.grade.overall;
		out.wentIn = win.wentIn;
		out.gradeWhy = win.grade.overallWhy;
		out.held = held;
		out.heldOf = kNudges;
		out.eventTick = win.eventTick;
		out.touchTick = win.touchTick;
		const Frame& hit = win.frames[std::min(static_cast<int>(win.frames.size()) - 1, std::max(0, win.touchTick))];
		win.boostUsed = boostTicks(win.frames, s, win.touchTick);
		if (!startedGrounded) win.takeoff = -1;

		// The cue: the one or two biggest differences from what you did, in words (the numbers are on the hover)
		struct Diff { float size; std::string say; };
		std::vector<Diff> diffs;
		auto amount = [](float size) { return size >= 3.0f ? "a lot " : (size >= 1.6f ? "" : "a bit "); };
		// Where on the ball the car hits it (relative to the ball's center, in the car's frame at contact)
		auto contact = [](const Frame& f, float& under, float& side) {
			RotMat R = Angle(f.carYaw * kU2R, f.carPitch * kU2R, f.carRoll * kU2R).ToRotMat();
			Vec rel = Vec(f.touchX - f.ballX, f.touchY - f.ballY, f.touchZ - f.ballZ);
			under = -rel.z; side = rel.Dot(R.right);
		};
		if (real.valid)
		{
			const Frame& yh = fr[real.touchTick];
			if (win.takeoff >= 0 && yourTakeoff >= 0 && std::abs(win.takeoff - yourTakeoff) >= 6)
			{
				float sz = std::abs(win.takeoff - yourTakeoff) / 8.0f;
				diffs.push_back({ sz, std::format("jump {}{}", amount(sz), win.takeoff < yourTakeoff ? "earlier" : "later") });
			}
			if (win.takeoff >= 0 && yourTakeoff < 0) diffs.push_back({ 5.0f, "jump for it" });
			if (win.takeoff < 0 && yourTakeoff >= 0) diffs.push_back({ 5.0f, "stay on the ground" });
			if (hit.touch && yh.touch)
			{
				float eu, es, yu, ys;
				contact(hit, eu, es); contact(yh, yu, ys);
				if (std::fabs(eu - yu) >= 15.0f)
				{
					float sz = std::fabs(eu - yu) / 20.0f;
					diffs.push_back({ sz, eu > yu ? std::format("get {}more under it", amount(sz)) : std::format("hit it {}flatter", amount(sz)) });
				}
				if (std::fabs(es - ys) >= 15.0f)
				{
					float sz = std::fabs(es - ys) / 20.0f;
					diffs.push_back({ sz, std::format("hit it {}more on its {} side", amount(sz), es > ys ? "right" : "left") });
				}
			}
			if (std::abs(win.touchTick - real.touchTick) >= 8)
			{
				float sz = std::abs(win.touchTick - real.touchTick) / 10.0f;
				diffs.push_back({ sz, std::format("meet it {}{}", amount(sz), win.touchTick < real.touchTick ? "sooner" : "later") });
			}
			if (std::fabs(win.boostUsed - yourBoost) >= 10.0f)
			{
				float sz = std::fabs(win.boostUsed - yourBoost) / 15.0f;
				diffs.push_back({ sz, win.boostUsed > yourBoost ? std::format("{}more boost", amount(sz)) : std::format("{}less boost", amount(sz)) });
			}
			if (win.shot.shotSpeed - real.shotSpeed >= 300.0f)
			{
				float sz = (win.shot.shotSpeed - real.shotSpeed) / 400.0f;
				diffs.push_back({ sz, std::format("hit it {}harder", amount(sz)) });
			}
		}
		// An air dribble found by the step search is judged on the carry, not on one hit (how many touches, how
		// close, how high it went in): until the step-by-step cues land, say it in those words
		if (real.valid && real.type == ShotType::AirDribble && pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && win.shot.airDribble)
		{
			diffs.clear();
			const int more = win.shot.airTouches - real.airTouches;
			if (more >= 1) diffs.push_back({ 1.0f + more / 2.0f, std::format("keep touching it ({} touches in the air, you had {})", win.shot.airTouches, real.airTouches) });
			if (win.shot.airCloseFrac - real.airCloseFrac >= 0.15f)
				diffs.push_back({ (win.shot.airCloseFrac - real.airCloseFrac) * 8.0f, "keep it closer to your nose" });
			if (win.shot.reachesGoalLine && real.reachesGoalLine && win.shot.crossZ - real.crossZ >= 60.0f)
				diffs.push_back({ (win.shot.crossZ - real.crossZ) / 60.0f, std::format("carry it in {}higher", amount((win.shot.crossZ - real.crossZ) / 60.0f)) });
			if (win.shot.reachesGoalLine && std::fabs(win.shot.crossX) >= 580.0f && std::fabs(real.crossX) < 580.0f)
				diffs.push_back({ 2.0f, "take it to the corner" });
			// The first touch (where the carry is won or lost: after a good one, Coop's carries finished high 51% of the
			// time, after the rest 10%): high (above 1,425), soft (rising slower than ~506), close to the nose. It leads the
			// cue when yours missed the target and the line hit it: the rest of the carry follows from it
			float ez, evz, ed, yz, yvz, yd;
			if (FirstTouchAfter(win.frames, win.shot.airFirstTouch, ez, evz, ed) && FirstTouchAfter(fr, real.airFirstTouch, yz, yvz, yd)
				&& GoodFirstTouch(ez, evz, ed) && !GoodFirstTouch(yz, yvz, yd))
			{
				if (yz < 1375.0f && ez - yz >= 100.0f)
					diffs.push_back({ 3.5f + (ez - yz) / 200.0f, std::format("take the first touch higher (ball {:.0f} high after it, you had {:.0f})", ez, yz) });
				if (yvz > 600.0f && yvz - evz >= 150.0f)
					diffs.push_back({ 3.5f + (yvz - evz) / 300.0f, std::format("softer first touch (you sent it up at {}, the line at {})", Units::Speed(yvz), Units::Speed(std::max(0.0f, evz))) });
				if (yd >= 300.0f && yd - ed >= 80.0f)
					diffs.push_back({ 3.0f + (yd - ed) / 150.0f, "stay closer to it after the first touch" });
			}
		}
		std::sort(diffs.begin(), diffs.end(), [](const Diff& a, const Diff& b) { return a.size > b.size; });
		std::vector<std::string> parts;
		for (const Diff& d : diffs) if (parts.size() < 2 && d.size >= 0.75f) parts.push_back(d.say);
		bool yourLine = pk.botIdx < 0 && cemBest[pk.cemIdx].source == 0;
		std::string kind = yourLine ? "Engine (your line, sharpened)" : "Engine";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && !cemBest[pk.cemIdx].note.empty()) kind = "Engine (" + cemBest[pk.cemIdx].note + ")";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && real.valid && real.type == ShotType::FlipReset) kind = "Engine (your takeoff, its reset and shot)";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && real.valid && real.type == ShotType::Dribble) kind = "Engine (carried it, then flicked)";
		if (pk.cemIdx >= 0 && cemBest[pk.cemIdx].source == 2 && real.valid && real.type == ShotType::AirDribble)
			kind = cemBest[pk.cemIdx].note == "your setup" ? "Engine (your setup, then carried in)" : "Engine (carried in)";
		if (sameGrade)
		{
			// What's cleaner about it, in words: the one or two biggest steps toward a 5
			std::vector<std::pair<float, std::string>> better;
			if (win.shot.shotSpeed - real.shotSpeed >= 150.0f) better.push_back({ (win.shot.shotSpeed - real.shotSpeed) / 300.0f, "harder" });
			if (win.shot.crossZ - real.crossZ >= 60.0f) better.push_back({ (win.shot.crossZ - real.crossZ) / 120.0f, "higher" });
			if (real.cornerDist - win.shot.cornerDist >= 100.0f) better.push_back({ (real.cornerDist - win.shot.cornerDist) / 200.0f, "closer to the corner" });
			if ((real.touchTick - real.startTick) - (win.shot.touchTick - win.shot.startTick) >= 12) better.push_back({ ((real.touchTick - real.startTick) - (win.shot.touchTick - win.shot.startTick)) / 24.0f, "earlier" });
			if (real.aimErrDeg - win.shot.aimErrDeg >= 1.0f) better.push_back({ (real.aimErrDeg - win.shot.aimErrDeg) / 2.0f, "more on line" });
			std::sort(better.begin(), better.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::string what = "a cleaner 4";
			for (size_t i = 0; i < better.size() && i < 2; i++) what += (i == 0 ? ": " : " and ") + better[i].second;
			kind = kind == "Engine" ? "Engine (" + what + ")" : kind.substr(0, kind.size() - 1) + ", " + what + ")";
		}
		std::string joined;
		for (size_t i = 0; i < parts.size(); i++) joined += (i == 0 ? "" : " and ") + parts[i];
		if (joined.empty()) joined = "same idea, cleaner contact";
		out.cue = kind + ": " + joined;
		if (!out.cue.empty()) out.cue[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out.cue[0])));

		std::string lines;
		if (win.takeoff >= 0) lines += std::format("Takes off at {} ({}).\n", Secs(win.takeoff - s), yourTakeoff >= 0 ? "you: " + Secs(yourTakeoff - s) : std::string("you didn't jump"));
		if (real.valid)
		{
			const Frame& yh = fr[real.touchTick];
			lines += std::format("Meets the ball at {}, {:.0f} high (you: {}, {:.0f} high).\n", Secs(win.touchTick - s), hit.ballZ, Secs(real.touchTick - s), yh.ballZ);
			lines += std::format("Ball off the hit at {} (you: {}).\n", Units::Speed(win.shot.shotSpeed), Units::Speed(real.shotSpeed));
			if (hit.touch && yh.touch)
			{
				float eu, es, yu, ys;
				contact(hit, eu, es); contact(yh, yu, ys);
				lines += std::format("Contact {:.0f} under the ball's center and {:.0f} {} (you: {:.0f} under, {:.0f} {}).\n",
					eu, std::fabs(es), es >= 0 ? "right" : "left", yu, std::fabs(ys), ys >= 0 ? "right" : "left");
			}
		}
		if (real.valid && real.type == ShotType::AirDribble && win.shot.airDribble)
		{
			float ez, evz, ed, yz, yvz, yd;
			if (FirstTouchAfter(win.frames, win.shot.airFirstTouch, ez, evz, ed) && FirstTouchAfter(fr, real.airFirstTouch, yz, yvz, yd))
			{
				auto rise = [](float vz) { return vz >= 0.0f ? "rising " + Units::Speed(vz) : "dropping " + Units::Speed(-vz); };
				lines += std::format("First touch leaves the ball {:.0f} high, {}, {:.0f} from the car (you: {:.0f} high, {}, {:.0f} away). A good one: above 1,425, rising under ~500, within 300.\n",
					ez, rise(evz), ed, yz, rise(yvz), yd);
			}
		}
		lines += std::format("Boost used: {:.0f} (you: {:.0f}).\n", win.boostUsed, yourBoost);

		// Step by step (Nov 1, plan 6e): both reps graded segment by segment; for each step the engine played, how you
		// both did, and the biggest difference that belongs to it. The earliest step it did better is the root cause.
		{
			auto rank = [](Grade g) {
				switch (g)
				{
				case Grade::Calculated: case Grade::Clean: return 4;
				case Grade::Solid: return 3;
				case Grade::Sloppy: return 2;
				case Grade::None: return 0;
				default: return 1;
				}
			};
			// which step a difference belongs to (by what it's about)
			auto stepOf = [](const std::string& say) {
				if (say.find("jump") != std::string::npos || say.find("ground") != std::string::npos) return SegKind::Takeoff;
				if (say.find("first touch") != std::string::npos || say.find("touching it") != std::string::npos || say.find("nose") != std::string::npos
					|| say.find("carry it") != std::string::npos) return SegKind::AirDribble;
				if (say.find("boost") != std::string::npos) return SegKind::Flight;
				if (say.find("corner") != std::string::npos) return SegKind::Result;
				return SegKind::Contact;   // under it, flatter, its side, sooner / later, harder
			};
			const int yEnd = eventTick >= 0 ? eventTick : n - 1;
			RepSegments ys = Segmenter::Split(fr, yEnd);
			RepGrade yg = Grader::GradeRep(fr, ys, real, realIn, floorBeforeGoal, ref);
			const int lEnd = win.eventTick >= 0 ? win.eventTick : static_cast<int>(win.frames.size()) - 1;
			RepSegments ls = Segmenter::Split(win.frames, lEnd);
			RepGrade lg = Grader::GradeRep(win.frames, ls, win.shot, win.wentIn, win.floorFirst, ref);
			auto gradeOf = [](const RepGrade& g, int segIndex) -> const SegmentGrade* {
				for (const SegmentGrade& sg : g.segs) if (sg.segIndex == segIndex) return &sg;
				return nullptr;
			};
			// pair the line's steps with yours: the same kind of segment, the same one in order (2nd contact with 2nd contact)
			std::map<int, int> seen;
			bool rootSet = false;
			for (int li = 0; li < static_cast<int>(ls.segs.size()); li++)
			{
				const Segment& L = ls.segs[li];
				const int occ = seen[static_cast<int>(L.kind)]++;
				if (L.kind == SegKind::Result || L.kind == SegKind::Recovery) continue;
				if (L.end < s) continue;   // before the engine took over: the same as yours
				const SegmentGrade* lgr = gradeOf(lg, li);
				if (!lgr || lgr->grade == Grade::None) continue;
				int yi = -1, k = 0;
				for (int i = 0; i < static_cast<int>(ys.segs.size()); i++)
					if (ys.segs[i].kind == L.kind && k++ == occ) { yi = i; break; }
				const SegmentGrade* ygr = yi >= 0 ? gradeOf(yg, yi) : nullptr;
				EngineResult::StepCue c;
				c.step = Segmenter::Name(L.kind);
				c.line = rank(lgr->grade);
				c.lineWhy = lgr->why;
				c.yours = ygr ? rank(ygr->grade) : 0;
				c.yourWhy = ygr ? ygr->why : (yi < 0 ? "you didn't have this step" : "");
				for (const Diff& d : diffs) if (stepOf(d.say) == L.kind && d.size >= 0.75f) { c.fix = d.say; break; }
				if (!rootSet && c.line > c.yours) { c.root = true; rootSet = true; }
				out.steps.push_back(c);
			}
			if (!out.steps.empty())
			{
				lines += "Step by step:\n";
				for (const EngineResult::StepCue& c : out.steps)
					lines += std::format("{}{}: {} (you: {}){}\n", c.root ? "* " : "", c.step, c.lineWhy.empty() ? "graded the same" : c.lineWhy,
						c.yourWhy.empty() ? "-" : c.yourWhy, c.fix.empty() ? "" : " - " + c.fix);
			}
		}
		out.compare = lines;
		out.detail = std::format("{}Would be a {} ({}).\n{}It plays perfect inputs from {} into your rep: it shows the ceiling for this shot. "
			"It scored from {} of {} slightly different starts, with the timing a tick or two early or late.", lines, out.grade, out.gradeWhy,
			yourLine ? "The engine started from your own inputs and reshaped them. " : "The engine played this from scratch (none of your inputs). ",
			Secs(s - (real.valid ? real.startTick : 0)), held, kNudges);
		out.frames = std::move(win.frames);
		break;
	}
	if (out.status != EngineStatus::Found)
	{
		if (kStepDebug) fprintf(stderr, "PICKSTAT none | 4-5 candidates %d: wrong kind %d, fragile %d, replay/style %d, unchecked %d | checked %d\n", dbgCand, dbgKind, dbgFragile, dbgWin, dbgUnreached, checked);
		out.status = (cancel && cancel->load()) ? EngineStatus::Cancelled : EngineStatus::NothingFound;
	}
	int touched = 0, goals = 0, high = 0;
	for (const Tried& t : tried) { touched += t.run.touchTick >= 0; goals += t.run.wentIn; high += t.run.wentIn && t.run.grade.overall >= 4; }
	for (const CemTry& c : cemBest) high += c.run.grade.overall >= 4;
	out.note = std::format("{} lines tried: {} touched the ball, {} scored, {} as a 4 or 5. {:.1f} s.{}", tried.size(), touched, goals, high, elapsed(),
		scale < 0.99f ? std::format(" This computer is slower, so the search was sized down to {:.0f}% to fit the time.", scale * 100.0f)
		: scale > 1.01f ? std::format(" It had time to spare, so it searched {:.1f}x deeper.", scale) : std::string());
	if (!stepNote.empty()) out.note += " " + stepNote;
	// Nothing better, in words (Nov 1): on the ground bench a third of the "nothing found" reps weren't shots at all (the
	// touch went the other way, e.g. carrying it out of the corner), and others had hundreds of goals that just didn't
	// beat yours. "Your shot was the best it could find" was wrong for the first and vague for the second.
	if (out.status == EngineStatus::NothingFound)
	{
		if (goals == 0 && real.valid && !realIn && !real.reachesGoalLine && real.aimErrDeg > 90.0f)
			out.noneWhy = std::format("Your touch went {:.0f} degrees away from the goal, so this wasn't really a shot, and nothing it tried from here scored either.", real.aimErrDeg);
		else if (touched == 0)
			out.noneWhy = "None of its lines could reach the ball in time from here.";
		else if (goals == 0)
			out.noneWhy = std::format("{} of its lines touched the ball, but none of them scored from here.", touched);
		else if (kindBeat > 0)
			out.noneWhy = std::format("It found {} goals from here. The ones graded above yours played a different kind of shot, so they don't count.", goals);
		else
			out.noneWhy = std::format("It found {} goals from here, but none graded higher than your {}: your shot was as good as anything it found.", goals, out.realGrade);
	}
	out.ms = elapsed() * 1000.0f;
	return out;
}

static EngineResult LineOnce(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth);

EngineResult Engine::Line(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	if (depth < 2) return LineOnce(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, startTick, 1);
	// Thinking harder: the normal search first, then the big one, so it never comes back with less
	auto t0 = std::chrono::steady_clock::now();
	EngineResult quick = LineOnce(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds * 0.25f, startTick, 1);
	float used = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
	if (cancel && cancel->load()) return quick;
	EngineResult deep = LineOnce(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, std::max(1.0f, maxSeconds - used), startTick, 2);
	bool qf = quick.status == EngineStatus::Found, df = deep.status == EngineStatus::Found;
	EngineResult& best = (df && (!qf || deep.grade > quick.grade)) ? deep : quick;
	best.tries = quick.tries + deep.tries;
	return std::move(best);
}

// A line good enough to stop the ceiling-shot stages: a 5, or a 4 that's really clean (every part of the shot graded
// Clean or Solid, and it holds from nearly every nudged start)
static bool GoodEnoughToStop(const EngineResult& r, const ShotReference& ref)
{
	if (r.status != EngineStatus::Found) return false;
	if (r.grade >= 5) return true;
	if (r.grade < 4 || r.frames.empty()) return false;
	if (r.heldOf > 0 && r.held < r.heldOf - 1) return false;
	ShotAnalysis a = ShotGeometry::Analyze(r.frames);
	RepSegments segs = Segmenter::Split(r.frames, r.eventTick);
	RepGrade g = Grader::GradeRep(r.frames, segs, a, r.wentIn, false, ref);
	for (const SegmentGrade& sg : g.segs)
	{
		if (sg.segIndex < 0 || sg.segIndex >= static_cast<int>(segs.segs.size())) continue;
		SegKind k = segs.segs[sg.segIndex].kind;
		if (k == SegKind::Recovery || k == SegKind::Result) continue;
		if (sg.grade == Grade::Sloppy || sg.grade == Grade::Costly || sg.grade == Grade::Throw) return false;
	}
	return true;
}

static EngineResult CeilingStages(const std::vector<Frame>& fr, const ShotAnalysis& real, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int s, int depth)
{
	const int n = static_cast<int>(fr.size());
	struct Stage { int start; const char* name; };
	std::vector<Stage> stages{ { s, "" } };
	const int fromCeiling = std::max(real.startTick + 1, real.ceilLand + 6);
	if (fromCeiling < s - 20) stages.push_back({ fromCeiling, "from the ceiling" });
	const int fromWall = real.ceilWallTouch >= 0 ? std::max(real.startTick + 1, real.ceilWallTouch - 30) : -1;
	if (fromWall >= 1 && fromWall < stages.back().start - 20) stages.push_back({ fromWall, "from your wall touch" });

	auto t0 = std::chrono::steady_clock::now();
	EngineResult best;
	int bestStage = -1, tries = 0;
	std::string searched;
	for (size_t i = 0; i < stages.size(); i++)
	{
		const int st = std::clamp(stages[i].start, 1, std::max(1, n - 2));
		EngineResult r = LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, st, depth);
		tries += r.tries;
		if (r.status == EngineStatus::Cancelled || r.status == EngineStatus::NoBall || (cancel && cancel->load()))
		{
			if (bestStage < 0) return r;
			break;
		}
		searched += i == 0 ? "0.5 s before your touch" : std::string(i + 1 == stages.size() ? " and " : ", ") + stages[i].name;
		const bool found = r.status == EngineStatus::Found;
		if (found && (bestStage < 0 || best.status != EngineStatus::Found || r.grade > best.grade)) { best = std::move(r); bestStage = static_cast<int>(i); }
		else if (bestStage < 0) { best = std::move(r); bestStage = static_cast<int>(i); }
		if (GoodEnoughToStop(best, ref)) break;
	}
	if (best.status == EngineStatus::Found && bestStage > 0)
	{
		const std::string tag = stages[bestStage].name;
		if (best.cue.rfind("Engine (", 0) == 0) best.cue = "Engine (" + tag + ", " + best.cue.substr(8);
		else if (best.cue.rfind("Engine", 0) == 0) best.cue = "Engine (" + tag + ")" + best.cue.substr(6);
	}
	best.note += " Searched " + searched + (best.status == EngineStatus::Found && bestStage > 0 ? std::string("; the best line came ") + stages[bestStage].name + "." : ".");
	best.tries = tries;
	best.ms = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() * 1000.0f;
	return best;
}

static EngineResult LineOnce(const std::vector<Frame>& fr, bool realIn, bool floorBeforeGoal, int eventTick,
	const ShotReference& ref, int carBody, const std::atomic<bool>* cancel, float maxSeconds, int startTick, int depth)
{
	const int n = static_cast<int>(fr.size());
	if (n < 4) { EngineResult r; r.status = EngineStatus::NoBall; return r; }
	if (startTick >= 0) return LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, startTick, depth);

	// Default start: half a second before your touch, so the engine plays the shot you were already lined up for
	// ("Engine from here" in the viewer picks any other moment, e.g. the start of the rep for a different setup)
	ShotAnalysis real = ShotGeometry::Analyze(fr);
	int s = real.valid ? std::max(real.startTick, real.touchTick - 60) : 1;
#ifdef ENGINE_CONTACT_ONLY
	return LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, s, depth);
#else
	// Ceiling shots (Coop, Oct 25): one stage at a time, stopping as soon as one finds a good 5 or a really clean 4.
	// 1. The usual 0.5 s before the touch (it keeps your drop and only changes the flip and the hit).
	// 2. From the ceiling, just after you landed on it (it picks when and where to fall off).
	// 3. From just before the wall touch (it redoes the setup too).
	// Each stage gets a full search; the best line across the stages wins, a tie keeps the earlier stage (smaller change).
	if (real.valid && real.type == ShotType::CeilingShot)
		return CeilingStages(fr, real, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, s, depth);
	// Either start (Oct 20): the two starts find lines on different reps. From 0.5 s out the engine is stuck with your
	// approach; from your setup (takeoff, wall pop, the start of the dribble) it can redo the approach. So: search
	// from 0.5 s out first; unless that already found a 4 or 5, also search from your setup and keep the better line
	// (a tie keeps the 0.5 s line, the smaller change). Benchmark: aerials 66% -> 73%, double taps 57% -> 73%,
	// air dribbles about 65% -> 83%.
	int sm = Mechanics::MechanicStart(real, n);
	if (!real.valid || sm > s - 20)
		return LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, s, depth);
	auto t0 = std::chrono::steady_clock::now();
	EngineResult a, b;
	const int W = EngineThreads();
	if (W >= 4)
	{
		// Enough cores: both starts search side by side, each with the full time and half the workers
		std::thread tb([&] { LowPriority(); tThreads = W / 2; b = LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, sm, depth); });
		tThreads = W - W / 2;
		a = LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, s, depth);
		tThreads = 0;
		tb.join();
		if (a.status == EngineStatus::NoBall) return a;
		if (a.status == EngineStatus::Cancelled || b.status == EngineStatus::Cancelled || (cancel && cancel->load())) { a.status = EngineStatus::Cancelled; return a; }
	}
	else
	{
		// Few cores (fewer than 4 workers): one after the other. The 0.5 s search keeps its full time (giving it 60% cost
		// dribbles and flip resets lines, Oct 20), and the setup search only runs when that found no 4 or 5, with 60% more
		// time. Flip resets skip it: from the setup they find nothing yet.
		a = LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds, s, depth);
		if (a.status == EngineStatus::Cancelled || a.status == EngineStatus::NoBall || (cancel && cancel->load())) return a;
		if ((a.status == EngineStatus::Found && a.grade >= 4) || real.type == ShotType::FlipReset) return a;
		b = LineAt(fr, realIn, floorBeforeGoal, eventTick, ref, carBody, cancel, maxSeconds * 0.6f, sm, depth);
		if (b.status == EngineStatus::Cancelled) return b;
	}
	bool af = a.status == EngineStatus::Found, bf = b.status == EngineStatus::Found;
	bool useB = bf && (!af || b.grade > a.grade);
	EngineResult& best = useB ? b : a;
	if (useB && best.cue.rfind("Engine (", 0) == 0) best.cue = "Engine (from your setup, " + best.cue.substr(8);
	else if (useB && best.cue.rfind("Engine", 0) == 0) best.cue = "Engine (from your setup)" + best.cue.substr(6);
	best.note += useB ? (af ? " Also searched from your setup and found a higher grade there." : " Nothing better from 0.5 s before your touch, so it searched again from your setup.")
		: af ? " Also searched from your setup; the line from 0.5 s before your touch was as good or better."
		: " Searched from 0.5 s before your touch and from your setup.";
	best.tries = a.tries + b.tries;
	best.ms = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() * 1000.0f;
	return std::move(best);
#endif
}
