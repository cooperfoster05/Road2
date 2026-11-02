#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
int main(int, char** argv)
{
	auto t0 = std::chrono::steady_clock::now();
	std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
	auto all = Combine::LoadChallenges(ss.str());
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	printf("%zu loaded in %lld ms\n", all.size(), (long long)ms);
	for (char k : { 'F', 'D' })
		for (int lv = 1; lv <= 5; lv++)
		{
			auto t1 = std::chrono::steady_clock::now();
			auto set = Combine::MakeChallengeSet(3, lv, 10, k, all);
			auto m2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
			float len = 0; for (auto& c : set) len += c.botEnd;
			printf("%c level %d: %zu in %lld ms, avg %.1f s long, %zu frames first\n", k, lv, set.size(), (long long)m2, len / set.size(), set[0].bot.size());
		}
}
