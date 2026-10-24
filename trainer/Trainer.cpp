// The ground-shot bot trial: PPO with our grader as the reward.
//
// 1. Warm start: the policy first copies your own inputs on your ground shots (so it starts out driving like you,
//    not mashing buttons), then
// 2. PPO: it plays your ground-shot starts (from the ball starting to move up to half a second before your touch,
//    mirrored left-right too, slightly jittered) and is rewarded by the grade the plugin's grader gives the shot.
//    A line that isn't a ground shot by the rulebook (Mechanics::SameMechanic: no ceiling or wall banks, no low
//    hovering, no tips) or never touches the ball scores -1, below any miss.
// 3. Every half hour it runs the benchmark on the 61 held-out ground shots it never trains on, with the engine's
//    rules: a goal, beats your grade, same mechanic, and holds up from 6 nudged, timing-shifted starts (4 of 6).
#include "Trainer.h"
#include "Bot.h"
#include "Data.h"
#include "LineSim.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

#ifdef _WIN32
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void* thread, int priority);
extern "C" __declspec(dllimport) unsigned long __stdcall SetThreadExecutionState(unsigned long flags);
static void LowPriority() { SetThreadPriority(GetCurrentThread(), -1); }   // below normal: the PC stays usable
// Keeps Windows from going to sleep while training (the screen can still turn off); cleared when the trainer exits
static void StayAwake(bool on) { SetThreadExecutionState(on ? (0x80000000ul | 0x00000001ul) : 0x80000000ul); }
#else
static void LowPriority() {}
static void StayAwake(bool) {}
#endif

namespace fs = std::filesystem;
using namespace RocketSim;

namespace
{
	std::atomic<bool> gStop{ false };
	void OnSignal(int) { gStop = true; }

	// ---------------- Settings (bot/train_config.txt, written with these defaults on the first run) ----------------
	struct Config
	{
		std::map<std::string, double> v = {
			{ "width", 256 }, { "depth", 3 },
			{ "steps_per_iter", 50000 }, { "minibatch", 10000 }, { "epochs", 2 },
			{ "lr_policy", 1e-4 }, { "lr_value", 3e-4 }, { "gamma", 0.995 }, { "lambda", 0.95 }, { "clip", 0.2 },
			{ "entropy", 0.002 }, { "max_grad_norm", 0.5 }, { "envs_per_thread", 8 },
			{ "jitter_car_pos", 12 }, { "jitter_car_vel", 40 }, { "jitter_ball_pos", 6 }, { "jitter_ball_vel", 25 },
			{ "delay_chance", 0.3 },
			{ "start_contact", 0.35 }, { "start_mech", 0.35 },
			{ "mirror", 1 },
			{ "bc_epochs", 6 }, { "bc_lr", 5e-4 },
			{ "reward_approach", 0.004 }, { "reward_touch", 0.2 }, { "reward_beat", 0.5 }, { "reward_credit", 0.3 },
			{ "eval_every_min", 30 }, { "eval_lines", 16 }, { "save_every_min", 10 },
		};
		double operator[](const char* k) const { auto it = v.find(k); return it == v.end() ? 0.0 : it->second; }
		void Load(const fs::path& f)
		{
			std::ifstream in(f);
			std::string line;
			while (std::getline(in, line))
			{
				auto h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
				auto e = line.find('='); if (e == std::string::npos) continue;
				std::string k = line.substr(0, e), val = line.substr(e + 1);
				auto trim = [](std::string& s) { while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++; s = s.substr(i); };
				trim(k); trim(val);
				if (!k.empty() && !val.empty()) v[k] = std::atof(val.c_str());
			}
		}
		void Save(const fs::path& f) const
		{
			std::ofstream o(f);
			o << "# Bot trainer settings. Edit and restart the trainer to use them (width/depth only apply to a fresh start).\n";
			for (auto& [k, x] : v) o << k << " = " << x << "\n";
		}
	};

	// ---------------- Checkpoints ----------------
	struct State
	{
		Bot::Policy pol;
		Adam optPi, optVf;
		long long steps = 0, iter = 0;
		double hours = 0;    // training time so far
		bool bcDone = false;
	};

	bool SaveState(const State& st, const fs::path& file)
	{
		fs::path tmp = file; tmp += ".tmp";
		std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
		if (!f) return false;
		const char magic[4] = { 'R', '2', 'B', '1' };
		bool ok = std::fwrite(magic, 1, 4, f) == 4 && std::fwrite(&st.steps, sizeof(st.steps), 1, f) == 1 && std::fwrite(&st.iter, sizeof(st.iter), 1, f) == 1
			&& std::fwrite(&st.hours, sizeof(st.hours), 1, f) == 1;
		const int bc = st.bcDone ? 1 : 0;
		ok = ok && std::fwrite(&bc, sizeof(bc), 1, f) == 1 && st.pol.norm.Write(f) && st.pol.pi.Write(f) && st.pol.vf.Write(f) && st.optPi.Write(f) && st.optVf.Write(f);
		std::fclose(f);
		if (!ok) return false;
		std::error_code ec;
		fs::rename(tmp, file, ec);
		if (ec) { fs::remove(file, ec); fs::rename(tmp, file, ec); }
		return !ec;
	}

