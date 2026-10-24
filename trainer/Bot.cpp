#include "Bot.h"
#include <algorithm>
#include <cmath>

using namespace RocketSim;

namespace
{
	constexpr float kU2R = 3.14159265f / 32768.0f;
	const float kThrottle[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
	inline float Bin9(int i) { return -1.0f + 0.25f * i; }
	inline int Near9(float v) { return std::clamp(static_cast<int>(std::lround((std::clamp(v, -1.0f, 1.0f) + 1.0f) * 4.0f)), 0, 8); }
	inline int Near5(float v) { return std::clamp(static_cast<int>(std::lround((std::clamp(v, -1.0f, 1.0f) + 1.0f) * 2.0f)), 0, 4); }
}

CarControls Bot::Controls(const Act& a)
{
	CarControls c;
	c.throttle = kThrottle[a[0]];
	c.steer = Bin9(a[1]);
	c.pitch = Bin9(a[2]);
	c.yaw = Bin9(a[3]);
	c.roll = kThrottle[a[4]];
	c.jump = a[5] == 1;
	c.boost = a[6] == 1;
	c.handbrake = a[7] == 1;
	return c;
}

Bot::Act Bot::FromControls(const Frame& f)
{
	Act a;
	a[0] = Near5(f.throttle);
	a[1] = Near9(f.steer);
	a[2] = Near9(f.pitch);
	a[3] = Near9(f.yaw);
	a[4] = Near5(f.roll);
	a[5] = f.jump ? 1 : 0;
	a[6] = f.boost ? 1 : 0;
	a[7] = f.handbrake ? 1 : 0;
	return a;
}

void Bot::ActValues(const Act& a, float* o)
{
	CarControls c = Controls(a);
	o[0] = c.throttle; o[1] = c.steer; o[2] = c.pitch; o[3] = c.yaw; o[4] = c.roll;
	o[5] = c.jump ? 1.0f : 0.0f; o[6] = c.boost ? 1.0f : 0.0f; o[7] = c.handbrake ? 1.0f : 0.0f;
}

void Bot::Memory::Seed(const std::vector<Frame>& fr, int s)
{
	airTicks = 0; jumpHeld = 0;
	for (int i = s; i >= 0 && !fr[i].onGround; i--) airTicks++;
	for (int i = s - 1; i >= 0 && fr[i].jump; i--) jumpHeld++;
	const Frame& p = fr[std::max(0, s - 1)];
	prev[0] = p.throttle; prev[1] = p.steer; prev[2] = p.pitch; prev[3] = p.yaw; prev[4] = p.roll;
	prev[5] = p.jump ? 1.0f : 0.0f; prev[6] = p.boost ? 1.0f : 0.0f; prev[7] = p.handbrake ? 1.0f : 0.0f;
}

void Bot::Memory::After(const Frame& now, const CarControls& c)
{
	airTicks = now.onGround ? 0 : airTicks + 1;
	jumpHeld = c.jump ? jumpHeld + 1 : 0;
	prev[0] = c.throttle; prev[1] = c.steer; prev[2] = c.pitch; prev[3] = c.yaw; prev[4] = c.roll;
	prev[5] = c.jump ? 1.0f : 0.0f; prev[6] = c.boost ? 1.0f : 0.0f; prev[7] = c.handbrake ? 1.0f : 0.0f;
}

void Bot::BallPath::Build(Arena* ballArena, const Frame& f, int ticks)
{
	BallState bs;
	bs.pos = Vec(f.ballX, f.ballY, f.ballZ);
	bs.vel = Vec(f.ballVX, f.ballVY, f.ballVZ);
	bs.angVel = Vec(f.ballAVX, f.ballAVY, f.ballAVZ);
	ballArena->ball->SetState(bs);
	p.resize(ticks + 1);
	p[0] = bs.pos;
	for (int i = 1; i <= ticks; i++) { ballArena->Step(1); p[i] = ballArena->ball->GetState().pos; }
}

void Bot::Observe(const Frame& f, const Memory& m, const BallPath& path, int since, bool touched, float gs, float* o)
{
	// Everything is turned so the bot always attacks +Y (a half turn about the vertical when it shoots at -Y)
	const float sx = gs, sy = gs;
	RotMat R = Angle(f.carYaw * kU2R, f.carPitch * kU2R, f.carRoll * kU2R).ToRotMat();
	auto V = [&](float x, float y, float z) { return Vec(x * sx, y * sy, z); };
	const Vec cp = V(f.carX, f.carY, f.carZ), cv = V(f.carVX, f.carVY, f.carVZ), cw = V(f.carAVX, f.carAVY, f.carAVZ);
	const Vec fw = V(R.forward.x, R.forward.y, R.forward.z), rt = V(R.right.x, R.right.y, R.right.z), up = V(R.up.x, R.up.y, R.up.z);
	const Vec bp = V(f.ballX, f.ballY, f.ballZ), bv = V(f.ballVX, f.ballVY, f.ballVZ), bw = V(f.ballAVX, f.ballAVY, f.ballAVZ);
	int k = 0;
	auto put = [&](float v) { o[k++] = v; };
	auto put3 = [&](const Vec& v, float sx_, float sy_, float sz_) { put(v.x / sx_); put(v.y / sy_); put(v.z / sz_); };
	put3(cp, 4096.0f, 5120.0f, 2044.0f);
	put3(cv, 2300.0f, 2300.0f, 2300.0f);
	put3(cw, 5.5f, 5.5f, 5.5f);
	put3(fw, 1, 1, 1);
	put3(up, 1, 1, 1);
	put(f.boostAmount / 100.0f); put(f.onGround ? 1.0f : 0.0f); put(f.hasFlip ? 1.0f : 0.0f);
	put(std::min(m.airTicks / 120.0f, 2.0f)); put(std::min(m.jumpHeld / 24.0f, 1.0f));
	put3(bp, 4096.0f, 5120.0f, 2044.0f);
	put3(bv, 2300.0f, 2300.0f, 2300.0f);
	put3(bw, 6.0f, 6.0f, 6.0f);
	const Vec rel = bp - cp, rv = bv - cv;
	put(rel.Dot(fw) / 2300.0f); put(rel.Dot(rt) / 2300.0f); put(rel.Dot(up) / 2300.0f);
	put(rv.Dot(fw) / 2300.0f); put(rv.Dot(rt) / 2300.0f); put(rv.Dot(up) / 2300.0f);
	for (int j = 0; j < kPathN; j++)
	{
		Vec q = bp;
		if (!path.p.empty())
		{
			const int idx = std::clamp(since + kPathAt[j], 0, static_cast<int>(path.p.size()) - 1);
			q = V(path.p[idx].x, path.p[idx].y, path.p[idx].z);
		}
		const Vec d = q - cp;
		put(d.x / 2300.0f); put(d.y / 2300.0f); put(d.z / 2300.0f);
	}
	put(0.0f);   // (was time since the start: it meant different things in the warm start and in training, so it's off)
	put(touched ? 1.0f : 0.0f);
	for (int j = 0; j < 8; j++) put(m.prev[j]);
}

void Bot::Policy::Init(std::mt19937& rng, int width, int depth)
{
	std::vector<int> ps = { kObs }, vs = { kObs };
	for (int i = 0; i < depth; i++) { ps.push_back(width); vs.push_back(width); }
	ps.push_back(kLogits); vs.push_back(1);
	pi.Init(ps, rng, 0.01f);
	vf.Init(vs, rng, 0.1f);
	norm.Init(kObs);
}

void Bot::Policy::Logits(const float* obs, int B, MLP::Work& w, std::vector<float>& logits) const
{
	const float* y = pi.Forward(obs, B, w);
	logits.assign(y, y + static_cast<size_t>(B) * kLogits);
}

void Bot::LogSoftmax(const float* z, float* lp)
{
	for (int h = 0; h < kHeads; h++)
	{
		const int o = kOff[h], n = kBins[h];
		float mx = z[o];
		for (int j = 1; j < n; j++) mx = std::max(mx, z[o + j]);
		float s = 0;
		for (int j = 0; j < n; j++) s += std::exp(z[o + j] - mx);
		const float ls = mx + std::log(s);
		for (int j = 0; j < n; j++) lp[o + j] = z[o + j] - ls;
	}
}

using Bot::kOff;

Bot::Act Bot::Sample(const float* lp, std::mt19937& rng)
{
	Act a;
	std::uniform_real_distribution<float> u(0.0f, 1.0f);
	for (int h = 0; h < kHeads; h++)
	{
		const int o = kOff[h], n = kBins[h];
		float r = u(rng), c = 0;
		int pick = n - 1;
		for (int j = 0; j < n; j++) { c += std::exp(lp[o + j]); if (r < c) { pick = j; break; } }
		a[h] = pick;
	}
	return a;
}

Bot::Act Bot::Greedy(const float* lp)
{
	Act a;
	for (int h = 0; h < kHeads; h++)
	{
		const int o = kOff[h], n = kBins[h];
		int best = 0;
		for (int j = 1; j < n; j++) if (lp[o + j] > lp[o + best]) best = j;
		a[h] = best;
	}
	return a;
}

float Bot::LogProb(const float* lp, const Act& a)
{
	float s = 0;
	for (int h = 0; h < kHeads; h++) s += lp[kOff[h] + a[h]];
	return s;
}

float Bot::Entropy(const float* lp)
{
	float e = 0;
	for (int i = 0; i < kLogits; i++) e -= std::exp(lp[i]) * lp[i];
	return e;
}
