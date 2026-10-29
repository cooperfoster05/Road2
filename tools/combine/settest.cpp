#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <chrono>
#include <cstdio>
int main(int argc, char** argv)
{
	RocketSim::Init(std::filesystem::path(argv[1]), true);
	for (int lv = 1; lv <= 5; lv++)
		for (uint32_t seed : { 1u, 2u })
		{
			auto t0 = std::chrono::steady_clock::now();
			auto set = Combine::MakeSaveSet(seed, lv, 10);
			float ms = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() * 1000;
			printf("level %d seed %u: %zu shots in %.0f ms  [%s]\n", lv, seed, set.size(), ms, Combine::SetCode("SV", lv, seed).c_str());
			if (seed == 1) for (auto& s : set) printf("   ball %5.0f,%5.0f,%4.0f speed %4.0f  car %5.0f,%5.0f  slack %.2f cross %.2f\n", s.ball.pos.x, s.ball.pos.y, s.ball.pos.z,
				std::sqrt(s.ball.vel.x*s.ball.vel.x+s.ball.vel.y*s.ball.vel.y+s.ball.vel.z*s.ball.vel.z), s.car.pos.x, s.car.pos.y, s.slack, s.tCross);
		}
}