	bool LoadState(State& st, const fs::path& file)
	{
		std::FILE* f = std::fopen(file.string().c_str(), "rb");
		if (!f) return false;
		char magic[4] = {};
		int bc = 0;
		bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "R2B1", 4) == 0 && std::fread(&st.steps, sizeof(st.steps), 1, f) == 1
			&& std::fread(&st.iter, sizeof(st.iter), 1, f) == 1 && std::fread(&st.hours, sizeof(st.hours), 1, f) == 1 && std::fread(&bc, sizeof(bc), 1, f) == 1
			&& st.pol.norm.Read(f) && st.pol.pi.Read(f) && st.pol.vf.Read(f);
		if (ok) { st.optPi.Init(st.pol.pi); st.optVf.Init(st.pol.vf); ok = st.optPi.Read(f) && st.optVf.Read(f); }
		std::fclose(f);
		st.bcDone = bc != 0;
		return ok;
	}

	// ---------------- Reward ----------------
	float Ramp(float v, float lo, float hi) { return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); }

	float FinalReward(const LineResult& r, const Rep& rep, const Config& cfg)
	{
		if (!r.touched || !r.sameMechanic) return -1.0f;
		const ShotAnalysis& a = r.shot;
		const float g = static_cast<float>(r.grade.overall);
		if (r.wentIn)
		{
			float credit = a.bouncedIn ? 0.0f : 2.0f * Ramp(a.crossZ, 200.0f, 520.0f) + Ramp(a.shotSpeed, 1500.0f, 2100.0f);
			return g + static_cast<float>(cfg["reward_credit"]) * credit + (r.grade.overall > rep.realGrade ? static_cast<float>(cfg["reward_beat"]) : 0.0f);
		}
		const float miss = a.reachesGoalLine ? std::min(a.missDist, 3000.0f) : 3000.0f;
		return g - 0.5f * miss / 3000.0f;
	}

	// ---------------- One training game ----------------
	struct Env
	{
		LineSim sim;
		Bot::Memory mem;
		Bot::BallPath path;
		const Rep* rep = nullptr;
		int s0 = 0, delay = 0;
		float gs = 1;
		CarControls lastCtl;
		bool live = false;
		float ret = 0;

		void Reset(const std::vector<Rep>& reps, std::mt19937& rng, const Config& cfg, Arena* ballArena)
		{
			std::uniform_int_distribution<size_t> pick(0, reps.size() - 1);
			rep = &reps[pick(rng)];
			std::uniform_real_distribution<float> u(0.0f, 1.0f);
			const float r = u(rng);
			const int a = rep->sMech, b = std::max(rep->sMech, rep->sContact);
			if (r < cfg["start_contact"]) s0 = b;
			else if (r < cfg["start_contact"] + cfg["start_mech"]) s0 = a;
			else s0 = a + static_cast<int>(u(rng) * (b - a + 1));
			s0 = std::clamp(s0, a, b);
			sim.Start(*rep, s0, 0);
			sim.Jitter(rng, static_cast<float>(cfg["jitter_car_pos"]), static_cast<float>(cfg["jitter_car_vel"]),
				static_cast<float>(cfg["jitter_ball_pos"]), static_cast<float>(cfg["jitter_ball_vel"]));
			path.Build(ballArena, sim.Now(), 480 + 130);
			mem.Seed(rep->fr, s0);
			gs = rep->real.valid ? rep->real.goalSign : 1.0f;
			delay = u(rng) < cfg["delay_chance"] ? 1 : 0;
			const Frame& p = rep->fr[std::max(0, s0 - 1)];
			lastCtl = CarControls();
			lastCtl.throttle = p.throttle; lastCtl.steer = p.steer; lastCtl.pitch = p.pitch; lastCtl.yaw = p.yaw; lastCtl.roll = p.roll;
			lastCtl.jump = p.jump; lastCtl.boost = p.boost; lastCtl.handbrake = p.handbrake;
			live = true;
			ret = 0;
		}
		void Observe(float* o) const { Bot::Observe(sim.Now(), mem, path, sim.TickNow() - s0, sim.Touched(), gs, o); }

		// Two ticks with this action. Returns the reward; done = the line is over (graded)
		float Step(const Bot::Act& a, const Config& cfg, bool& done)
		{
			const CarControls c = Bot::Controls(a);
			float r = 0;
			const bool touchedBefore = sim.Touched();
			bool alive = true;
			for (int k = 0; k < 2 && alive; k++)
			{
				const CarControls use = (k == 0 && delay) ? lastCtl : c;   // reaction delay: the new input lands a tick late
				alive = sim.Tick(use);
				const int t = sim.TickNow() - 1;
				mem.After(sim.Now(), sim.Result().ctl[t]);
				if (sim.HandsOff()) break;
			}
			lastCtl = c;
			if (!touchedBefore)
			{
				// Shaping: a little for closing on the ball before the touch, a little for the touch itself
				const Frame& f = sim.Now();
				const float dx = f.ballX - f.carX, dy = f.ballY - f.carY, dz = f.ballZ - f.carZ;
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-3f;
				const float closing = (f.carVX * dx + f.carVY * dy + f.carVZ * dz) / d;
				r += static_cast<float>(cfg["reward_approach"]) * std::clamp(closing / 2300.0f, -1.0f, 1.0f);
				if (sim.Touched()) r += static_cast<float>(cfg["reward_touch"]);
			}
			done = !alive || sim.HandsOff();
			if (done)
			{
				const LineResult& lr = sim.Finish();
				r += FinalReward(lr, *rep, cfg);
				live = false;
			}
			ret += r;
			return r;
		}
	};

	// ---------------- A worker thread's share of the batch ----------------
	struct Trans { float obs[Bot::kObs]; Bot::Act act; float logp, rew, val; bool done; };
	struct EpStats { long long n = 0, touched = 0, same = 0, goals = 0, high = 0, beat = 0, kind = 0, hover = 0, bank = 0, tip = 0; double grade = 0, ret = 0; };

	// ---------------- The benchmark: the bot on the held-out reps, engine rules ----------------
	struct EvalRow { std::string file; int realGrade = 0; int foundContact = 0, foundMech = 0; int best = 0; int greedyGoal = 0; };

	// A line by the policy from tick s. k = 0: its top choice every time; k > 0: sampled (seeded, so it replays)
	LineResult PlayLine(const Bot::Policy& pol, const Rep& rep, int s, int k, LineSim& sim, Arena* ballArena, MLP::Work& w)
	{
		std::mt19937 rng(static_cast<unsigned>(std::hash<std::string>()(rep.file)) ^ (static_cast<unsigned>(s) * 2654435761u) ^ (static_cast<unsigned>(k) * 40503u));
		sim.Start(rep, s, 0);
		Bot::Memory mem; mem.Seed(rep.fr, s);
		Bot::BallPath path; path.Build(ballArena, sim.Now(), 480 + 130);
		const float gs = rep.real.valid ? rep.real.goalSign : 1.0f;
		float obs[Bot::kObs], lp[Bot::kLogits];
		std::vector<float> logits;
		bool alive = true;
		while (alive && !sim.HandsOff())
		{
			Bot::Observe(sim.Now(), mem, path, sim.TickNow() - s, sim.Touched(), gs, obs);
			pol.norm.Apply(obs);
			pol.Logits(obs, 1, w, logits);
			Bot::LogSoftmax(logits.data(), lp);
			const Bot::Act a = k == 0 ? Bot::Greedy(lp) : Bot::Sample(lp, rng);
			const CarControls c = Bot::Controls(a);
			for (int t = 0; t < 2 && alive; t++)
			{
				alive = sim.Tick(c);
				mem.After(sim.Now(), sim.Result().ctl[sim.TickNow() - 1]);
				if (sim.HandsOff()) break;
			}
		}
		return sim.Finish();
	}

	// The engine's check: replay the line's inputs from 6 nudged starts, a tick or two early or late; 4 must score
	// within a grade of it, and the plain replay must still be a better, same-mechanic goal
	int HeldUp(const Rep& rep, int s, const LineResult& line, LineSim& sim)
	{
		static const int kShift[6] = { -2, 2, -1, 1, 0, 0 };
		const int expect = line.grade.overall;
		const std::vector<CarControls>& c0 = line.ctl;
		auto replay = [&](int nudge, int shift) -> const LineResult& {
			sim.Start(rep, s, nudge);
			sim.ReplayMode();
			const int n = static_cast<int>(c0.size());
			for (int i = s;; i++)
			{
				const CarControls c = i < n ? c0[std::clamp(i - shift, s, n - 1)] : CarControls();
				if (!sim.Tick(c)) break;
			}
			return sim.Finish();
		};
		int held = 0;
		for (int j = 1; j <= 6; j++)
		{
			const LineResult& r = replay(300 + j, kShift[j - 1]);
			held += r.wentIn && r.grade.overall >= expect - 1;
			if (j - held > 2) return 0;
		}
		if (held < 4) return 0;
		const LineResult& win = replay(0, 0);
		if (!win.wentIn || win.grade.overall <= rep.realGrade || !win.sameMechanic) return 0;
		return win.grade.overall;
	}

	std::vector<EvalRow> RunEval(const Bot::Policy& pol, const std::vector<Rep>& held, int lines, int threads)
	{
		std::vector<EvalRow> rows(held.size());
		std::atomic<size_t> next{ 0 };
		auto work = [&] {
			LowPriority();
			LineSim sim, chk;
			std::unique_ptr<Arena> ballArena(Arena::Create(GameMode::SOCCAR));
			MLP::Work w;
			for (size_t i; (i = next.fetch_add(1)) < held.size();)
			{
				const Rep& rep = held[i];
				EvalRow& row = rows[i];
				row.file = rep.file;
				row.realGrade = rep.realGrade;
				for (int which = 0; which < 2; which++)
				{
					const int s = which == 0 ? rep.sContact : rep.sMech;
					if (which == 1 && rep.sMech > rep.sContact - 20) continue;   // the same start, as the engine does
					struct Cand { int grade; float q; LineResult r; };
					std::vector<Cand> cands;
					for (int k = 0; k < lines; k++)
					{
						LineResult r = PlayLine(pol, rep, s, k, sim, ballArena.get(), w);
						if (k == 0 && which == 0) row.greedyGoal = r.wentIn && r.sameMechanic ? r.grade.overall : 0;
						if (r.wentIn && r.sameMechanic && r.grade.overall > rep.realGrade) cands.push_back({ r.grade.overall, r.quality, std::move(r) });
					}
					std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.grade != b.grade ? a.grade > b.grade : a.q > b.q; });
					int found = 0;
					for (const Cand& c : cands) if ((found = HeldUp(rep, s, c.r, chk)) > 0) break;
					(which == 0 ? row.foundContact : row.foundMech) = found;
				}
				row.best = std::max(row.foundContact, row.foundMech);
			}
		};
		std::vector<std::thread> ts;
		for (int t = 0; t < std::max(1, threads); t++) ts.emplace_back(work);
		for (auto& t : ts) t.join();
		return rows;
	}

	struct EvalSum { int reps = 0, found = 0, high = 0, foundContact = 0, foundMech = 0, greedyGoals = 0; };
	EvalSum Sum(const std::vector<EvalRow>& rows)
	{
		EvalSum s;
		for (const EvalRow& r : rows)
		{
			s.reps++;
			s.found += r.best > 0; s.high += r.best >= 4;
			s.foundContact += r.foundContact > 0; s.foundMech += r.foundMech > 0;
			s.greedyGoals += r.greedyGoal > 0;
		}
		return s;
	}

	// ---------------- Warm start: copy your inputs ----------------
	void WarmStart(State& st, const std::vector<Rep>& reps, const Config& cfg, int threads)
	{
		std::vector<float> X;
		std::vector<Bot::Act> Y;
		std::unique_ptr<Arena> ballArena(Arena::Create(GameMode::SOCCAR));
		for (const Rep& rep : reps)
		{
			if (!rep.real.valid || rep.real.touchTick < 0) continue;
			const int s = rep.sMech, end = std::min(rep.real.touchTick + 3, static_cast<int>(rep.fr.size()) - 2);
			if (end <= s) continue;
			Bot::BallPath path; path.Build(ballArena.get(), rep.fr[s], 480 + 130);
			Bot::Memory mem; mem.Seed(rep.fr, s);
			const float gs = rep.real.goalSign;
			for (int t = s; t <= end; t++)
			{
				float o[Bot::kObs];
				Bot::Observe(rep.fr[t], mem, path, t - s, t > rep.real.touchTick, gs, o);
				X.insert(X.end(), o, o + Bot::kObs);
				Y.push_back(Bot::FromControls(rep.fr[t]));
				CarControls c = Bot::Controls(Y.back());
				mem.After(rep.fr[t + 1], c);
			}
		}
		const int N = static_cast<int>(Y.size());
		std::printf("Warm start: copying your inputs on %d ticks from %zu reps\n", N, reps.size());
		st.pol.norm.Add(X.data(), N);
		for (int i = 0; i < N; i++) st.pol.norm.Apply(&X[static_cast<size_t>(i) * Bot::kObs]);
		Adam opt; opt.Init(st.pol.pi); opt.lr = static_cast<float>(cfg["bc_lr"]);
		std::mt19937 rng(7);
		std::vector<int> idx(N);
		std::iota(idx.begin(), idx.end(), 0);
		const int mb = 2048, T = std::max(1, threads);
		for (int ep = 0; ep < static_cast<int>(cfg["bc_epochs"]) && !gStop; ep++)
		{
			std::shuffle(idx.begin(), idx.end(), rng);
			double lossSum = 0; long long right = 0;
			for (int b0 = 0; b0 < N; b0 += mb)
			{
				const int B = std::min(mb, N - b0);
				std::vector<std::vector<std::vector<float>>> gW(T), gb(T);
				std::vector<double> loss(T, 0); std::vector<long long> ok(T, 0);
				std::vector<std::thread> ts;
				for (int t = 0; t < T; t++)
					ts.emplace_back([&, t] {
						st.pol.pi.ZeroLike(gW[t], gb[t]);
						const int lo = B * t / T, hi = B * (t + 1) / T, n = hi - lo;
						if (n <= 0) return;
						std::vector<float> x(static_cast<size_t>(n) * Bot::kObs), d(static_cast<size_t>(n) * Bot::kLogits);
						for (int i = 0; i < n; i++) std::memcpy(&x[static_cast<size_t>(i) * Bot::kObs], &X[static_cast<size_t>(idx[b0 + lo + i]) * Bot::kObs], sizeof(float) * Bot::kObs);
						MLP::Work w;
						const float* z = st.pol.pi.Forward(x.data(), n, w);
						for (int i = 0; i < n; i++)
						{
							float lp[Bot::kLogits];
							Bot::LogSoftmax(z + static_cast<size_t>(i) * Bot::kLogits, lp);
							const Bot::Act& y = Y[idx[b0 + lo + i]];
							const Bot::Act g = Bot::Greedy(lp);
							ok[t] += g == y;
							for (int h = 0; h < Bot::kHeads; h++)
							{
								loss[t] -= lp[Bot::kOff[h] + y[h]];
								for (int j = 0; j < Bot::kBins[h]; j++)
									d[static_cast<size_t>(i) * Bot::kLogits + Bot::kOff[h] + j] = (std::exp(lp[Bot::kOff[h] + j]) - (j == y[h] ? 1.0f : 0.0f)) / B;
							}
						}
						st.pol.pi.Backward(d.data(), n, w, gW[t], gb[t]);
					});
				for (auto& th : ts) th.join();
				for (int t = 1; t < T; t++)
					for (size_t l = 0; l < gW[0].size(); l++)
					{
						for (size_t i = 0; i < gW[0][l].size(); i++) gW[0][l][i] += gW[t][l][i];
						for (size_t i = 0; i < gb[0][l].size(); i++) gb[0][l][i] += gb[t][l][i];
					}
				opt.Step(st.pol.pi, gW[0], gb[0], 1.0f);
				for (int t = 0; t < T; t++) { lossSum += loss[t]; right += ok[t]; }
			}
			std::printf("  warm start pass %d: loss %.3f per tick, exact match on all 8 inputs %.1f%%\n", ep + 1, lossSum / N, 100.0 * right / N);
			std::fflush(stdout);
		}
		st.bcDone = true;
	}

	std::string Now()
	{
		std::time_t t = std::time(nullptr);
		char buf[64];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
		return buf;
	}
}

