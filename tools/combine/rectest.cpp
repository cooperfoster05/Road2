#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <chrono>
#include <cstdio>
#include <cmath>
int main(int argc, char** argv)
{
	RocketSim::Init(std::filesystem::path(argv[1]), true);
	for (int lv = 1; lv <= 5; lv++)
	{
		auto t0 = std::chrono::steady_clock::now();
		auto set = Combine::MakeRecoverySet(7, lv, 10);
		float ms = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() * 1000;
		printf("level %d: %zu throws in %.0f ms\n", lv, set.size(), ms);
		for (auto& t : set)
			printf("   z %4.0f speed %4.0f spin %.1f  lands on %s in %.2f s, turn %.2f s, spare %.2f\n", t.pos.z,
				std::sqrt(t.vel.x*t.vel.x+t.vel.y*t.vel.y+t.vel.z*t.vel.z), std::sqrt(t.angVel.x*t.angVel.x+t.angVel.y*t.angVel.y+t.angVel.z*t.angVel.z),
				t.normal.z > 0.7f ? "floor" : t.normal.z < -0.7f ? "ceiling" : "wall", t.tHit, t.tTurnReal, t.margin);
	}
}
