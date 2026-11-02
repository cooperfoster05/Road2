#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <chrono>
#include <cstdio>
int main(int, char** argv)
{
	RocketSim::Init(argv[1], true);
	for (int lv = 1; lv <= 5; lv++)
	{
		int full = 0, n = 0; float spare = 0, speed = 0; long long ms = 0;
		for (uint32_t seed = 1; seed <= 20; seed++)
		{
			auto t0 = std::chrono::steady_clock::now();
			auto set = Combine::MakeReadSet(seed, lv, 10);
			ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			full += set.size() == 10;
			for (auto& r : set) { n++; spare += r.slackReal; speed += std::sqrt(r.ball.vel.x * r.ball.vel.x + r.ball.vel.y * r.ball.vel.y + r.ball.vel.z * r.ball.vel.z); }
		}
		printf("level %d: %d/20 full, meet it above %.0f, avg spare %.2f s, avg ball speed at start %.0f, %lld ms per set\n", lv, full, Combine::ReadMinHeight(lv), spare / n, speed / n, ms / 20);
	}
}
