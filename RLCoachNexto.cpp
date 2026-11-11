// Nexto playing the Road2 Bot (Nov 4). Nexto.cpp is the network; this file feeds it the game and drives the bot:
// every 8 ticks it looks at the field and picks one of its 90 control choices, which go on the bot 6 ticks later
// (the timing it has in RLBot; lower Combine levels make it later). Kickoffs hand over after the real taker's kickoff
// (Nexto's own scripted kickoff was taken out for v1: it ignored where it spawned).
#include "pch.h"
#include "RLCoach.h"
#include <algorithm>
#include <cmath>

namespace
{
	constexpr float kU2R = 3.14159265f / 32768.0f;

	Nexto::Car FromCar(CarWrapper& c, int team)
	{
		Nexto::Car n;
		const Vector p = c.GetLocation(), v = c.GetVelocity(), w = c.GetAngularVelocity();
		const Rotator r = c.GetRotation();
		const float pi = r.Pitch * kU2R, ya = r.Yaw * kU2R, ro = r.Roll * kU2R;
		const float cp = std::cos(pi), sp = std::sin(pi), cy = std::cos(ya), sy = std::sin(ya), cr = std::cos(ro), sr = std::sin(ro);
		const float fw[3] = { cp * cy, cp * sy, sp };
		const float up[3] = { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
		const float pv[3] = { p.X, p.Y, p.Z }, vv[3] = { v.X, v.Y, v.Z }, wv[3] = { w.X, w.Y, w.Z };
		for (int k = 0; k < 3; k++) { n.pos[k] = pv[k]; n.vel[k] = vv[k]; n.ang[k] = wv[k]; n.fw[k] = fw[k]; n.up[k] = up[k]; }
		BoostWrapper b = c.GetBoostComponent();
		n.boost = b.IsNull() ? 0.0f : std::clamp(b.GetCurrentBoostAmount(), 0.0f, 1.0f);
		n.onGround = c.IsOnGround();
		n.hasFlip = c.HasFlip() != 0;
		n.team = team;
		return n;
	}


	ControllerInput ToInput(const Nexto::Controls& a)
	{
		ControllerInput in{};
		in.Throttle = a[0]; in.Steer = a[1]; in.Pitch = a[2]; in.Yaw = a[3]; in.Roll = a[4];
		in.Jump = a[5] > 0.0f ? 1 : 0;
		in.ActivateBoost = a[6] > 0.0f ? 1 : 0;
		in.HoldingBoost = in.ActivateBoost;
		in.Handbrake = a[7] > 0.0f ? 1 : 0;
		return in;
	}
}

bool RLCoach::NextoReady()
{
	if (nexto.Ready()) return true;
	if (nextoTried) return false;
	nextoTried = true;
	const std::filesystem::path file = writer ? writer->Folder() / "nexto" / "nexto.bin" : std::filesystem::path(gameWrapper->GetDataFolderW()) / "Road2" / "nexto" / "nexto.bin";
	if (!nexto.Load(file, nextoError)) { LOG("Nexto: {}", nextoError); return false; }
	LOG("Nexto: loaded");
	return true;
}

void RLCoach::NextoPadsReset()
{
	for (int i = 0; i < Nexto::kPads; i++) { nextoPadsUp[i] = true; nextoPadDown[i] = 0; }
}

void RLCoach::NextoStart()
{
	NextoPadsReset();
	nextoPrev = nextoPending = nextoNow = Nexto::Controls{};
	nextoTick = 0;
	nextoQueue.clear();
	nextoSeed = combineSeed * 2654435761u + static_cast<uint32_t>(combineIndex) * 97u + static_cast<uint32_t>(combineResets) + 1u;
	botMode = 5;
}

void RLCoach::NextoStep(CarWrapper& me, BallWrapper& ball)
{
	CarWrapper bot = BotCar();
	if (bot.IsNull() || ball.IsNull() || botMode != 5 || !nexto.Ready()) return;

	// The pads: there's no list of them to read, so a pad goes down when a car that isn't full drives over it, for as
	// long as the game keeps it down (4 s small, 10 s big)
	for (int i = 0; i < Nexto::kPads; i++)
		if (!nextoPadsUp[i] && --nextoPadDown[i] <= 0) nextoPadsUp[i] = true;
	for (CarWrapper* c : { &me, &bot })
	{
		const Vector p = c->GetLocation();
		BoostWrapper b = c->GetBoostComponent();
		if (b.IsNull() || b.GetCurrentBoostAmount() >= 0.999f || p.Z > 200.0f) continue;
		const int pad = Nexto::PadAt(p.X, p.Y);
		if (pad < 0 || !nextoPadsUp[pad]) continue;
		const bool big = Nexto::kPadPos[pad][2] > 72.0f;
		const float dx = p.X - Nexto::kPadPos[pad][0], dy = p.Y - Nexto::kPadPos[pad][1];
		if (dx * dx + dy * dy > (big ? 208.0f * 208.0f : 144.0f * 144.0f)) continue;
		nextoPadsUp[pad] = false;
		nextoPadDown[pad] = big ? 1200 : 480;
	}

	const Vector bl = ball.GetLocation(), bv = ball.GetVelocity();

	// A decision every 8 ticks, on the bot 6 ticks after it looked (the first one at once)
	const int phase = nextoTick % Nexto::kTickSkip;
	if (phase == 0)
	{
		// The Combine always has you attacking the orange goal and the bot defending it, whatever team the game put
		// the bot on, so Nexto is told it's orange
		const int botTeam = 1;
		std::vector<Nexto::Car> others{ FromCar(me, 0) };
		Nexto::Ball b;
		const Vector ba = ball.GetAngularVelocity();
		const float bp[3] = { bl.X, bl.Y, bl.Z }, bvv[3] = { bv.X, bv.Y, bv.Z }, bav[3] = { ba.X, ba.Y, ba.Z };
		for (int k = 0; k < 3; k++) { b.pos[k] = bp[k]; b.vel[k] = bvv[k]; b.ang[k] = bav[k]; }
		const int a = nexto.Act(FromCar(bot, botTeam), others, b, nextoPadsUp, nextoPrev, combineNextoBeta, nextoSeed + static_cast<uint32_t>(nextoTick));
		nextoPending = nexto.Action(a);
		nextoPrev = nextoPending;
		if (nextoTick == 0) nextoNow = nextoPending;
		else nextoQueue.push_back({ nextoTick + combineNextoDelay, nextoPending });
	}
	while (!nextoQueue.empty() && nextoQueue.front().first <= nextoTick) { nextoNow = nextoQueue.front().second; nextoQueue.erase(nextoQueue.begin()); }
	nextoTick++;
	bot.SetInput(ToInput(nextoNow));
}

// The bot's inputs while Nexto drives it (the pre-input hook: the game reads these this tick)
void RLCoach::NextoInput(void* params)
{
	*reinterpret_cast<ControllerInput*>(params) = ToInput(nextoNow);
}

// When Nexto takes the bot over (s from the start of the item)
//  - Score on a goalie: after your take, later at lower levels (more room to set it up before it closes you down)
//  - 50s: just before the real contact, so the run-in is the real one and Nexto plays the 50 itself
//  - Kickoffs: just before the real taker gets to the ball (its real speedflip, then Nexto plays what comes after)
//  - Shadow defense: at once (it works best that way)
float RLCoach::NextoHandoff() const
{
	if (combineIndex >= static_cast<int>(combineChallenges.size())) return 0.0f;
	const Combine::Challenge& c = combineChallenges[combineIndex];
	if (combineEvent == "SG")
	{
		static const float kRoom[Combine::kSaveLevels] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.0f };
		return c.tContact + kRoom[std::clamp(combineLevel, 1, Combine::kSaveLevels) - 1];
	}
	if (combineEvent == "FF") return std::max(0.0f, c.tContact - 0.3f);
	if (combineEvent == "KO") return std::max(0.0f, c.tContact - 0.15f);
	return 0.0f;
}

