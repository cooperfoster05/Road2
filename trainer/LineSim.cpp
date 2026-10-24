#include "LineSim.h"
#include "../EngineSim.h"
#include "../Mechanics.h"
#include <algorithm>
#include <cmath>

using namespace RocketSim;

LineSim::LineSim(int carBody)
{
	A.reset(Arena::Create(GameMode::SOCCAR));
	C = A->AddCar(Team::BLUE, EngineSim::Body(carBody));
}

void LineSim::Start(const Rep& r, int s, int nudge)
{
	rep = &r;
	const int n = static_cast<int>(r.fr.size());
	s0 = std::clamp(s, 1, n - 2);
	tick = s0;
	res = LineResult();
	res.frames.assign(r.fr.begin(), r.fr.begin() + s0 + 1);
	res.frames.reserve(s0 + 760);
	res.ctl.assign(s0, CarControls());
	firstTouch = lastTouch = handOff = stopAt = -1;
	bounced = over = finished = false;
	handsOffRule = true;
	// Like the engine: the line can run up to 6 s; no touch by a second and a half after your own touch
	// (and never past 4 s) counts as missing it
	maxTick = s0 + 720;
	const int yours = r.real.valid ? r.real.touchTick : s0 + 120;
	deadline = std::min(s0 + 480, std::max(s0 + 60, yours + 180));
	EngineSim::Start(A.get(), C, r.fr, s0, nudge);
}

void LineSim::Jitter(std::mt19937& rng, float carPos, float carVel, float ballPos, float ballVel)
{
	std::uniform_real_distribution<float> u(-1.0f, 1.0f);
	CarState cs = C->GetState();
	cs.pos += Vec(u(rng), u(rng), 0.25f * u(rng)) * carPos;
	if (cs.pos.z < 17.0f) cs.pos.z = std::max(cs.pos.z, C->GetState().pos.z);
	cs.vel += Vec(u(rng), u(rng), 0.25f * u(rng)) * carVel;
	C->SetState(cs);
	BallState bs = A->ball->GetState();
	bs.pos += Vec(u(rng), u(rng), u(rng)) * ballPos;
	bs.pos.z = std::max(bs.pos.z, 93.0f);
	bs.vel += Vec(u(rng), u(rng), u(rng)) * ballVel;
	A->ball->SetState(bs);
	// The start frame shows the jittered start (it's what the bot sees first)
	Frame& f = res.frames.back();
	cs = C->GetState();
	f.carX = cs.pos.x; f.carY = cs.pos.y; f.carZ = cs.pos.z; f.carVX = cs.vel.x; f.carVY = cs.vel.y; f.carVZ = cs.vel.z;
	f.ballX = bs.pos.x; f.ballY = bs.pos.y; f.ballZ = bs.pos.z; f.ballVX = bs.vel.x; f.ballVY = bs.vel.y; f.ballVZ = bs.vel.z;
}