// ======================================================================================================

int Trainer::Train(const Args& a, const fs::path& dir)
{
	LowPriority();
	StayAwake(true);
	struct Awake { ~Awake() { StayAwake(false); } } awake;
	std::signal(SIGINT, OnSignal);
#ifdef SIGBREAK
	std::signal(SIGBREAK, OnSignal);
#endif
	fs::create_directories(a.out);
	const fs::path cfgFile = fs::path(a.out) / "train_config.txt";
	Config cfg;
	if (!a.config.empty()) cfg.Load(a.config);
	else if (fs::exists(cfgFile)) cfg.Load(cfgFile);
	cfg.Save(cfgFile);

	ShotBook book;
	auto heldNames = Data::ReadList(Data::HeldoutFile(dir, ShotType::Ground));
	std::vector<Rep> train = Data::Load(a.data, book, ShotType::Ground, heldNames, 2, a.threads);
	std::vector<Rep> held = Data::Load(a.data, book, ShotType::Ground, heldNames, 1, a.threads);
	if (cfg["mirror"] > 0)
	{
		const size_t n = train.size();
		for (size_t i = 0; i < n; i++)
		{
			Rep m = train[i];
			m.fr = Data::Mirror(m.fr);
			m.mirrored = true;
			Data::Finish(m, book, "");   // keeps the shot reference
			if (m.real.valid && m.real.type == ShotType::Ground) train.push_back(std::move(m));
		}
	}
	std::printf("[%s] Ground-shot bot trial. %zu training starts (mirrored included), %zu held-out reps, %d threads.\n",
		Now().c_str(), train.size(), held.size(), a.threads);
	if (train.empty() || held.empty()) { std::printf("No reps found in %s\n", a.data.c_str()); return 1; }

	State st;
	const fs::path latest = fs::path(a.out) / "latest.bin";
	const fs::path loadFrom = !a.ckpt.empty() ? fs::path(a.ckpt) : latest;
	if (fs::exists(loadFrom) && LoadState(st, loadFrom))
		std::printf("Resumed from %s: %lld steps, %.2f h trained.\n", loadFrom.string().c_str(), st.steps, st.hours);
	else
	{
		std::mt19937 rng(20261023);
		st.pol.Init(rng, static_cast<int>(cfg["width"]), static_cast<int>(cfg["depth"]));
		st.optPi.Init(st.pol.pi);
		st.optVf.Init(st.pol.vf);
		std::printf("Fresh start: policy %zu weights, value %zu weights.\n", st.pol.pi.ParamCount(), st.pol.vf.ParamCount());
	}
	st.optPi.lr = static_cast<float>(cfg["lr_policy"]);
	st.optVf.lr = static_cast<float>(cfg["lr_value"]);

	const fs::path evalLog = fs::path(a.out) / "eval_log.csv", trainLog = fs::path(a.out) / "train_log.csv";
	auto evalNow = [&](const char* label) {
		auto t0 = std::chrono::steady_clock::now();
		std::vector<EvalRow> rows = RunEval(st.pol, held, static_cast<int>(cfg["eval_lines"]), a.threads);
		EvalSum s = Sum(rows);
		const float sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
		const bool fresh = !fs::exists(evalLog);
		std::ofstream o(evalLog, std::ios::app);
		if (fresh) o << "time,label,steps,hours,reps,better_line,to_4_or_5,from_contact,from_setup,greedy_goals\n";
		o << Now() << "," << label << "," << st.steps << "," << st.hours << "," << s.reps << "," << s.found << "," << s.high << ","
			<< s.foundContact << "," << s.foundMech << "," << s.greedyGoals << "\n";
		std::printf("[%s] Held-out check (%s, %.0f s): better line on %d of %d (%.0f%%), %d to a 4 or 5 (%.0f%%). Its first-choice line scores on %d.\n",
			Now().c_str(), label, sec, s.found, s.reps, 100.0 * s.found / s.reps, s.high, 100.0 * s.high / s.reps, s.greedyGoals);
		std::fflush(stdout);
		return s;
	};

	if (!st.bcDone && cfg["bc_epochs"] > 0)
	{
		WarmStart(st, train, cfg, a.threads);
		SaveState(st, latest);
		evalNow("after warm start");
	}

	// Best so far (by the held-out check): kept as best.bin
	int bestScore = -1;
	{
		std::ifstream in(fs::path(a.out) / "best_score.txt");
		in >> bestScore;
	}

	const int T = std::max(1, a.threads);
	const int E = std::max(1, static_cast<int>(cfg["envs_per_thread"]));
	std::vector<std::vector<Env>> envs(T);
	std::vector<std::mt19937> rngs;
	std::vector<std::unique_ptr<Arena>> ballArenas;
	for (int t = 0; t < T; t++) ballArenas.emplace_back(Arena::Create(GameMode::SOCCAR));
	for (int t = 0; t < T; t++) { envs[t] = std::vector<Env>(E); rngs.emplace_back(1234567u + static_cast<unsigned>(t) * 7919u + static_cast<unsigned>(st.iter) * 104729u); }

	auto clock0 = std::chrono::steady_clock::now();
	auto lastSave = clock0, lastEval = clock0;
	const double hours0 = st.hours;
	auto elapsedH = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count() / 3600.0; };

	while (!gStop)
	{
		if (a.hours > 0 && elapsedH() >= a.hours) break;
		if (a.maxSteps > 0 && st.steps >= a.maxSteps) break;
		auto it0 = std::chrono::steady_clock::now();

		// ---- Collect: every thread plays its games with the current policy ----
		const long long target = static_cast<long long>(cfg["steps_per_iter"]);
		std::atomic<long long> count{ 0 };
		std::vector<std::vector<std::vector<Trans>>> traj(T, std::vector<std::vector<Trans>>(E));
		std::vector<std::vector<std::array<float, Bot::kObs>>> bootObs(T, std::vector<std::array<float, Bot::kObs>>(E));
		std::vector<std::vector<char>> needBoot(T, std::vector<char>(E, 0));
		std::vector<EpStats> eps(T);
		{
			std::vector<std::thread> ts;
			for (int t = 0; t < T; t++)
				ts.emplace_back([&, t] {
					LowPriority();
					std::mt19937& rng = rngs[t];
					MLP::Work w;
					Arena* ballArena = ballArenas[t].get();
					std::vector<float> raw(static_cast<size_t>(E) * Bot::kObs), x(raw.size()), logits;
					while (count.load() < target && !gStop)
					{
						for (int e = 0; e < E; e++)
						{
							Env& env = envs[t][e];
							if (!env.live) env.Reset(train, rng, cfg, ballArena);
							env.Observe(&raw[static_cast<size_t>(e) * Bot::kObs]);
						}
						x = raw;
						for (int e = 0; e < E; e++) st.pol.norm.Apply(&x[static_cast<size_t>(e) * Bot::kObs]);
						st.pol.Logits(x.data(), E, w, logits);
						for (int e = 0; e < E; e++)
						{
							float lp[Bot::kLogits];
							Bot::LogSoftmax(&logits[static_cast<size_t>(e) * Bot::kLogits], lp);
							Trans tr;
							std::memcpy(tr.obs, &raw[static_cast<size_t>(e) * Bot::kObs], sizeof(tr.obs));
							tr.act = Bot::Sample(lp, rng);
							tr.logp = Bot::LogProb(lp, tr.act);
							bool done = false;
							Env& env = envs[t][e];
							tr.rew = env.Step(tr.act, cfg, done);
							tr.done = done;
							tr.val = 0;
							traj[t][e].push_back(tr);
							if (done)
							{
								const LineResult& lr = env.sim.Result();
								EpStats& s = eps[t];
								s.n++; s.touched += lr.touched; s.same += lr.touched && lr.sameMechanic;
								const bool good = lr.touched && lr.sameMechanic;
								s.goals += good && lr.wentIn; s.high += good && lr.wentIn && lr.grade.overall >= 4;
								s.beat += good && lr.wentIn && lr.grade.overall > env.rep->realGrade;
								s.grade += good ? lr.grade.overall : 0; s.ret += env.ret;
								if (lr.touched && !lr.sameMechanic)
								{
									s.kind += lr.why.find("different kind") != std::string::npos;
									s.hover += lr.why.find("hovered") != std::string::npos;
									s.bank += lr.why.find("banked") != std::string::npos;
									s.tip += lr.why.find("tip") != std::string::npos;
								}
							}
						}
						count += E;
					}
					for (int e = 0; e < E; e++)
					{
						Env& env = envs[t][e];
						needBoot[t][e] = env.live && !traj[t][e].empty() && !traj[t][e].back().done;
						if (needBoot[t][e]) env.Observe(bootObs[t][e].data());
					}
				});
			for (auto& th : ts) th.join();
		}
		if (gStop) break;
		auto it1 = std::chrono::steady_clock::now();

		// ---- Flatten, normalize with the stats the policy just used, values, advantages ----
		std::vector<Trans*> all;
		for (int t = 0; t < T; t++) for (int e = 0; e < E; e++) for (Trans& tr : traj[t][e]) all.push_back(&tr);
		const int N = static_cast<int>(all.size());
		std::vector<float> X(static_cast<size_t>(N) * Bot::kObs);
		for (int i = 0; i < N; i++) { std::memcpy(&X[static_cast<size_t>(i) * Bot::kObs], all[i]->obs, sizeof(float) * Bot::kObs); st.pol.norm.Apply(&X[static_cast<size_t>(i) * Bot::kObs]); }
		// values for every step and for the bootstrap states, in parallel
		std::vector<float> V(N);
		std::vector<std::vector<float>> bootV(T, std::vector<float>(E, 0.0f));
		{
			std::vector<std::thread> ts;
			for (int t = 0; t < T; t++)
				ts.emplace_back([&, t] {
					MLP::Work w;
					const int lo = static_cast<int>(static_cast<long long>(N) * t / T), hi = static_cast<int>(static_cast<long long>(N) * (t + 1) / T);
					for (int b0 = lo; b0 < hi; b0 += 1024)
					{
						const int B = std::min(1024, hi - b0);
						const float* v = st.pol.vf.Forward(&X[static_cast<size_t>(b0) * Bot::kObs], B, w);
						for (int i = 0; i < B; i++) V[b0 + i] = v[i];
					}
					for (int e = 0; e < E; e++)
						if (needBoot[t][e])
						{
							float o[Bot::kObs];
							std::memcpy(o, bootObs[t][e].data(), sizeof(o));
							st.pol.norm.Apply(o);
							bootV[t][e] = st.pol.vf.Forward(o, 1, w)[0];
						}
				});
			for (auto& th : ts) th.join();
		}
		for (int i = 0; i < N; i++) all[i]->val = V[i];
		std::vector<float> adv(N), ret(N);
		{
			const float gamma = static_cast<float>(cfg["gamma"]), lam = static_cast<float>(cfg["lambda"]);
			int base = 0;
			for (int t = 0; t < T; t++)
				for (int e = 0; e < E; e++)
				{
					std::vector<Trans>& tj = traj[t][e];
					const int n = static_cast<int>(tj.size());
					float gae = 0, nextV = needBoot[t][e] ? bootV[t][e] : 0.0f;
					for (int i = n - 1; i >= 0; i--)
					{
						const float nv = tj[i].done ? 0.0f : nextV;
						if (tj[i].done) gae = 0;
						const float delta = tj[i].rew + gamma * nv - tj[i].val;
						gae = delta + gamma * lam * gae;
						adv[base + i] = gae;
						ret[base + i] = gae + tj[i].val;
						nextV = tj[i].val;
					}
					base += n;
				}
		}
		{
			double m = 0, s2 = 0;
			for (float x : adv) m += x;
			m /= std::max(1, N);
			for (float x : adv) s2 += (x - m) * (x - m);
			const float sd = static_cast<float>(std::sqrt(s2 / std::max(1, N)) + 1e-8);
			for (float& x : adv) x = static_cast<float>((x - m) / sd);
		}

		// ---- PPO update ----
		const int mb = std::max(256, static_cast<int>(cfg["minibatch"]));
		const float clip = static_cast<float>(cfg["clip"]), entC = static_cast<float>(cfg["entropy"]);
		double piLoss = 0, vLoss = 0, ent = 0, kl = 0, clipFrac = 0; long long counted = 0;
		std::vector<int> idx(N);
		std::iota(idx.begin(), idx.end(), 0);
		std::mt19937 shuf(static_cast<unsigned>(st.iter) + 99u);
		for (int ep = 0; ep < static_cast<int>(cfg["epochs"]) && !gStop; ep++)
		{
			std::shuffle(idx.begin(), idx.end(), shuf);
			for (int b0 = 0; b0 + mb / 2 <= N; b0 += mb)
			{
				const int B = std::min(mb, N - b0);
				std::vector<std::vector<std::vector<float>>> pgW(T), pgb(T), vgW(T), vgb(T);
				std::vector<double> sPi(T, 0), sV(T, 0), sEnt(T, 0), sKl(T, 0), sClip(T, 0);
				std::vector<std::thread> ts;
				for (int t = 0; t < T; t++)
					ts.emplace_back([&, t] {
						LowPriority();
						st.pol.pi.ZeroLike(pgW[t], pgb[t]);
						st.pol.vf.ZeroLike(vgW[t], vgb[t]);
						const int lo = B * t / T, hi = B * (t + 1) / T, n = hi - lo;
						if (n <= 0) return;
						std::vector<float> x(static_cast<size_t>(n) * Bot::kObs), dz(static_cast<size_t>(n) * Bot::kLogits), dv(n);
						for (int i = 0; i < n; i++) std::memcpy(&x[static_cast<size_t>(i) * Bot::kObs], &X[static_cast<size_t>(idx[b0 + lo + i]) * Bot::kObs], sizeof(float) * Bot::kObs);
						MLP::Work w;
						const float* z = st.pol.pi.Forward(x.data(), n, w);
						for (int i = 0; i < n; i++)
						{
							const int k = idx[b0 + lo + i];
							float lp[Bot::kLogits];
							Bot::LogSoftmax(z + static_cast<size_t>(i) * Bot::kLogits, lp);
							const Bot::Act& act = all[k]->act;
							const float newLp = Bot::LogProb(lp, act);
							const float logRatio = std::clamp(newLp - all[k]->logp, -20.0f, 20.0f);
							const float ratio = std::exp(logRatio);
							const float A = adv[k];
							const float clipped = std::clamp(ratio, 1.0f - clip, 1.0f + clip);
							const bool useUnclipped = ratio * A <= clipped * A;
							sPi[t] += -std::min(ratio * A, clipped * A);
							sKl[t] += (ratio - 1.0f) - logRatio;
							sClip[t] += std::fabs(ratio - 1.0f) > clip;
							const float gLp = useUnclipped ? -A * ratio : 0.0f;   // d loss / d log prob
							// entropy per head, and its gradient: d(-c*H)/dz_j = c * p_j * (log p_j + H)
							float* d = &dz[static_cast<size_t>(i) * Bot::kLogits];
							for (int h = 0; h < Bot::kHeads; h++)
							{
								const int o = Bot::kOff[h], nb = Bot::kBins[h];
								float H = 0;
								for (int j = 0; j < nb; j++) H -= std::exp(lp[o + j]) * lp[o + j];
								sEnt[t] += H;
								for (int j = 0; j < nb; j++)
								{
									const float p = std::exp(lp[o + j]);
									d[o + j] = (gLp * ((j == act[h] ? 1.0f : 0.0f) - p) + entC * p * (lp[o + j] + H)) / B;
								}
							}
						}
						st.pol.pi.Backward(dz.data(), n, w, pgW[t], pgb[t]);
						const float* v = st.pol.vf.Forward(x.data(), n, w);
						for (int i = 0; i < n; i++)
						{
							const int k = idx[b0 + lo + i];
							const float e = v[i] - ret[k];
							sV[t] += 0.5 * e * e;
							dv[i] = e / B;
						}
						st.pol.vf.Backward(dv.data(), n, w, vgW[t], vgb[t]);
					});
				for (auto& th : ts) th.join();
				auto reduce = [&](std::vector<std::vector<std::vector<float>>>& gW, std::vector<std::vector<std::vector<float>>>& gb) {
					for (int t = 1; t < T; t++)
						for (size_t l = 0; l < gW[0].size(); l++)
						{
							for (size_t i = 0; i < gW[0][l].size(); i++) gW[0][l][i] += gW[t][l][i];
							for (size_t i = 0; i < gb[0][l].size(); i++) gb[0][l][i] += gb[t][l][i];
						}
				};
				reduce(pgW, pgb); reduce(vgW, vgb);
				const float maxNorm = static_cast<float>(cfg["max_grad_norm"]);
				st.optPi.Step(st.pol.pi, pgW[0], pgb[0], maxNorm);
				st.optVf.Step(st.pol.vf, vgW[0], vgb[0], maxNorm);
				for (int t = 0; t < T; t++) { piLoss += sPi[t]; vLoss += sV[t]; ent += sEnt[t]; kl += sKl[t]; clipFrac += sClip[t]; }
				counted += B;
			}
		}
		// The new observations go into the running scale only now (the update used the stats the games were played with)
		{
			std::vector<float> rawAll(static_cast<size_t>(N) * Bot::kObs);
			for (int i = 0; i < N; i++) std::memcpy(&rawAll[static_cast<size_t>(i) * Bot::kObs], all[i]->obs, sizeof(float) * Bot::kObs);
			st.pol.norm.Add(rawAll.data(), N);
		}
		auto it2 = std::chrono::steady_clock::now();
		st.steps += N;
		st.iter++;
		st.hours = hours0 + elapsedH();

		EpStats s;
		for (const EpStats& e : eps) { s.n += e.n; s.touched += e.touched; s.same += e.same; s.goals += e.goals; s.high += e.high; s.beat += e.beat; s.grade += e.grade; s.ret += e.ret; s.kind += e.kind; s.hover += e.hover; s.bank += e.bank; s.tip += e.tip; }
		const float collectS = std::chrono::duration<float>(it1 - it0).count(), updateS = std::chrono::duration<float>(it2 - it1).count();
		const double sps = N / std::max(1e-3f, collectS + updateS);
		const double c = std::max<long long>(1, counted);
		auto pct = [&](long long x) { return 100.0 * x / std::max<long long>(1, s.n); };
		{
			std::ofstream so(fs::path(a.out) / "status.txt");
			so << Now() << ": " << st.steps << " steps, " << st.hours << " h trained, " << sps << " steps/s. Last batch: "
				<< s.n << " shots, ground-shot touch " << pct(s.same) << "%, goal " << pct(s.goals) << "%, 4-5 " << pct(s.high) << "%, beat you "
				<< pct(s.beat) << "%.\n";
		}
		std::printf("[%s] it %lld | %.1fM steps | %.0f steps/s (play %.1fs, learn %.1fs) | %lld shots: touch %.0f%% (not a ground shot: kind %.0f%% hover %.0f%% bank %.0f%% tip %.0f%%), goal %.0f%%, 4-5 %.0f%%, beat you %.0f%% | reward %.2f | ent %.2f kl %.4f clip %.2f\n",
			Now().c_str(), st.iter, st.steps / 1e6, sps, collectS, updateS, s.n, pct(s.touched), pct(s.kind), pct(s.hover), pct(s.bank), pct(s.tip), pct(s.goals), pct(s.high), pct(s.beat),
			s.ret / std::max<long long>(1, s.n), ent / c, kl / c, clipFrac / c);
		std::fflush(stdout);
		{
			const bool fresh = !fs::exists(trainLog);
			std::ofstream o(trainLog, std::ios::app);
			if (fresh) o << "time,iter,steps,hours,steps_per_s,shots,touch_pct,same_mech_pct,goal_pct,high_pct,beat_pct,avg_reward,policy_loss,value_loss,entropy,kl,clip_frac\n";
			o << Now() << "," << st.iter << "," << st.steps << "," << st.hours << "," << sps << "," << s.n << "," << pct(s.touched) << "," << pct(s.same) << "," << pct(s.goals) << ","
				<< pct(s.high) << "," << pct(s.beat) << "," << s.ret / std::max<long long>(1, s.n) << "," << piLoss / c << "," << vLoss / c << "," << ent / c << ","
				<< kl / c << "," << clipFrac / c << "\n";
		}

		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - lastSave).count() >= cfg["save_every_min"] * 60.0)
		{
			SaveState(st, latest);
			lastSave = now;
		}
		if (std::chrono::duration<double>(now - lastEval).count() >= cfg["eval_every_min"] * 60.0)
		{
			EvalSum es = evalNow("training");
			const int score = es.found * 100 + es.high;
			SaveState(st, fs::path(a.out) / std::format("ckpt_{:.0f}M.bin", st.steps / 1e6));
			if (score > bestScore)
			{
				bestScore = score;
				SaveState(st, fs::path(a.out) / "best.bin");
				std::ofstream(fs::path(a.out) / "best_score.txt") << bestScore << "\n";
			}
			lastEval = std::chrono::steady_clock::now();
		}
	}
	SaveState(st, latest);
	std::printf("[%s] Stopped and saved: %lld steps, %.2f h trained. Run the trainer again to pick up where it left off.\n", Now().c_str(), st.steps, st.hours);
	return 0;
}

