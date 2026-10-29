// Runs the Combine's save check on real saves from match replays: it must never call one impossible.
#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <memory>
#include <cmath>
int main(int argc, char** argv)
{
	RocketSim::Init(std::filesystem::path(argv[2]), true);
	std::ifstream f(argv[1]); std::string line; std::getline(f, line);
	int n = 0, ok = 0, late = 0, offTarget = 0, skipped = 0;
	std::vector<float> slacks;
	while (std::getline(f, line))
	{
		std::vector<std::string> c; std::stringstream ss(line); std::string x;
		while (std::getline(ss, x, ',')) c.push_back(x);
		auto F = [&](int i) { return std::stof(c[i]); };
		Combine::BallStart b{ {F(3), F(4), F(5)}, {F(6), F(7), F(8)}, {F(9), F(10), F(11)} };
		Combine::CarStart car{ {F(12), F(13), F(14)}, {F(15), F(16), F(17)} };
		const float ax = std::fabs(car.pos.x), ay = std::fabs(car.pos.y), az = car.pos.z;
		car.onSurface = az < 60 || az > 1940 || ax > 3980 || (ay > 5000 && !(ax < 893 && az < 642)) || ax + ay > 7900;
		const float dt = F(2);
		const int gs = std::stoi(c[32]);
		Combine::ReachResult r = Combine::SaveReach(b, car, gs);
		// Clean cases only: the simulated ball has to be where the real one was at the save (nothing else touched it)
		float simDev = 1e9f;
		{
			std::unique_ptr<RocketSim::Arena> a(RocketSim::Arena::Create(RocketSim::GameMode::SOCCAR));
			RocketSim::BallState bs; bs.pos = RocketSim::Vec(b.pos.x, b.pos.y, b.pos.z); bs.vel = RocketSim::Vec(b.vel.x, b.vel.y, b.vel.z); bs.angVel = RocketSim::Vec(b.angVel.x, b.angVel.y, b.angVel.z);
			a->ball->SetState(bs);
			a->Step(std::max(1, (int)std::lround(dt * 120)));
			RocketSim::Vec q = a->ball->GetState().pos;
			simDev = std::sqrt((q.x - F(26)) * (q.x - F(26)) + (q.y - F(27)) * (q.y - F(27)) + (q.z - F(28)) * (q.z - F(28)));
		}
		const bool clean = simDev < 250.0f && r.onTarget;
		if (!clean) { skipped++; continue; }
		n++;
		offTarget += r.onTarget ? 0 : 1;
		if (r.possible) { ok++; slacks.push_back(r.slack); }
		const bool tooLate = !r.possible || r.tReach > dt + 0.05f;
		late += tooLate ? 1 : 0;
		if (tooLate || argc > 3)
			printf("%s %s frame %s: %s, reach %.2f s, real save at %.2f s, ball crosses %.2f s, slack %.2f, car %s %.0f,%.0f,%.0f ball %.0f,%.0f,%.0f%s\n",
				tooLate ? "!!" : "  ", c[0].c_str(), c[1].c_str(), r.possible ? "possible" : "IMPOSSIBLE", r.tReach, dt, r.tCross, r.slack,
				car.onSurface ? "surface" : "air", car.pos.x, car.pos.y, car.pos.z, b.pos.x, b.pos.y, b.pos.z, r.onTarget ? "" : " (sim says it misses)");
	}
	std::sort(slacks.begin(), slacks.end());
	auto pct = [&](float p) { return slacks.empty() ? 0.0f : slacks[std::min(slacks.size() - 1, (size_t)(p * slacks.size()))]; };
	printf("clean real saves %d (skipped %d where the replay ball left the free path or the shot misses): judged possible %d, reach later than the real save %d\n", n, skipped, ok, late);
	printf("slack (s): min %.2f p1 %.2f p2 %.2f p10 %.2f p5 %.2f p25 %.2f p50 %.2f p75 %.2f p95 %.2f\n", slacks.empty()?0:slacks[0], pct(0.01f), pct(0.02f), pct(0.10f), pct(0.05f), pct(0.25f), pct(0.5f), pct(0.75f), pct(0.95f));
}