// ---- The spanish's second bot: their last back ----
CarWrapper RLCoach::Bot2Car()
{
	if (!bot2Addr) return CarWrapper(0);
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return CarWrapper(0);
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper c = cars.Get(i);
		if (!c.IsNull() && c.memory_address == bot2Addr) return c;
	}
	return CarWrapper(0);
}

void RLCoach::Bot2Spawn()
{
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	wantBot2 = true;
	server.SetbRandomizedBotLoadouts(0);
	server.SpawnBot(23, "Road2 Bot 2");
	gameWrapper->SetTimeout([this](GameWrapper*) { BotFind(); }, 0.5f);
}

void RLCoach::Bot2Remove()
{
	// Every car that isn't yours or bot 1: bot 2, and the new car it comes back as after you demo it (Nov 4: that one
	// used to stay on for the next kickoff)
	wantBot2 = false;
	bot2Addr = 0;
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	std::vector<CarWrapper> extra;
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper c = cars.Get(i);
		if (c.IsNull() || c.memory_address == botCarAddr || (!local.IsNull() && c.memory_address == local.memory_address)) continue;
		extra.push_back(c);
	}
	for (CarWrapper& c : extra)
	{
		AIControllerWrapper ai = c.GetAIController();
		if (!ai.IsNull()) ai.DoNothing();
		server.RemoveCar(c);
		if (!ai.IsNull()) server.RemovePlayer(ai);
	}
}

namespace
{
	// Where their last back starts: one of the standard kickoff spawns that isn't the taker's (orange side)
	Vector Bot2Spot(const Combine::Challenge& c, uint32_t seed)
	{
		const float tx = c.bot.empty() ? 0.0f : c.bot.front().pos.x;
		const bool takerDiagonal = std::fabs(tx) > 1500.0f, takerBack = std::fabs(tx) < 100.0f;
		const bool side = (seed & 1u) != 0;
		if (takerBack) return Vector(side ? 256.0f : -256.0f, 3840.0f, 17.0f);
		if (takerDiagonal && (seed & 2u)) return Vector(tx > 0 ? -256.0f : 256.0f, 3840.0f, 17.0f);
		return Vector(0.0f, 4608.0f, 17.0f);
	}
	uint32_t Bot2Seed(uint32_t setSeed, int index) { return setSeed * 2891336453u + static_cast<uint32_t>(index) * 1103515245u + 12345u; }
	Nexto::Controls bot2Ctl{};
}

