#pragma once
// The bot: what it sees (observations), what it can press (actions), and the policy network that picks them.
// It decides every 2 ticks (60 times a second) with analog-ish inputs: 9 steps of steer / pitch / yaw,
// 5 of throttle and roll, and the three buttons. Everything it sees comes from the same Frame data the plugin
// records, so it can learn from your own reps too (the warm start) and run on any rep later.
#include "Net.h"
#include "../RepData.h"
#include "../third_party/RocketSim/src/RocketSim.h"
#include <array>
#include <random>
#include <vector>

namespace Bot
{
	constexpr int kHeads = 8;
	// throttle, steer, pitch, yaw, roll, jump, boost, handbrake
	constexpr int kBins[kHeads] = { 5, 9, 9, 9, 5, 2, 2, 2 };
	constexpr int kLogits = 5 + 9 + 9 + 9 + 5 + 2 + 2 + 2;
	constexpr int kOff[kHeads] = { 0, 5, 14, 23, 32, 37, 39, 41 };   // where each head's logits start
	constexpr int kObs = 66;
	constexpr int kPathN = 7;
	constexpr int kPathAt[kPathN] = { 10, 20, 30, 45, 60, 90, 120 };   // ticks ahead

	using Act = std::array<int, kHeads>;   // a bin per head

	RocketSim::CarControls Controls(const Act& a);
	Act FromControls(const Frame& f);        // nearest bins to recorded inputs (your inputs, for the warm start)
	void ActValues(const Act& a, float* out8);

	// What the bot remembers between ticks (the frame data doesn't hold it): time in the air, how long jump has
	// been held, its last inputs
	struct Memory
	{
		int airTicks = 0, jumpHeld = 0;
		float prev[8] = {};
		void Seed(const std::vector<Frame>& fr, int s);   // from the rep's history up to tick s
		void After(const Frame& now, const RocketSim::CarControls& c);   // c = inputs just applied
	};

	// The ball's path from the start, with nobody touching it (index = ticks since the start)
	struct BallPath
	{
		std::vector<RocketSim::Vec> p;
		void Build(RocketSim::Arena* ballArena, const Frame& f, int ticks);
	};

	// Observation for the frame `f`, `since` ticks after the start, shooting at goalSign's goal
	void Observe(const Frame& f, const Memory& m, const BallPath& path, int since, bool touched, float goalSign, float* out);

	struct Policy
	{
		MLP pi, vf;
		ObsNorm norm;
		void Init(std::mt19937& rng, int width, int depth);
		// For B normalized obs: logits into `logits` (B x kLogits)
		void Logits(const float* obs, int B, MLP::Work& w, std::vector<float>& logits) const;
	};

	// Per head log-softmax helpers
	void LogSoftmax(const float* logits, float* logp);   // all heads, kLogits values
	Act Sample(const float* logp, std::mt19937& rng);
	Act Greedy(const float* logp);
	float LogProb(const float* logp, const Act& a);
	float Entropy(const float* logp);
}
