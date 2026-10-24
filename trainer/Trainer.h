#pragma once
#include <filesystem>
#include <string>

struct Args
{
	std::string mode, data, out, ckpt, tag, config;
	int threads = 0;
	double hours = 0;          // stop after this long (0 = until Ctrl+C or --steps)
	long long maxSteps = 0;
	int lines = 32;            // evalbot: lines tried per start (1 greedy + sampled)
	int limit = 0;
	double engineSeconds = 0;
};

namespace Trainer
{
	int Train(const Args& a, const std::filesystem::path& trainerDir);
	int EvalBot(const Args& a, const std::filesystem::path& trainerDir);
}