int Trainer::EvalBot(const Args& a, const fs::path& dir)
{
	State st;
	const fs::path file = !a.ckpt.empty() ? fs::path(a.ckpt) : fs::path(a.out) / "best.bin";
	if (!LoadState(st, file)) { std::printf("Can't load %s\n", file.string().c_str()); return 1; }
	ShotBook book;
	auto heldNames = Data::ReadList(Data::HeldoutFile(dir, ShotType::Ground));
	std::vector<Rep> held = Data::Load(a.data, book, ShotType::Ground, heldNames, 1, a.threads);
	std::printf("Bot %s (%lld steps) on %zu held-out ground shots, %d lines per start...\n", file.string().c_str(), st.steps, held.size(), a.lines);
	std::vector<EvalRow> rows = RunEval(st.pol, held, a.lines, a.threads);
	EvalSum s = Sum(rows);

	// Side by side with the engine, if its benchmark has been run (enginebench)
	std::map<std::string, int> eng;
	{
		std::ifstream in(fs::path(a.out) / "enginebench.csv");
		std::string line;
		std::getline(in, line);
		while (std::getline(in, line))
		{
			std::vector<std::string> p; std::stringstream ss(line); std::string c;
			while (std::getline(ss, c, ',')) p.push_back(c);
			if (p.size() < 6) continue;
			const std::string f = p[0].substr(0, p[0].find('#'));
			eng[f] = std::max(eng[f], p[4] == "1" ? std::atoi(p[5].c_str()) : 0);
		}
	}
	fs::create_directories(a.out);
	std::ofstream o(fs::path(a.out) / ("evalbot" + a.tag + ".csv"));
	o << "file,your_grade,bot_from_contact,bot_from_setup,bot_best,engine_best,either\n";
	int either = 0, eitherHigh = 0, engFound = 0, engHigh = 0, botOnly = 0, engOnly = 0, both = 0;
	for (const EvalRow& r : rows)
	{
		const int e = eng.count(r.file) ? eng[r.file] : -1;
		const int x = std::max(r.best, std::max(0, e));
		o << r.file << "," << r.realGrade << "," << r.foundContact << "," << r.foundMech << "," << r.best << "," << e << "," << x << "\n";
		either += x > 0; eitherHigh += x >= 4;
		if (e >= 0) { engFound += e > 0; engHigh += e >= 4; botOnly += r.best > 0 && e == 0; engOnly += e > 0 && r.best == 0; both += e > 0 && r.best > 0; }
	}
	std::printf("Bot: better line on %d of %d (%.0f%%), %d to a 4 or 5 (%.0f%%); from 0.5 s out %d, from the setup %d. First-choice line scores on %d.\n",
		s.found, s.reps, 100.0 * s.found / s.reps, s.high, 100.0 * s.high / s.reps, s.foundContact, s.foundMech, s.greedyGoals);
	if (!eng.empty())
	{
		std::printf("Engine: %d (%d to a 4 or 5). Either: %d of %d (%.0f%%), %d to a 4 or 5 (%.0f%%). Bot only %d, engine only %d, both %d.\n",
			engFound, engHigh, either, s.reps, 100.0 * either / s.reps, eitherHigh, 100.0 * eitherHigh / s.reps, botOnly, engOnly, both);
	}
	return 0;
}
