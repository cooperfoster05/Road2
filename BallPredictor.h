#pragma once
// Ball prediction using RocketSim (an open-source Rocket League physics simulator).
// Plain C++, no BakkesMod code, so RocketSim and the game SDK never share a file.

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace RocketSim { class Arena; }

struct BallSample
{
	float x = 0, y = 0, z = 0;
	float vx = 0, vy = 0, vz = 0;
};

class BallPredictor
{
public:
	~BallPredictor();

	// Loads the arena collision files in the background. Safe to call more than once.
	void StartInit(const std::filesystem::path& meshesFolder);

	bool Ready() const { return ready; }
	std::string Status() const;

	// Simulates the ball forward from the given state. Angular velocity is in radians per second.
	// Returns false if RocketSim isn't ready yet.
	bool Predict(const BallSample& start, float avx, float avy, float avz, int ticks, std::vector<BallSample>& out);

	// Mirror X when handing states to RocketSim (in case its axes are flipped relative to the game's).
	// The arena is symmetric, so this only matters for spin on bounces. Chosen by the prediction test.
	std::atomic<bool> mirrorX{ false };

	// Replays every saved rep through the predictor and writes pred_test.csv with the errors.
	// Runs on its own thread; poll TestStatus().
	void StartAccuracyTest(const std::filesystem::path& rlcoachFolder);
	std::string TestStatus() const;

private:
	void SetStatus(const std::string& s);

	mutable std::mutex mtx;
	RocketSim::Arena* arena = nullptr;
	std::atomic<bool> ready{ false };
	std::atomic<bool> started{ false };
	std::string status = "Not started";
	std::thread initThread;

	mutable std::mutex testMtx;
	std::string testStatus;
	std::thread testThread;
	std::atomic<bool> testRunning{ false };
};