void RLCoach::Bot2Place(bool frozen)
{
	CarWrapper b2 = Bot2Car();
	if (b2.IsNull() || combineIndex >= static_cast<int>(combineChallenges.size())) return;
	const Vector p = Bot2Spot(combineChallenges[combineIndex], Bot2Seed(combineSeed, combineIndex));
	const float yaw = std::atan2(-p.Y, -p.X);   // facing the ball
	b2.SetInput(ControllerInput{});
	b2.SetLocation(p);
	b2.SetRotation(Rotator(0, static_cast<int>(yaw / kU2R), 0));
	b2.SetVelocity(Vector(0, 0, 0));
	b2.SetAngularVelocity(Vector(0, 0, 0), false);
	BoostWrapper bb = b2.GetBoostComponent();
	if (!bb.IsNull()) { bb.SetUnlimitedBoostRefCount(0); bb.SetCurrentBoostAmount(0.333f); }
	bot2Ctl = Nexto::Controls{};
	if (!frozen)
	{
		bot2Plan = static_cast<int>((Bot2Seed(combineSeed, combineIndex) >> 3) & 1u);
		bot2Stage = 0;
		bot2Seen = true;
		bot2Demoed = false;
		bot2Bumped = false;
		bot2DemoTick = -1;
		bot2PrevVel = Vector(0, 0, 0);
	}
}

// Their last back on a kickoff: either cheats up toward the middle, or takes its corner boost and goes back to net
void RLCoach::Bot2Drive()
{
	CarWrapper b2 = Bot2Car();
	if (b2.IsNull())
	{
		// it was there at the go and now it's gone: you demoed it
		if (bot2Seen && !bot2Demoed) { bot2Demoed = true; bot2DemoTick = combineLiveTicks; }
		return;
	}
	const Vector p = b2.GetLocation();
	// a bump: its speed jumps while you're right on it (a demo takes the car away instead, above)
	{
		const Vector v = b2.GetVelocity();
		CarWrapper me = gameWrapper->GetLocalCar();
		if (!me.IsNull() && (me.GetLocation() - p).magnitude() < 320.0f && (v - bot2PrevVel).magnitude() > 450.0f) bot2Bumped = true;
		bot2PrevVel = v;
	}
	const Vector start = combineIndex < static_cast<int>(combineChallenges.size()) ? Bot2Spot(combineChallenges[combineIndex], Bot2Seed(combineSeed, combineIndex)) : p;
	Vector target;
	if (bot2Plan == 0) target = Vector(start.X * 0.4f, 1400.0f, 17.0f);                                     // cheat up
	else target = bot2Stage == 0 ? Vector(start.X >= 0.0f ? 3072.0f : -3072.0f, 4096.0f, 17.0f) : Vector(0.0f, 4700.0f, 17.0f);   // boost, then net
	const float dx = target.X - p.X, dy = target.Y - p.Y, dist = std::sqrt(dx * dx + dy * dy);
	if (dist < 250.0f && bot2Plan == 1 && bot2Stage == 0) bot2Stage = 1;
	const bool there = dist < 250.0f && (bot2Plan == 0 || bot2Stage == 1);
	const float yaw = b2.GetRotation().Yaw * kU2R;
	float diff = std::atan2(dy, dx) - yaw;
	while (diff > 3.14159265f) diff -= 6.2831853f;
	while (diff < -3.14159265f) diff += 6.2831853f;
	bot2Ctl = Nexto::Controls{};
	if (!there)
	{
		bot2Ctl[0] = 1.0f;
		bot2Ctl[1] = std::clamp(diff * 2.5f, -1.0f, 1.0f);
		bot2Ctl[6] = (std::fabs(diff) < 0.3f && dist > 1200.0f && bot2Plan == 1) ? 1.0f : 0.0f;
		bot2Ctl[7] = std::fabs(diff) > 1.4f ? 1.0f : 0.0f;
	}
	ControllerInput in{};
	in.Throttle = bot2Ctl[0]; in.Steer = bot2Ctl[1]; in.ActivateBoost = bot2Ctl[6] > 0 ? 1 : 0; in.HoldingBoost = in.ActivateBoost; in.Handbrake = bot2Ctl[7] > 0 ? 1 : 0;
	b2.SetInput(in);
}

void RLCoach::Bot2Input(CarWrapper&, void* params)
{
	ControllerInput in{};
	if (combinePhase != CombinePhase::Live) { *reinterpret_cast<ControllerInput*>(params) = in; return; }   // still between items
	in.Throttle = bot2Ctl[0]; in.Steer = bot2Ctl[1]; in.ActivateBoost = bot2Ctl[6] > 0 ? 1 : 0; in.HoldingBoost = in.ActivateBoost; in.Handbrake = bot2Ctl[7] > 0 ? 1 : 0;
	*reinterpret_cast<ControllerInput*>(params) = in;
}
