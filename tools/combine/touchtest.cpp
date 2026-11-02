#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <cstdio>
#include <fstream>
#include <sstream>
int main(int, char** argv)
{
	RocketSim::Init(argv[1], true);
	std::ifstream f(argv[2]); std::stringstream ss; ss << f.rdbuf();
	auto passes = Combine::LoadSaveSituations(ss.str());
	printf("%zu passes loaded\n", passes.size());
	for (int lv = 1; lv <= 5; lv++)
	{
		int full = 0; float z = 0, sp = 0; int n = 0;
		for (uint32_t seed = 1; seed <= 30; seed++)
		{
			auto set = Combine::MakeTouchSet(seed, lv, 10, passes);
			full += set.size() == 10;
			for (auto& t : set) { z += t.recvZ; sp += t.recvSpeed; n++; }
		}
		printf("level %d (%s): %d/30 full, avg receive height %.0f, speed %.0f\n", lv, Combine::TouchLevelNote(lv), full, z / n, sp / n);
	}
}
