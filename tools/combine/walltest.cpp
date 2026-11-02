// Wall save spawns: do the wheels grip from the start?
#include "Combine.h"
#include "RocketSim.h"
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace RocketSim;
int main(int argc, char** argv)
{
	RocketSim::Init(argv[1], true);
	std::ifstream f(argv[2]); std::stringstream ss; ss << f.rdbuf();
	auto sits = Combine::LoadSaveSituations(ss.str());
	int walls = 0, gripStart = 0, grip12 = 0, grip36 = 0;
	for (int lv = 1; lv <= 5; lv++)
		for (uint32_t seed = 1; seed <= 40; seed++)
			for (auto& s : Combine::MakeSaveSet(seed, lv, 10, &sits))
			{
				RotMat m = Angle(s.car.yaw, s.car.pitch, s.car.roll).ToRotMat();
				if (!s.car.onSurface || m.up.z > 0.9f) continue;
				walls++;
				std::unique_ptr<Arena> a(Arena::Create(GameMode::SOCCAR));
				Car* c = a->AddCar(Team::BLUE, CAR_CONFIG_OCTANE);
				CarState cs; cs.pos = { s.car.pos.x, s.car.pos.y, s.car.pos.z }; cs.rotMat = m;
				cs.vel = { s.car.vel.x, s.car.vel.y, s.car.vel.z }; cs.angVel = { s.carAngVel.x, s.carAngVel.y, s.carAngVel.z };
				c->SetState(cs);
				CarControls in; in.throttle = 1; c->controls = in;
				int contact[3] = {};
				for (int t = 1; t <= 36; t++)
				{
					a->Step(1);
					int w = 0; for (bool b : c->GetState().wheelsWithContact) w += b;
					if (t == 2) contact[0] = w; if (t == 12) contact[1] = w; if (t == 36) contact[2] = w;
				}
				gripStart += contact[0] >= 3; grip12 += contact[1] >= 3; grip36 += contact[2] >= 3;
			}
	printf("wall spawns %d: 3+ wheels gripping at tick 2: %d, tick 12: %d, tick 36: %d\n", walls, gripStart, grip12, grip36);
}