bool LineSim::Tick(const CarControls& cIn)
{
	if (over) return false;
	CarControls c = (handsOffRule && HandsOff()) ? CarControls() : cIn;
	if (static_cast<int>(res.ctl.size()) <= tick) res.ctl.resize(tick + 1);
	res.ctl[tick] = c;
	Frame g = EngineSim::Step(A.get(), C, c, res.frames.back());
	const int i = tick;
	tick++;
	if (g.touch)
	{
		lastTouch = tick;
		if (firstTouch < 0) { firstTouch = tick; res.touchTick = tick; }
		if (handOff < 0) handOff = tick + 4;   // a few more ticks of the hit, then hands off (as the engine does)
	}
	res.frames.push_back(g);

	// The engine's end rules (Run in Engine.cpp): a goal ends it; a floor bounce after the touch ends the shot, but the
	// ball is followed while it's still rolling or bouncing toward the net
	const float bx = g.ballX, by = g.ballY, bz = g.ballZ, bvx = g.ballVX, bvy = g.ballVY, bvz = g.ballVZ;
	const float dirY = bvy >= 0.0f ? 1.0f : -1.0f;
	const float toLine = 5120.0f - by * dirY;
	const float tLine = std::fabs(bvy) > 1.0f ? toLine / std::fabs(bvy) : 1e9f;
	const bool rollingIn = std::fabs(bvy) > 300.0f && toLine > 0.0f && tLine < 6.0f && std::fabs(bx + bvx * tLine) < 950.0f;
	auto done = [&] { if (tick >= maxTick) over = true; return !over; };   // the engine's loop runs to maxTick at most
	if (!res.wentIn && std::fabs(by) > 5215.0f && std::fabs(bx) < 893.0f && bz < 643.0f)
	{
		res.wentIn = true;
		res.floorFirst = bounced;
		res.eventTick = tick;
		stopAt = tick + 60;
		return done();
	}
	if (stopAt >= 0)
	{
		if (tick >= stopAt && (res.wentIn || !rollingIn || tick >= res.eventTick + 900)) { over = true; return false; }
		return done();
	}
	if (lastTouch >= 0 && bz < 100.0f && bvz > 0.0f && tick - lastTouch > 10 && !ShotGeometry::BallWithCar(g))
	{
		bounced = true;
		res.eventTick = tick;
		stopAt = tick + 60;
		return done();
	}
	if (lastTouch < 0 && i >= deadline) { res.eventTick = tick; over = true; return false; }   // missed it entirely
	if (lastTouch >= 0 && tick - lastTouch > 480) { res.eventTick = tick; stopAt = tick + 60; }
	return done();
}

const LineResult& LineSim::Finish()
{
	if (finished) return res;
	while (!over) Tick(CarControls());
	finished = true;
	if (res.eventTick < 0) res.eventTick = static_cast<int>(res.frames.size()) - 1;
	res.touched = firstTouch >= 0;
	res.shot = ShotGeometry::Analyze(res.frames);
	RepSegments segs = Segmenter::Split(res.frames, res.eventTick);
	res.grade = Grader::GradeRep(res.frames, segs, res.shot, res.wentIn, res.floorFirst, rep->ref);
	res.sameMechanic = res.touched && (!rep->real.valid || Mechanics::SameMechanic(rep->real, rep->fr, res.shot, res.frames, s0, &res.why));
	if (!res.touched) res.why = "never touched the ball";
	res.quality = Quality(res, rep->ref);
	return res;
}

float LineSim::Quality(const LineResult& r, const ShotReference& ref)
{
	if (!r.touched) return -100000.0f;
	const ShotAnalysis& a = r.shot;
	float q = r.grade.overall * 10000.0f;
	if (!r.wentIn) return q - (a.reachesGoalLine ? std::min(5000.0f, a.missDist) : 5000.0f);
	auto ramp = [](float v, float lo, float hi) { return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); };
	float credit = 0;
	if (a.type == ShotType::Ground)
	{
		credit += 2.0f * ramp(a.crossZ, 200.0f, 520.0f);
		credit += ramp(a.shotSpeed, 1500.0f, 2100.0f);
	}
	else
	{
		float touchSec = (a.touchTick - a.startTick) / 120.0f;
		float vsBest = ref.valid ? touchSec - ref.touchSec : 0.0f;
		credit += ramp(a.crossZ, 150.0f, 420.0f);
		credit += ramp(a.shotSpeed, 1800.0f, 2900.0f);
		credit += ref.valid ? ramp(-vsBest, -0.6f, 0.0f) : 1.0f;
		credit += ramp(-a.aimErrDeg, -8.0f, -1.6f);
		credit += ramp(-a.cornerDist, -900.0f, -200.0f);
	}
	q += 2000.0f * credit + a.shotSpeed * 0.2f;
	if (a.bouncedIn) q -= 4000.0f;
	return q;
}
