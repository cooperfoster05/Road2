// Turns real saves (real_saves.csv) into Combine save situations, and measures the realistic estimate on them.
// usage: situations real_saves.csv meshes out.csv
#include "Combine.h"
#include "third_party/RocketSim/src/RocketSim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
using namespace Combine;
int main(int argc, char** argv)
{
	RocketSim::Init(std::filesystem::path(argv[2]), true);
	std::ifstream f(argv[1]); std::string line; std::getline(f, line);
	FILE* out = fopen(argv[3], "w");
	fprintf(out, "source,bx,by,bz,bvx,bvy,bvz,bwx,bwy,bwz,cx,cy,cz,cvx,cvy,cvz,cwx,cwy,cwz,pitch,yaw,roll,boost,surface\n");
	int n = 0, clean = 0, rollA = 0, rollB = 0, unreal = 0;
	std::vector<float> sr, srP;
	while (std::getline(f, line))
	{
		std::vector<std::string> c; std::stringstream ss(line); std::string x;
		while (std::getline(ss, x, ',')) c.push_back(x);
		auto F = [&](int i) { return std::stof(c[i]); };
		n++;
		BallStart b{ {F(3), F(4), F(5)}, {F(6), F(7), F(8)}, {F(9), F(10), F(11)} };
		CarStart car{ {F(12), F(13), F(14)}, {F(15), F(16), F(17)} };
		Vec3 w{ F(18), F(19), F(20) };
		const float qw = F(21), qx = F(22), qy = F(23), qz = F(24), boost = F(25);
		const int gs = std::stoi(c[32]);
		const float dt = F(2);
		const float ax = std::fabs(car.pos.x), ay = std::fabs(car.pos.y), az = car.pos.z;
		car.onSurface = az < 60 || az > 1940 || ax > 3980 || (ay > 5000 && !(ax < 893 && az < 642)) || ax + ay > 7900;
		// Quaternion -> forward / right / up -> Unreal pitch, yaw, roll (roll's sign checked by rebuilding up)
		const float fx = 1 - 2 * (qy * qy + qz * qz), fy = 2 * (qx * qy + qw * qz), fz = 2 * (qx * qz - qw * qy);
		const float rz = 2 * (qy * qz + qw * qx);
		const float ux = 2 * (qx * qz + qw * qy), uy = 2 * (qy * qz - qw * qx), uz = 1 - 2 * (qx * qx + qy * qy);
		const float pitch = std::asin(std::clamp(fz, -1.0f, 1.0f)), yaw = std::atan2(fy, fx);
		auto upErr = [&](float roll) {
			const float sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw), sr = std::sin(roll), cr = std::cos(roll);
			const float ex = -(cr * sp * cy + sr * sy), ey = cy * sr - cr * sp * sy, ez = cr * cp;
			return std::fabs(ex - ux) + std::fabs(ey - uy) + std::fabs(ez - uz);
		};
		float roll = std::atan2(-rz, uz);
		if (upErr(roll) > upErr(-roll)) { roll = -roll; rollB++; } else rollA++;
		car.pitch = pitch; car.yaw = yaw; car.roll = roll; car.hasFacing = true;
		ReachResult r = SaveReach(b, car, gs);
		// clean only: the simulated ball was where the real one was at the save
		float dev;
		{
			std::unique_ptr<RocketSim::Arena> a(RocketSim::Arena::Create(RocketSim::GameMode::SOCCAR));
			RocketSim::BallState bs; bs.pos = RocketSim::Vec(b.pos.x, b.pos.y, b.pos.z); bs.vel = RocketSim::Vec(b.vel.x, b.vel.y, b.vel.z); bs.angVel = RocketSim::Vec(b.angVel.x, b.angVel.y, b.angVel.z);
			a->ball->SetState(bs); a->Step(std::max(1, (int)std::lround(dt * 120)));
			RocketSim::Vec q = a->ball->GetState().pos;
			dev = std::sqrt((q.x - F(26)) * (q.x - F(26)) + (q.y - F(27)) * (q.y - F(27)) + (q.z - F(28)) * (q.z - F(28)));
		}
		if (dev >= 250.0f || !r.onTarget || !r.possible) continue;
		clean++;
		if (r.tReachReal < 0 || r.tReachReal > dt + 0.05f)
		{
			unreal++;
			// what kind of case: car air/surface, height, speed, facing vs the save point
			const float sx = F(26), sy = F(27), sz = F(28);
			const float px = sx - car.pos.x, py = sy - car.pos.y, hd = std::sqrt(px * px + py * py);
			const float fx2 = std::cos(car.pitch) * std::cos(car.yaw), fy2 = std::cos(car.pitch) * std::sin(car.yaw);
			const float th = hd > 1 ? std::acos(std::clamp((fx2 * px + fy2 * py) / (hd * std::max(1e-3f, std::sqrt(fx2 * fx2 + fy2 * fy2))), -1.0f, 1.0f)) : 0;
			const float sp = std::sqrt(car.vel.x * car.vel.x + car.vel.y * car.vel.y + car.vel.z * car.vel.z);
			fprintf(stderr, "FAIL %s surf %d z %.0f speed %.0f vz %.0f theta %.2f dist %.0f saveZ %.0f realT %.2f est %.2f\n", car.onSurface ? "S" : "A", car.onSurface, car.pos.z, sp, car.vel.z, th, hd, sz, dt, r.tReachReal);
		}
		sr.push_back(r.slackReal);
		if (c[0].back() == 'P') srP.push_back(r.slackReal);
		// Turned so the ball heads for the blue goal (y = -5120)
		if (gs > 0)
		{
			b.pos.y = -b.pos.y; b.vel.y = -b.vel.y; b.angVel = { -b.angVel.x, b.angVel.y, -b.angVel.z };
			car.pos.y = -car.pos.y; car.vel.y = -car.vel.y; w = { -w.x, w.y, -w.z };
			car.yaw = -car.yaw; car.roll = -car.roll;
		}
		fprintf(out, "%c,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.1f,%d\n",
			c[0].back(), b.pos.x, b.pos.y, b.pos.z, b.vel.x, b.vel.y, b.vel.z, b.angVel.x, b.angVel.y, b.angVel.z,
			car.pos.x, car.pos.y, car.pos.z, car.vel.x, car.vel.y, car.vel.z, w.x, w.y, w.z, car.pitch, car.yaw, car.roll, boost, car.onSurface ? 1 : 0);
	}
	fclose(out);
	auto pr = [](const char* name, std::vector<float> v) {
		std::sort(v.begin(), v.end());
		auto q = [&](float p) { return v.empty() ? 0.0f : v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
		int neg = 0; for (float x : v) neg += x < -0.05f;
		printf("%s %zu: realistic slack min %.2f p2 %.2f p5 %.2f p10 %.2f p25 %.2f p50 %.2f p75 %.2f  (below -0.05: %d)\n", name, v.size(), v.empty() ? 0 : v[0], q(.02f), q(.05f), q(.10f), q(.25f), q(.5f), q(.75f), neg);
	};
	printf("rows %d, clean %d; roll sign A %d B %d; realistic estimate later than the real save: %d\n", n, clean, rollA, rollB, unreal);
	pr("all", sr); pr("pro", srP);
}
