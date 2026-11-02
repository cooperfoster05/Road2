#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <chrono>
#include <cstdio>
int main(int, char** argv)
{
	RocketSim::Init(argv[1], true);
	for (int lv = 1; lv <= 5; lv++)
	{
		int full = 0, kinds[3] = {}; float win = 0, slack = 0; int n = 0; long long ms = 0;
		for (uint32_t seed = 1; seed <= 20; seed++)
		{
			auto t0 = std::chrono::steady_clock::now();
			auto set = Combine::MakeReadSet(seed, lv, 10);
			ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			full += set.size() == 10;
			for (auto& r : set) { kinds[r.kind]++; win += r.tFloor - r.tWall; slack += r.slackReal; n++; }
			if (seed == 1 && (lv == 1 || lv == 5))
				for (auto& r : set) printf("   kind %d  wall %.2f floor %.2f  reach %.2f  spare %.2f  car (%.0f,%.0f)\n", r.kind, r.tWall, r.tFloor, r.tReachReal, r.slackReal, r.car.pos.x, r.car.pos.y);
		}
		printf("level %d: %d/20 sets full, kinds %d/%d/%d, avg window %.2f s, avg spare %.2f s, %lld ms per set\n", lv, full, kinds[0], kinds[1], kinds[2], win / n, slack / n, ms / 20);
	}
}
