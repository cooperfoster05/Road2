#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cmath>
int main(int argc, char** argv)
{
	RocketSim::Init(std::filesystem::path(argv[1]), true);
	std::ifstream f(argv[2]); std::stringstream ss; ss << f.rdbuf();
	auto sits = Combine::LoadSaveSituations(ss.str());
	printf("situations %zu\n", sits.size());
	for (int lv = 1; lv <= 5; lv++)
	{
		auto t0 = std::chrono::steady_clock::now();
		auto set = Combine::MakeSaveSet(11, lv, 10, &sits);
		float ms = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() * 1000;
		int real = 0; for (auto& s : set) real += s.real;
		printf("level %d: %zu shots (%d real) in %.0f ms\n", lv, set.size(), real, ms);
		for (auto& s : set) printf("   car %5.0f,%5.0f,%4.0f speed %4.0f boost %3.0f  ball speed %4.0f  real slack %.2f (perfect %.2f) crosses %.2f\n",
			s.car.pos.x, s.car.pos.y, s.car.pos.z, std::sqrt(s.car.vel.x*s.car.vel.x+s.car.vel.y*s.car.vel.y+s.car.vel.z*s.car.vel.z), s.boost,
			std::sqrt(s.ball.vel.x*s.ball.vel.x+s.ball.vel.y*s.ball.vel.y+s.ball.vel.z*s.ball.vel.z), s.slackReal, s.slack, s.tCross);
	}
}
