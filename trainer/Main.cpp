// RLCoachTrainer: the ground-shot bot trial (Phase 2b) and its benchmarks.
//
//   RLCoachTrainer train      [--data DIR] [--out DIR] [--threads N] [--hours H]   train (resumes from the last checkpoint)
//   RLCoachTrainer evalbot    [--data DIR] [--out DIR] [--ckpt FILE]                the bot on the held-out ground shots
//   RLCoachTrainer enginebench [--data DIR] [--out DIR]                             the engine on the same held-out reps
//   RLCoachTrainer heldout    [--data DIR]                                          (re)makes the held-out lists (don't, normally)
//   RLCoachTrainer check      [--data DIR]                                          replays your own inputs: is the sim set up right?
//
// Default data folder: %APPDATA%\bakkesmod\bakkesmod\data\RLCoach. Output goes in its "bot" subfolder.
#include "Data.h"
#include "LineSim.h"
#include "Trainer.h"
#include "../Engine.h"
#include "../Mechanics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;

Args ParseArgs(int argc, char** argv)
{
	Args a;
	if (argc >= 2) a.mode = argv[1];
	for (int i = 2; i + 1 < argc; i += 2)
	{
		std::string k = argv[i], v = argv[i + 1];
		if (k == "--data") a.data = v;
		else if (k == "--out") a.out = v;
		else if (k == "--threads") a.threads = std::atoi(v.c_str());
		else if (k == "--hours") a.hours = std::atof(v.c_str());
		else if (k == "--steps") a.maxSteps = std::atoll(v.c_str());
		else if (k == "--ckpt") a.ckpt = v;
		else if (k == "--lines") a.lines = std::atoi(v.c_str());
		else if (k == "--limit") a.limit = std::atoi(v.c_str());
		else if (k == "--seconds") a.engineSeconds = std::atof(v.c_str());
		else if (k == "--tag") a.tag = v;
		else if (k == "--config") a.config = v;
	}
	if (a.data.empty())
	{
		if (const char* app = std::getenv("APPDATA")) a.data = (fs::path(app) / "bakkesmod" / "bakkesmod" / "data" / "RLCoach").string();
		else a.data = ".";
	}
	if (a.out.empty()) a.out = (fs::path(a.data) / "bot").string();
	if (a.threads <= 0)
	{
		unsigned h = std::thread::hardware_concurrency();
		a.threads = static_cast<int>(h > 2 ? h - 2 : 1);   // two threads left for Windows (and the game, if it's open)
	}
	return a;
}

// Where this exe's held-out lists live: next to the exe, or in the repo's trainer folder
fs::path TrainerDir(const char* argv0)
{
	std::vector<fs::path> tries = { fs::current_path(), fs::path(argv0).parent_path(), fs::path(argv0).parent_path() / "..",
		fs::path(argv0).parent_path() / ".." / "..", fs::path(argv0).parent_path() / ".." / ".." / "trainer", fs::current_path() / "trainer" };
	for (const fs::path& p : tries)
		if (fs::exists(p / "heldout_ground.txt")) return p;
	return fs::current_path();
}

static int MakeHeldout(const Args& a, const fs::path& dir)
{
	// Fixed lists: 61 aerials, 61 ground shots and 30 of each other mechanic, drawn with a fixed seed from every rep
	// with a recording. Written once (Oct 23, 2026) and kept in the repo; the Oct 13 lists were cloud-only and got lost.
	ShotBook book;
	const ShotType types[] = { ShotType::Aerial, ShotType::Ground, ShotType::DoubleTap, ShotType::Dribble, ShotType::AirDribble, ShotType::FlipReset };
	for (ShotType t : types)
	{
		fs::path f = Data::HeldoutFile(dir, t);
		if (fs::exists(f)) { std::printf("%s exists, kept\n", f.string().c_str()); continue; }
		std::vector<Rep> reps = Data::Load(a.data, book, t, {}, 0, a.threads);
		std::vector<std::string> names;
		for (const Rep& r : reps) names.push_back(r.file);
		std::sort(names.begin(), names.end());
		std::mt19937 rng(20261023u + static_cast<unsigned>(t));
		std::shuffle(names.begin(), names.end(), rng);
		const size_t want = (t == ShotType::Aerial || t == ShotType::Ground) ? 61 : 30;
		names.resize(std::min(want, names.size()));
		std::sort(names.begin(), names.end());
		std::ofstream o(f);
		o << "# Held-out " << Data::TypeFile(t) << " reps: never trained on, only benchmarked. Fixed list (Oct 23, 2026), "
			<< names.size() << " of " << reps.size() << ".\n";
		for (const std::string& n : names) o << n << "\n";
		std::printf("%s: %zu of %zu\n", f.string().c_str(), names.size(), reps.size());
	}
	return 0;
}

