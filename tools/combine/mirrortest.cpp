// The 50s / shadow mirror: every frame of a mirrored set must face the mirror of the original, and move the way it faces
#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace RocketSim;
static Vec Fwd(const float q[4]) { float p, y, r; Combine::QuatToPYR(q, p, y, r); return Angle(y, p, r).ToRotMat().forward; }
static Vec Up(const float q[4]) { float p, y, r; Combine::QuatToPYR(q, p, y, r); return Angle(y, p, r).ToRotMat().up; }
int main(int, char** argv)
{
	std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
	auto all = Combine::LoadChallenges(ss.str());
	double worst = 0; int mirrored = 0, sets = 0, facingAway = 0, frames = 0;
	for (char k : { 'F', 'D' })
		for (int lv = 1; lv <= 5; lv++)
			for (uint32_t seed = 1; seed <= 20; seed++)
			{
				auto set = Combine::MakeChallengeSet(seed, lv, 10, k, all);
				sets++;
				for (auto& c : set)
				{
					// find the original (same start, maybe mirrored in x)
					for (auto& o : all)
					{
						if (o.kind != c.kind || std::fabs(o.you.pos.y - c.you.pos.y) > 0.01f || std::fabs(std::fabs(o.you.pos.x) - std::fabs(c.you.pos.x)) > 0.01f) continue;
						const bool m = std::fabs(o.you.pos.x + c.you.pos.x) < 0.01f && std::fabs(o.you.pos.x) > 0.01f;
						if (!m) break;
						mirrored++;
						Vec fo = Fwd(o.you.q), fc = Fwd(c.you.q), uo = Up(o.you.q), uc = Up(c.you.q);
						worst = std::max(worst, (double)((fc - Vec(-fo.x, fo.y, fo.z)).Length() + (uc - Vec(-uo.x, uo.y, uo.z)).Length()));
						break;
					}
					// you and the bot: moving the way you face (on the ground, at speed)?
					auto check = [&](const Combine::PathFrame& p) {
						const float sp = std::sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y);
						if (sp < 600 || p.pos.z > 40) return;
						frames++;
						Vec fw = Fwd(p.q);
						if ((fw.x * p.vel.x + fw.y * p.vel.y) / sp < -0.5f) facingAway++;
					};
					check(c.you);
					check(c.bot.front());
				}
			}
	printf("%d sets, %d mirrored spawns checked: worst facing error %.4f\n", sets, mirrored, worst);
	printf("spawns on the floor at speed: %d, driving backwards: %d\n", frames, facingAway);
}
