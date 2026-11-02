// Spare time (realistic estimate) of real saves, by where they came from: P pro, C clip, 1-6 gold to SSL
#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
int main(int argc, char** argv)
{
	RocketSim::Init(argv[1], true);
	std::map<char, std::vector<float>> by, speed;
	for (int a = 2; a < argc; a++)
	{
		std::ifstream f(argv[a]); std::stringstream ss; ss << f.rdbuf();
		for (auto& s : Combine::LoadSaveSituations(ss.str()))
		{
			auto r = Combine::SaveReach(s.ball, s.car, -1);
			if (!r.possible || r.tReachReal < 0) continue;
			by[s.source].push_back(r.slackReal);
			speed[s.source].push_back(std::sqrt(s.ball.vel.x * s.ball.vel.x + s.ball.vel.y * s.ball.vel.y + s.ball.vel.z * s.ball.vel.z));
		}
	}
	for (auto& [k, v] : by)
	{
		std::sort(v.begin(), v.end()); auto& sp = speed[k]; std::sort(sp.begin(), sp.end());
		auto q = [&](std::vector<float>& x, float p) { return x[std::min(x.size() - 1, (size_t)(p * x.size()))]; };
		printf("%c n %5zu  spare p10 %.2f p25 %.2f p50 %.2f p75 %.2f   shot speed p25 %.0f p50 %.0f p75 %.0f\n", k, v.size(), q(v, .10f), q(v, .25f), q(v, .5f), q(v, .75f), q(sp, .25f), q(sp, .5f), q(sp, .75f));
	}
}