// Sanity check: replay your own inputs from the engine's start and see whether the sim lands where you did
static int Check(const Args& a, const fs::path& dir)
{
	ShotBook book;
	auto held = Data::ReadList(Data::HeldoutFile(dir, ShotType::Ground));
	std::vector<Rep> reps = Data::Load(a.data, book, ShotType::Ground, held, 0, a.threads);
	std::printf("%zu ground reps (%zu held out)\n", reps.size(), held.size());
	int n = 0, same = 0, sameMirror = 0, goals = 0;
	double err = 0, errM = 0;
	std::vector<float> errs;
	LineSim sim;
	for (size_t k = 0; k < reps.size() && n < (a.limit > 0 ? a.limit : 200); k += 3)
	{
		for (int m = 0; m < 2; m++)
		{
			Rep r = reps[k];
			if (m == 1) { r.fr = Data::Mirror(r.fr); Data::Finish(r, book, ""); }
			const int s = r.sContact;
			sim.Start(r, s);
			for (int t = s; t < static_cast<int>(r.fr.size()) - 1 && sim.TickNow() < s + 60; t++)
			{
				const Frame& f = r.fr[t];
				RocketSim::CarControls c;
				c.throttle = f.throttle; c.steer = f.steer; c.pitch = f.pitch; c.yaw = f.yaw; c.roll = f.roll;
				c.jump = f.jump; c.boost = f.boost; c.handbrake = f.handbrake;
				if (!sim.Tick(c)) break;
			}
			const Frame& g = sim.Now();
			const Frame& y = r.fr[std::min<size_t>(sim.TickNow(), r.fr.size() - 1)];
			float e = std::sqrt((g.carX - y.carX) * (g.carX - y.carX) + (g.carY - y.carY) * (g.carY - y.carY) + (g.carZ - y.carZ) * (g.carZ - y.carZ));
			(m ? errM : err) += e;
			if (m == 0) errs.push_back(e);
			// and the whole line, your inputs to the end
			sim.Start(r, s);
			for (int t = s; t < static_cast<int>(r.fr.size()) - 1; t++)
			{
				const Frame& f = r.fr[t];
				RocketSim::CarControls c;
				c.throttle = f.throttle; c.steer = f.steer; c.pitch = f.pitch; c.yaw = f.yaw; c.roll = f.roll;
				c.jump = f.jump; c.boost = f.boost; c.handbrake = f.handbrake;
				if (!sim.Tick(c)) break;
			}
			const LineResult& lr = sim.Finish();
			bool ok = lr.wentIn == r.wentIn;
			(m ? sameMirror : same) += ok;
			if (m == 0) goals += r.wentIn;
		}
		n++;
	}
	std::sort(errs.begin(), errs.end());
	if (!errs.empty()) std::printf("median %.1f uu off\n", errs[errs.size() / 2]);
	std::printf("checked %d reps: car 0.5 s later %.1f uu off (mirrored %.1f); same result with your inputs %d/%d (mirrored %d/%d); %d goals\n",
		n, err / n, errM / n, same, n, sameMirror, n, goals);
	return 0;
}

// The engine on the held-out ground shots, both starts, untimed (the benchmark's rules)
static int EngineBench(const Args& a, const fs::path& dir)
{
	ShotBook book;
	auto held = Data::ReadList(Data::HeldoutFile(dir, ShotType::Ground));
	std::vector<Rep> reps = Data::Load(a.data, book, ShotType::Ground, held, 1, a.threads);
	fs::create_directories(a.out);
	fs::path outFile = fs::path(a.out) / ("enginebench" + a.tag + ".csv");
	std::map<std::string, std::string> done;
	{
		std::ifstream in(outFile);
		std::string line;
		while (std::getline(in, line)) { auto c = line.find(','); if (c != std::string::npos) done[line.substr(0, c)] = line; }
	}
	std::ofstream o(outFile, std::ios::app);
	if (done.empty()) o << "file,start,start_tick,real_grade,found,grade,held,seconds\n";
	const float secs = a.engineSeconds > 0 ? static_cast<float>(a.engineSeconds) : 1e6f;
	int idx = 0;
	for (const Rep& r : reps)
	{
		if (a.limit > 0 && idx >= a.limit) break;
		idx++;
		for (int which = 0; which < 2; which++)
		{
			const int s = which == 0 ? r.sContact : r.sMech;
			const std::string key = r.file + (which == 0 ? "#contact" : "#mech");
			if (done.count(key)) continue;
			if (which == 1 && r.sMech > r.sContact - 20) { o << key << "," << (which ? "mech" : "contact") << "," << s << "," << r.realGrade << ",skip,0,0,0\n"; o.flush(); continue; }
			auto t0 = std::chrono::steady_clock::now();
			EngineResult er = Engine::Line(r.fr, r.wentIn, r.floorBeforeGoal, r.eventTick, r.ref, 0, nullptr, secs, s, 1);
			float sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
			bool found = er.status == EngineStatus::Found;
			o << key << "," << (which ? "mech" : "contact") << "," << s << "," << r.realGrade << "," << (found ? 1 : 0) << "," << (found ? er.grade : 0)
				<< "," << er.held << "," << sec << "\n";
			o.flush();
			std::printf("%s %s: you %d, engine %s %d (%.1f s)\n", r.file.c_str(), which ? "setup" : "contact", r.realGrade, found ? "found" : "none", found ? er.grade : 0, sec);
			std::fflush(stdout);
		}
	}
	return 0;
}

int main(int argc, char** argv)
{
	Args a = ParseArgs(argc, argv);
	if (a.mode.empty() || a.mode == "help" || a.mode == "--help")
	{
		std::printf("RLCoachTrainer train | evalbot | enginebench | heldout | check   [--data DIR] [--out DIR] [--threads N] [--hours H]\n");
		return 0;
	}
	const fs::path meshes = fs::path(a.data) / "collision_meshes";
	if (!fs::exists(meshes)) { std::printf("No collision meshes in %s (use --data to point at the RLCoach data folder)\n", meshes.string().c_str()); return 1; }
	RocketSim::Init(meshes, true);
	const fs::path dir = TrainerDir(argv[0]);
	if (a.mode == "heldout") return MakeHeldout(a, dir);
	if (!fs::exists(Data::HeldoutFile(dir, ShotType::Ground))) { std::printf("Can't find heldout_ground.txt (run from the trainer folder)\n"); return 1; }
	if (a.mode == "check") return Check(a, dir);
	if (a.mode == "enginebench") return EngineBench(a, dir);
	if (a.mode == "train") return Trainer::Train(a, dir);
	if (a.mode == "evalbot") return Trainer::EvalBot(a, dir);
	std::printf("Unknown mode %s\n", a.mode.c_str());
	return 1;
}
