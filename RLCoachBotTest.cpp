// Bot test (Oct 28): can the plugin put a second car in free play and drive it? If yes, the Combine gets 50s,
// shadow defense and kickoff battles: a bot that plays back a real player from a match replay, or one that
// Nexto drives. Console commands:
//   rlcoach_bot_spawn          spawn a bot (Octane) in free play
//   rlcoach_bot_mode <0-3>     0 the game's AI drives it, 1 held still by us, 2 plays back your newest rep's car path,
//                              3 plays back your newest rep's controller inputs from its start
//   rlcoach_bot_remove         remove it
// A status line on screen says what happened (and whether our per-tick hooks even fire for the bot).
#include "pch.h"
#include "RLCoach.h"
#include "RepLibrary.h"

void RLCoach::RegisterBotTest()
{
	cvarManager->registerNotifier("rlcoach_bot_spawn", [this](std::vector<std::string>) { BotSpawn(); },
		"Bot test: spawn a bot in free play", PERMISSION_ALL);
	cvarManager->registerNotifier("rlcoach_bot_mode", [this](std::vector<std::string> args) {
		int m = 0;
		if (args.size() >= 2) { try { m = std::stoi(args[1]); } catch (...) {} }
		BotSetMode(m);
	}, "Bot test: 0 game AI, 1 held still, 2 plays your newest rep's path, 3 plays your newest rep's inputs", PERMISSION_ALL);
	cvarManager->registerNotifier("rlcoach_bot_remove", [this](std::vector<std::string>) { BotRemove(); },
		"Bot test: remove the bot", PERMISSION_ALL);
}

void RLCoach::BotSpawn()
{
	if (!gameWrapper->IsInFreeplay()) { botStatus = "Bot test: open free play first"; LOG("{}", botStatus); return; }
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) { botStatus = "Bot test: no server (not in free play?)"; LOG("{}", botStatus); return; }
	const bool can = server.CanSpawnBots();
	const int before = server.GetCars().Count();
	server.SpawnBot(23, "Road2 Bot");   // 23 = Octane
	botStatus = std::format("Bot test: asked for a bot (CanSpawnBots said {}, cars before: {}). Looking for it...", can ? "yes" : "no", before);
	LOG("{}", botStatus);
	botTicksSeen = botInputsSeen = 0;
	// The car shows up a moment later
	gameWrapper->SetTimeout([this](GameWrapper*) { BotFind(); }, 0.5f);
}

void RLCoach::BotFind()
{
	botCarAddr = 0;
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper c = cars.Get(i);
		if (c.IsNull() || (!local.IsNull() && c.memory_address == local.memory_address)) continue;
		botCarAddr = c.memory_address;
		PriWrapper pri = c.GetPRI();
		const bool flagged = !pri.IsNull() && pri.GetbBot();
		botStatus = std::format("Bot test: found a second car ({} cars in all; marked as a bot: {}). Mode {}.", cars.Count(), flagged ? "yes" : "no", botMode);
		LOG("{}", botStatus);
		return;
	}
	botStatus = std::format("Bot test: no second car showed up ({} car(s) in the game). Spawning bots may not work in free play.", cars.Count());
	LOG("{}", botStatus);
}

void RLCoach::BotRemove()
{
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull() || !botCarAddr) { botStatus.clear(); botCarAddr = 0; return; }
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper c = cars.Get(i);
		if (c.IsNull() || c.memory_address != botCarAddr) continue;
		AIControllerWrapper ai = c.GetAIController();
		if (!ai.IsNull()) server.RemovePlayer(ai);
		else c.Demolish();
	}
	botCarAddr = 0;
	botMode = 0;
	botStatus = "Bot test: removed";
	LOG("{}", botStatus);
}

void RLCoach::BotSetMode(int mode)
{
	botMode = std::clamp(mode, 0, 3);
	botTick = 0;
	botFrames.clear();
	if (botMode >= 2 && writer)
	{
		// Your newest rep that has a recording
		auto reps = RepLibrary::ListReps(writer->Folder());
		std::string newest;
		for (const RepListItem& it : reps)
			if (it.hasRecording && it.file > newest) newest = it.file;
		std::string err;
		if (newest.empty() || !RepLibrary::LoadRep(writer->Folder(), newest, botFrames, err) || botFrames.size() < 10)
		{
			botFrames.clear();
			botStatus = "Bot test: couldn't load your newest rep (record one first)";
			LOG("{}", botStatus);
			botMode = 0;
			return;
		}
		botStatus = std::format("Bot test: mode {} with {} ({:.1f} s)", botMode, newest, botFrames.size() / 120.0f);
	}
	else
		botStatus = std::format("Bot test: mode {} ({})", botMode, botMode == 0 ? "the game's AI drives" : "held still by Road2");
	LOG("{}", botStatus);
}

bool RLCoach::IsBotCar(CarWrapper& car)
{
	return botCarAddr && !car.IsNull() && car.memory_address == botCarAddr;
}

void RLCoach::BotInput(CarWrapper& car, void* params)
{
	if (!IsBotCar(car) || !params) return;
	botInputsSeen++;
	if (botMode == 1 || botMode == 2)
	{
		*reinterpret_cast<ControllerInput*>(params) = ControllerInput{};
	}
	else if (botMode == 3 && !botFrames.empty())
	{
		const Frame& f = botFrames[std::min(botTick, static_cast<int>(botFrames.size()) - 1)];
		ControllerInput in{};
		in.Throttle = f.throttle; in.Steer = f.steer; in.Pitch = f.pitch; in.Yaw = f.yaw; in.Roll = f.roll;
		in.Handbrake = f.handbrake ? 1 : 0;
		in.Jump = f.jump ? 1 : 0;
		in.ActivateBoost = f.boost ? 1 : 0;
		in.HoldingBoost = in.ActivateBoost;
		*reinterpret_cast<ControllerInput*>(params) = in;
	}
}

void RLCoach::BotTick(CarWrapper& car)
{
	if (!IsBotCar(car)) return;
	botTicksSeen++;
	if (botFrames.empty() || botMode < 2) return;
	const int n = static_cast<int>(botFrames.size());
	auto place = [&](const Frame& f) {
		car.SetLocation(Vector(f.carX, f.carY, f.carZ));
		car.SetRotation(Rotator(f.carPitch, f.carYaw, f.carRoll));
		car.SetVelocity(Vector(f.carVX, f.carVY, f.carVZ));
		car.SetAngularVelocity(Vector(f.carAVX, f.carAVY, f.carAVZ), false);
	};
	if (botMode == 2) place(botFrames[botTick]);              // the path, every tick
	else if (botTick == 0) place(botFrames[0]);              // inputs: placed once at the start, then it drives
	if (++botTick >= n) botTick = 0;                         // loop
}

void RLCoach::DrawBotStatus(CanvasWrapper& canvas)
{
	if (botStatus.empty()) return;
	canvas.SetColor(255, 255, 255, 230);
	canvas.SetPosition(Vector2{ 24, 160 });
	canvas.DrawString(botStatus, 1.2f, 1.2f);
	if (botCarAddr)
	{
		canvas.SetPosition(Vector2{ 24, 184 });
		canvas.DrawString(std::format("Bot hooks seen: {} input, {} tick. Playback tick {} of {}.", botInputsSeen, botTicksSeen, botTick, botFrames.size()), 1.2f, 1.2f);
	}
}
