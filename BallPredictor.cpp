// No pch on purpose: this file only sees RocketSim, never the BakkesMod SDK.
#include "BallPredictor.h"
#include "RepLibrary.h"

#include "third_party/RocketSim/src/RocketSim.h"

#include <cmath>
#include <format>
#include <fstream>

namespace
{
	RocketSim::BallState ToSim(const BallSample& s, float avx, float avy, float avz, bool mirror)
	{
		RocketSim::BallState bs;
		float m = mirror ? -1.0f : 1.0f;
		bs.pos = RocketSim::Vec(s.x * m, s.y, s.z);
		bs.vel = RocketSim::Vec(s.vx * m, s.vy, s.vz);
		// Spin is a "pseudo-vector": mirroring X flips its Y and Z parts instead
		bs.angVel = mirror ? RocketSim::Vec(avx, -avy, -avz) : RocketSim::Vec(avx, avy, avz);
		return bs;
	}

	BallSample FromSim(const RocketSim::BallState& bs, bool mirror)
	{
		float m = mirror ? -1.0f : 1.0f;
		return BallSample{ bs.pos.x * m, bs.pos.y, bs.pos.z, bs.vel.x * m, bs.vel.y, bs.vel.z };
	}

	bool HasMeshFiles(const std::filesystem::path& folder)
	{
		std::error_code ec;
		auto soccar = folder / "soccar";
		if (!std::filesystem::is_directory(soccar, ec)) return false;
		for (auto& e : std::filesystem::directory_iterator(soccar, ec))
			if (e.path().extension() == ".cmf") return true;
		return false;
	}
}

BallPredictor::~BallPredictor()
{
	if (initThread.joinable()) initThread.join();
	if (testThread.joinable()) testThread.join();
	std::lock_guard<std::mutex> lock(mtx);
	delete arena;
	arena = nullptr;
}

void BallPredictor::SetStatus(const std::string& s)
{
	std::lock_guard<std::mutex> lock(mtx);
	status = s;
}

std::string BallPredictor::Status() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return status;
}

void BallPredictor::StartInit(const std::filesystem::path& meshesFolder)
{
	if (started.exchange(true)) return;

	if (!HasMeshFiles(meshesFolder))
	{
		SetStatus("No arena files yet: run RLArenaCollisionDumper (see the RLCoach settings tab)");
		started = false; // allow another try after the files are added
		return;
	}

	SetStatus("Loading arena files...");
	initThread = std::thread([this, meshesFolder]() {
		try
		{
			RocketSim::Init(meshesFolder, true);
			RocketSim::Arena* a = RocketSim::Arena::Create(RocketSim::GameMode::SOCCAR);
			{
				std::lock_guard<std::mutex> lock(mtx);
				arena = a;
				status = "Ready";
			}
			ready = true;
		}
		catch (const std::exception& e)
		{
			SetStatus(std::string("Couldn't load arena files: ") + e.what());
		}
	});
}

bool BallPredictor::Predict(const BallSample& start, float avx, float avy, float avz, int ticks, std::vector<BallSample>& out)
{
	out.clear();
	if (!ready) return false;
	std::lock_guard<std::mutex> lock(mtx);
	if (!arena) return false;

	bool mirror = mirrorX;
	arena->ball->SetState(ToSim(start, avx, avy, avz, mirror));
	out.reserve(ticks);
	for (int i = 0; i < ticks; i++)
	{
		arena->Step(1);
		out.push_back(FromSim(arena->ball->GetState(), mirror));
	}
	return true;
}

std::string BallPredictor::TestStatus() const
{
	std::lock_guard<std::mutex> lock(testMtx);
	return testStatus;
}

void BallPredictor::StartAccuracyTest(const std::filesystem::path& root)
{
	if (!ready || testRunning.exchange(true)) return;
	if (testThread.joinable()) testThread.join();

	{
		std::lock_guard<std::mutex> lock(testMtx);
		testStatus = "Running...";
	}

	testThread = std::thread([this, root]() {
		// Own arena so the test never blocks live predictions
		RocketSim::Arena* testArena = nullptr;
		try { testArena = RocketSim::Arena::Create(RocketSim::GameMode::SOCCAR); }
		catch (const std::exception& e)
		{
			std::lock_guard<std::mutex> lock(testMtx);
			testStatus = std::string("Failed: ") + e.what();
			testRunning = false;
			return;
		}

		std::ofstream out(root / "pred_test.csv");
		out << "file,mirror,start_tick,compared_ticks,bounced,err_0_5s,err_1s,err_2s,err_end,max_err\n";

		std::vector<RepListItem> reps = RepLibrary::ListReps(root);
		double sumEnd[2] = { 0, 0 };
		int counted = 0;

		for (const RepListItem& item : reps)
		{
			std::vector<Frame> frames;
			std::string err;
			if (!RepLibrary::LoadRep(root, item.file, frames, err)) continue;

			// Start once the ball is moving; stop just before the first touch (after that it's not free flight)
			int start = -1;
			for (int i = 0; i < static_cast<int>(frames.size()); i++)
			{
				const Frame& f = frames[i];
				if (std::fabs(f.ballVX) + std::fabs(f.ballVY) + std::fabs(f.ballVZ) > 1.0f) { start = i; break; }
			}
			if (start < 0) continue;
			int end = static_cast<int>(frames.size()) - 1;
			for (int i = start + 1; i < static_cast<int>(frames.size()); i++)
				if (frames[i].touch) { end = i - 1; break; }
			int n = end - start;
			if (n < 30) continue;

			bool bounced = false;
			for (int i = start; i <= end; i++) if (frames[i].ballZ < 100.0f) { bounced = true; break; }

			for (int mode = 0; mode < 2; mode++)
			{
				const Frame& s = frames[start];
				BallSample bs{ s.ballX, s.ballY, s.ballZ, s.ballVX, s.ballVY, s.ballVZ };
				testArena->ball->SetState(ToSim(bs, s.ballAVX, s.ballAVY, s.ballAVZ, mode == 1));

				float errAt[3] = { -1, -1, -1 };
				const int marks[3] = { 60, 120, 240 };
				float maxErr = 0, lastErr = 0;
				for (int k = 1; k <= n; k++)
				{
					testArena->Step(1);
					BallSample p = FromSim(testArena->ball->GetState(), mode == 1);
					const Frame& a = frames[start + k];
					float e = std::sqrt((p.x - a.ballX) * (p.x - a.ballX) + (p.y - a.ballY) * (p.y - a.ballY) + (p.z - a.ballZ) * (p.z - a.ballZ));
					maxErr = std::fmax(maxErr, e);
					lastErr = e;
					for (int m = 0; m < 3; m++) if (k == marks[m]) errAt[m] = e;
				}
				out << std::format("{},{},{},{},{},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}\n",
					item.file, mode, start, n, (int)bounced, errAt[0], errAt[1], errAt[2], lastErr, maxErr);
				sumEnd[mode] += lastErr;
			}
			counted++;
		}
		delete testArena;

		std::lock_guard<std::mutex> lock(testMtx);
		if (counted == 0)
			testStatus = "No usable reps found";
		else
			testStatus = std::format("Done: {} reps. Average error at first touch: {:.0f} units (normal), {:.0f} (mirrored). Saved pred_test.csv",
				counted, sumEnd[0] / counted, sumEnd[1] / counted);
		testRunning = false;
	});
}
