// Bot test (Oct 28): can the plugin put a second car in free play and drive it? If yes, the Combine gets 50s,
// shadow defense and kickoff battles: a bot that plays back a real player from a match replay, or one that
// Nexto drives. Console commands:
//   road2_bot_spawn          spawn a bot (Octane) in free play
//   road2_bot_mode <0-3>     0 the game's AI drives it, 1 held still by us, 2 plays back your newest rep's car path,
//                              3 plays back your newest rep's controller inputs from its start
//   road2_bot_remove         remove it
// A status line on screen says what happened (and whether our per-tick hooks even fire for the bot).
#include "pch.h"
#include "RLCoach.h"
// (Nov 6) the bot's housekeeping lines only go to the log while Developer tools is on or the bot test was used, so
// a tester's bakkesmod.log isn't filled with them
#define BOTLOG(...) do { if (botTestUsed || (devTools && *devTools)) LOG(__VA_ARGS__); } while (0)
#include "RepLibrary.h"
#include "bakkesmod/core/bot_loadout_structs.h"
#include "bakkesmod/utilities/LoadoutUtilities.h"
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>

void RLCoach::RegisterBotTest()
{
	cvarManager->registerNotifier("road2_bot_spawn", [this](std::vector<std::string>) { botTestUsed = true; BotSpawn(); },
		"Bot test: spawn a bot in free play", PERMISSION_ALL);
	cvarManager->registerNotifier("road2_bot_mode", [this](std::vector<std::string> args) {
		int m = 0;
		if (args.size() >= 2) { try { m = std::stoi(args[1]); } catch (...) {} }
		botTestUsed = true;
		BotSetMode(m);
	}, "Bot test: 0 game AI, 1 held still, 2 plays your newest rep's path, 3 plays your newest rep's inputs", PERMISSION_ALL);
	cvarManager->registerNotifier("road2_bot_remove", [this](std::vector<std::string>) { botTestUsed = true; BotRemove(); },
		"Bot test: remove the bot", PERMISSION_ALL);
	cvarManager->registerNotifier("road2_bot_capture", [this](std::vector<std::string> args) {
		std::string name;
		for (size_t i = 1; i < args.size(); i++) name += (i > 1 ? " " : "") + args[i];
		BotCaptureLoadout(name);
	}, "Copy one of your garage presets (by name, its orange side) onto the Road2 Bot", PERMISSION_ALL);
}

// ---- The bot's look ----
// bot_loadout.txt, one setting per line:
//   slot <slot> <product id> [paint id]    (slots: 0 body, 1 decal, 2 wheels, 3 boost, 4 antenna, 5 topper,
//                                           7 paint finish, 12 accent finish, 13 engine audio, 14 trail, 15 goal explosion)
//   primary <colour id> / accent <colour id> / primary_finish <id> / accent_finish <id>
// Products go on by id, so the bot can wear items the player doesn't own (it's only ever seen offline)
namespace
{
	constexpr int kOctane = 23;
	constexpr int kOrange = 1;

	BotLoadoutData DefaultLook()
	{
		BotLoadoutData d;
		d.products[0] = kOctane;
		d.team = kOrange;
		return d;
	}

	// The game's item paints (the SDK's pluginsdk::Itempaint), by name: "Titanium White", "titanium white" and
	// "TitaniumWhite" all work
	const char* kPaintNames[19] = { "None", "Crimson", "Lime", "Black", "Sky Blue", "Cobalt", "Burnt Sienna", "Forest Green",
		"Purple", "Pink", "Orange", "Grey", "Titanium White", "Saffron", "Gold", "Rose Gold", "White Gold", "Onyx", "Platinum" };
	std::string Squash(const std::string& s)
	{
		std::string o;
		for (char ch : s) if (!std::isspace(static_cast<unsigned char>(ch))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return o;
	}
	int PaintId(const std::string& name)
	{
		for (int i = 1; i < 19; i++) if (Squash(name) == Squash(kPaintNames[i])) return i;
		return 0;
	}

	// paintNames: paints given by name ("slot 3 63 Titanium White"), turned into ids with the game's paint list
	BotLoadoutData ReadLook(const std::filesystem::path& file, bool& found, std::map<int, std::string>& paintNames)
	{
		BotLoadoutData d = DefaultLook();
		found = false;
		std::ifstream in(file);
		if (!in) return d;
		found = true;
		std::string line;
		while (std::getline(in, line))
		{
			const size_t hash = line.find('#');
			if (hash != std::string::npos) line = line.substr(0, hash);
			std::istringstream ss(line);
			std::string key;
			if (!(ss >> key)) continue;
			if (key == "slot")
			{
				int slot = -1, product = 0;
				ss >> slot >> product;
				if (slot < 0 || product <= 0) continue;
				d.products[slot] = product;
				// the rest of the line: a paint id, or a paint's name
				std::string rest;
				std::getline(ss, rest);
				while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
				while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
				if (rest.empty()) continue;
				if (std::all_of(rest.begin(), rest.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }))
				{
					const int paint = std::atoi(rest.c_str());
					if (paint > 0) d.product_attributes[slot] = { { BotLoadoutData::Attribute::Type::PAINT, paint } };
				}
				else paintNames[slot] = rest;
			}
			else if (key == "primary") { int v = 0; ss >> v; d.team_color_id = static_cast<unsigned char>(v); }
			else if (key == "accent") { int v = 0; ss >> v; d.custom_color_id = static_cast<unsigned char>(v); }
			else if (key == "primary_finish") ss >> d.team_finish_id;
			else if (key == "accent_finish") ss >> d.custom_finish_id;
		}
		if (!d.products.count(0)) d.products[0] = kOctane;
		d.team = kOrange;
		return d;
	}
}

void RLCoach::BotCaptureLoadout(const std::string& preset)
{
	if (preset.empty()) { BOTLOG("Road2 Bot: give a preset name, e.g. road2_bot_capture \"Road2\" (or me: what you're wearing now)"); return; }
	if (preset == "me")
	{
		// (Nov 4) What you're wearing right now, paints included: read off your own car, which a preset can't give
		CarWrapper me = gameWrapper->GetLocalCar();
		PriWrapper pri = me.IsNull() ? PriWrapper(0) : me.GetPRI();
		if (pri.IsNull()) { BOTLOG("Road2 Bot: get into free play first"); return; }
		const std::optional<pluginsdk::Loadout> mine = LoadoutUtilities::GetLoadoutFromPri(pri, pri.GetTeamNum2() == 1 ? 1 : 0);
		const std::optional<pluginsdk::Loadout> orangeSide = LoadoutUtilities::GetLoadoutFromPri(pri, 1);
		if (!mine) { BOTLOG("Road2 Bot: couldn't read your loadout"); return; }
		ItemsWrapper items = gameWrapper->GetItemsWrapper();
		std::ostringstream out;
		out << "# The Road2 Bot's look, copied from what you were wearing (road2_bot_capture me)\n";
		out << "# Paints: the paint's name after the item, e.g. \"slot 2 386 Titanium White\"\n";
		for (const auto& [slot, item] : mine->items)
		{
			if (item.product_id <= 0) continue;
			int paint = 0;
			for (const auto& at : item.attributes) if (at.type == pluginsdk::ItemAttribute::AttributeType::PAINT) paint = at.value;
			std::string name;
			if (!items.IsNull()) { ProductWrapper pw = items.GetProduct(item.product_id); if (!pw.IsNull()) name = pw.GetLongLabel().ToString(); }
			out << "slot " << static_cast<int>(slot) << " " << item.product_id;
			if (paint > 0 && paint < 19) out << " " << kPaintNames[paint];
			if (!name.empty()) out << "    # " << name;
			out << "\n";
		}
		const pluginsdk::Loadout& col = orangeSide ? *orangeSide : *mine;
		int primary = 0, accent = 0;
		if (col.paint_finish.team_paint) { primary = col.paint_finish.team_paint->team_color_id; accent = col.paint_finish.team_paint->custom_color_id; }
		out << "primary " << primary << "\naccent " << accent << "\n";
		const std::string text = out.str();
		if (writer) { std::ofstream f(writer->Folder() / "bot_loadout.txt"); f << text; }
		BOTLOG("Road2 Bot: copied what you're wearing to bot_loadout.txt:\n{}", text);
		botLookAddr = 0;
		CarWrapper bot = BotCar();
		if (!bot.IsNull()) BotApplyLook(bot);
		return;
	}
	LoadoutSaveWrapper save = gameWrapper->GetUserLoadoutSave();
	if (save.IsNull()) { BOTLOG("Road2 Bot: couldn't read your garage presets"); return; }
	LoadoutSetWrapper set = save.GetPreset(preset);
	if (set.IsNull()) { BOTLOG("Road2 Bot: no preset called \"{}\"", preset); return; }
	ItemsWrapper items = gameWrapper->GetItemsWrapper();
	// One side of the preset: each slot's item, its paint (from the owned copy of the item: the instance whose product
	// matches, wherever it sits in the list), and the colours. Logged in full, so a wrong look can be traced
	struct Item { int slot, product, paint; std::string name; };
	struct Side { std::vector<Item> items; int primary = 0, accent = 0, pFinish = 0, aFinish = 0, score = 0; };
	auto read = [&](LoadoutWrapper lw, const char* label) {
		Side sd;
		if (lw.IsNull()) { BOTLOG("Road2 Bot: {} side: none", label); return sd; }
		ArrayWrapper<int> ids = lw.GetLoadout();
		std::vector<ProductInstanceID> inst = lw.GetOnlineLoadoutV2();
		for (int slot = 0; slot < ids.Count(); slot++)
		{
			const int product = ids.Get(slot);
			if (product <= 0) continue;
			Item it{ slot, product, -1, "" };
			if (!items.IsNull())
			{
				ProductWrapper pw = items.GetProduct(product);
				if (!pw.IsNull()) it.name = pw.GetLongLabel().ToString();
				for (size_t q = 0; q < inst.size() && it.paint < 0; q++)
				{
					if (!ItemsWrapper::IsOnlineID(inst[q])) continue;
					OnlineProductWrapper op = items.GetOnlineProduct(inst[q]);
					if (op.IsNull() || op.GetProductID() != product) continue;
					it.paint = 0;
					ArrayWrapper<ProductAttributeWrapper> attrs = op.GetAttributes();
					for (int a = 0; a < attrs.Count(); a++)
					{
						ProductAttributeWrapper at = attrs.Get(a);
						if (!at.IsNull() && at.GetAttributeType() == "ProductAttribute_Painted_TA")
							it.paint = ProductAttribute_PaintedWrapper(at.memory_address).GetPaintID();
					}
				}
			}
			BOTLOG("Road2 Bot: {} slot {} = {} ({}), paint {}", label, slot, product, it.name, it.paint);
			sd.score += 1 + (it.paint > 0 ? 2 : 0);
			sd.items.push_back(it);
		}
		sd.primary = lw.GetPrimaryPaintColorId(); sd.accent = lw.GetAccentPaintColorId();
		sd.pFinish = lw.GetPrimaryFinishId(); sd.aFinish = lw.GetAccentFinishId();
		BOTLOG("Road2 Bot: {} colours {} / {}, finishes {} / {}", label, sd.primary, sd.accent, sd.pFinish, sd.aFinish);
		return sd;
	};
	const LoadoutSetData data = set.GetLoadoutData();
	const Side blue = read(data.blue, "blue"), orange = read(data.orange, "orange");
	// The items from whichever side you dressed up more (the orange side of a preset is often left as it came); the
	// colours from the orange side, since the bot plays orange
	const bool useBlue = blue.score > orange.score;
	const Side& pick = useBlue ? blue : orange;
	std::ostringstream out;
	out << "# The Road2 Bot's look, copied from your preset \"" << preset << "\" (items from its " << (useBlue ? "blue" : "orange")
		<< " side, colours from its orange side)\n";
	for (const Item& it : pick.items)
	{
		out << "slot " << it.slot << " " << it.product;
		if (it.paint > 0) out << " " << it.paint;
		if (!it.name.empty()) out << "    # " << it.name;
		out << "\n";
	}
	out << "primary " << orange.primary << "\n" << "accent " << orange.accent << "\n";
	out << "primary_finish " << pick.pFinish << "\n" << "accent_finish " << pick.aFinish << "\n";
	const std::string text = out.str();
	if (writer)
	{
		std::ofstream f(writer->Folder() / "bot_loadout.txt");
		f << text;
	}
	BOTLOG("Road2 Bot: copied preset \"{}\" to bot_loadout.txt:\n{}", preset, text);
	botLookAddr = 0;   // the next bot (or this one, below) gets it
	CarWrapper bot = BotCar();
	if (!bot.IsNull()) BotApplyLook(bot);
}

void RLCoach::BotApplyLook(CarWrapper& bot)
{
	if (bot.IsNull() || bot.memory_address == botLookAddr) return;
	botLookAddr = bot.memory_address;
	PriWrapper pri = bot.GetPRI();
	if (pri.IsNull()) return;
	// Orange, always (free play gives you blue)
	if (pri.GetTeamNum2() != kOrange) pri.ServerChangeTeam(kOrange);
	bool fromFile = false;
	std::map<int, std::string> paintNames;
	BotLoadoutData look = ReadLook(writer ? writer->Folder() / "bot_loadout.txt" : std::filesystem::path(), fromFile, paintNames);
	if (!paintNames.empty())
	{
		for (auto& [slot, name] : paintNames)
		{
			const int id = PaintId(name);
			if (id > 0) look.product_attributes[slot] = { { BotLoadoutData::Attribute::Type::PAINT, id } };
			BOTLOG("Road2 Bot: slot {} paint \"{}\" is id {}", slot, name, id);
		}
	}
	botLookPrimary = look.team_color_id;
	botLookAccent = look.custom_color_id;
	// (Nov 4, Coop saw a random topper) a slot the preset leaves empty is set empty, not left to the game's bot outfit
	for (int slot : { 1, 2, 3, 4, 5, 13, 14, 15 }) if (!look.products.count(slot)) look.products[slot] = 0;
	const uintptr_t addr = bot.memory_address;
	// The car's colours don't take on a bot (Nov 4: through the loadout, SetCarColor every tick and BakkesMod's paint
	// tools all left it the default orange), so it's orange. Paints on items do take.
	auto apply = [this, look](PriWrapper p) {
#pragma warning(push)
#pragma warning(disable : 4996)   // SetBotLoadout is marked experimental in the SDK
		gameWrapper->SetBotLoadout(p, look);
#pragma warning(pop)
	};
	apply(pri);
	// and again a second later: the game finishes dressing a new bot (and a team change) after it appears, which can
	// write over a look put on straight away
	gameWrapper->SetTimeout([this, addr, apply](GameWrapper*) {
		CarWrapper b = BotCar();
		if (b.IsNull() || b.memory_address != addr) return;
		PriWrapper p = b.GetPRI();
		if (!p.IsNull()) { apply(p); BOTLOG("Road2 Bot: look applied again"); }
	}, 1.0f);
	BOTLOG("Road2 Bot: look applied ({} items, {})", look.products.size(), fromFile ? "from bot_loadout.txt" : "the built-in look");
}

void RLCoach::BotSpawn()
{
	if (!gameWrapper->IsInFreeplay()) { botStatus = "Bot test: open free play first"; BOTLOG("{}", botStatus); return; }
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) { botStatus = "Bot test: no server (not in free play?)"; BOTLOG("{}", botStatus); return; }
	const bool can = server.CanSpawnBots();
	const int before = server.GetCars().Count();
	server.SetbRandomizedBotLoadouts(0);   // (Nov 4) otherwise the game dresses it as a random bot, ignoring the body
	server.SpawnBot(kOctane, "Road2 Bot");
	botStatus = std::format("Bot test: asked for a bot (CanSpawnBots said {}, cars before: {}). Looking for it...", can ? "yes" : "no", before);
	BOTLOG("{}", botStatus);
	botTicksSeen = botInputsSeen = 0;
	// The car shows up a moment later
	gameWrapper->SetTimeout([this](GameWrapper*) { BotFind(); }, 0.5f);
}

void RLCoach::BotFind()
{
	const uintptr_t prevBot = botCarAddr;
	botCarAddr = 0;
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	const int total = cars.Count();
	std::vector<CarWrapper> others;
	for (int i = 0; i < total; i++)
	{
		CarWrapper c = cars.Get(i);
		if (c.IsNull() || (!local.IsNull() && c.memory_address == local.memory_address)) continue;
		others.push_back(c);
	}
	if (others.empty())
	{
		botStatus = std::format("Bot test: no second car showed up ({} car(s) in the game). Spawning bots may not work in free play.", total);
		BOTLOG("{}", botStatus);
		return;
	}
	// The first is ours; any more (a leftover bot, or one spawned twice) are removed: one bot is all we want (Oct 30).
	// Except the spanish's second bot (Nov 4): while an item wants it, a second car is kept as bot 2
	size_t keep = 1;
	// the car that was already bot 1 stays bot 1 (a respawned bot 2 mustn't take its place)
	for (size_t k = 1; k < others.size(); k++)
		if (prevBot && others[k].memory_address == prevBot) { std::swap(others[0], others[k]); break; }
	CarWrapper c = others[0];
	botCarAddr = c.memory_address;
	BotApplyLook(c);
	{
		AIControllerWrapper ai = c.GetAIController();
		if (!ai.IsNull() && botMode != 0) ai.DoNothing();
	}
	if (wantBot2 && others.size() > 1)
	{
		keep = 2;
		bot2Addr = others[1].memory_address;
		AIControllerWrapper ai2 = others[1].GetAIController();
		if (!ai2.IsNull()) ai2.DoNothing();
		botLookAddr = 0;
		BotApplyLook(others[1]);
		botLookAddr = c.memory_address;
	}
	for (size_t k = keep; k < others.size(); k++)
	{
		AIControllerWrapper ai = others[k].GetAIController();
		if (!ai.IsNull()) ai.DoNothing();
		server.RemoveCar(others[k]);
		if (!ai.IsNull()) server.RemovePlayer(ai);
	}
	PriWrapper pri = c.GetPRI();
	const bool flagged = !pri.IsNull() && pri.GetbBot();
	botStatus = std::format("Bot test: found a second car ({} cars in all; marked as a bot: {}; extras removed: {}). Mode {}.", total, flagged ? "yes" : "no", others.size() - 1, botMode);
	BOTLOG("{}", botStatus);
}

void RLCoach::BotRemove()
{
	// Every car that isn't yours goes (free play only ever has yours otherwise): a bot from an earlier set that never
	// got removed, or one spawned twice, can't hang around and get picked up by the next set (Oct 30)
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	botCarAddr = 0;
	bot2Addr = 0;
	wantBot2 = false;
	botMode = 0;
	botFrames.clear();
	if (server.IsNull() || !gameWrapper->IsInFreeplay()) { botStatus.clear(); return; }
	CarWrapper local = gameWrapper->GetLocalCar();
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	std::vector<CarWrapper> extra;
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper c = cars.Get(i);
		if (c.IsNull() || (!local.IsNull() && c.memory_address == local.memory_address)) continue;
		extra.push_back(c);
	}
	for (CarWrapper& c : extra)
	{
		AIControllerWrapper ai = c.GetAIController();
		if (!ai.IsNull()) ai.DoNothing();
		server.RemoveCar(c);
		if (!ai.IsNull()) server.RemovePlayer(ai);
	}
	// Check they're really gone
	gameWrapper->SetTimeout([this](GameWrapper*) {
		ServerWrapper sv = gameWrapper->GetGameEventAsServer();
		if (sv.IsNull()) return;
		const int n = sv.GetCars().Count();
		botStatus = n <= 1 ? "Bot test: removed" : std::format("Bot test: tried to remove it, but there are still {} cars (reset free play to clear it)", n);
		BOTLOG("{}", botStatus);
	}, 0.5f);
	botStatus = "Bot test: removing...";
	BOTLOG("{}", botStatus);
}

void RLCoach::BotSetMode(int mode)
{
	botMode = std::clamp(mode, 0, 3);
	botTick = 0;
	botFrames.clear();
	botDriven = 0;
	botHoldSet = false;
	// Our modes need the game's own bot AI switched off, or it fights us for the controls
	if (botMode != 0)
	{
		CarWrapper bot = BotCar();
		if (!bot.IsNull()) { AIControllerWrapper ai = bot.GetAIController(); if (!ai.IsNull()) ai.DoNothing(); }
	}
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
			BOTLOG("{}", botStatus);
			botMode = 0;
			return;
		}
		botStatus = std::format("Bot test: mode {} with {} ({:.1f} s)", botMode, newest, botFrames.size() / 120.0f);
	}
	else
		botStatus = std::format("Bot test: mode {} ({})", botMode, botMode == 0 ? "the game's AI drives" : "held still by Road2");
	BOTLOG("{}", botStatus);
}

bool RLCoach::IsBotCar(CarWrapper& car)
{
	return botCarAddr && !car.IsNull() && car.memory_address == botCarAddr;
}

void RLCoach::BotInput(CarWrapper& car, void* params)
{
	if (bot2Addr && params && !car.IsNull() && car.memory_address == bot2Addr) { Bot2Input(car, params); return; }
	if (!IsBotCar(car) || !params) return;
	botInputsSeen++;
	if (botMode == 1 || botMode == 2 || botMode == 4)
	{
		*reinterpret_cast<ControllerInput*>(params) = ControllerInput{};
	}
	else if (botMode == 5) NextoInput(params);   // Nexto drives it (RLCoachNexto.cpp)
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
	(void)n; (void)place;   // driving happens in BotDrive (from your car's tick); this only counts that the bot's own tick arrives
}

void RLCoach::DrawBotStatus(CanvasWrapper& canvas)
{
	if (botStatus.empty() || botMode == 4 || !botTestUsed) return;   // only after the bot test commands, never for the Combine's bot
	canvas.SetColor(255, 255, 255, 230);
	canvas.SetPosition(Vector2{ 24, 160 });
	canvas.DrawString(botStatus, 1.2f, 1.2f);
	if (botCarAddr)
	{
		canvas.SetPosition(Vector2{ 24, 184 });
		canvas.DrawString(std::format("Driven {} ticks (the bot's own updates seen: {} input, {} tick). Playback tick {} of {}.", botDriven, botInputsSeen, botTicksSeen, botTick, botFrames.size()), 1.2f, 1.2f);
	}
}

// The bot's car, found by address among the game's cars (null if it's gone)
CarWrapper RLCoach::BotCar()
{
	if (!botCarAddr) return CarWrapper(0);
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return CarWrapper(0);
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper c = cars.Get(i);
		if (!c.IsNull() && c.memory_address == botCarAddr) return c;
	}
	return CarWrapper(0);
}

// Every tick, from YOUR car's update (which we know fires): drive the bot in our modes. Its own per-tick update
// may not reach the plugin, so this doesn't rely on it.
void RLCoach::BotDrive()
{
	if (!botCarAddr || botMode == 0 || botMode == 4 || botMode == 5) return;   // 4: the Combine places it itself; 5: Nexto (the Combine steps it)
	CarWrapper bot = BotCar();
	if (bot.IsNull()) { botStatus = "Bot test: the bot's car is gone"; botCarAddr = 0; return; }
	botDriven++;
	if (botMode == 1)
	{
		// Held still where it was when you picked mode 1
		if (!botHoldSet) { botHold = bot.GetLocation(); botHoldRot = bot.GetRotation(); botHoldSet = true; }
		bot.SetInput(ControllerInput{});
		bot.SetLocation(botHold);
		bot.SetRotation(botHoldRot);
		bot.SetVelocity(Vector(0, 0, 0));
		bot.SetAngularVelocity(Vector(0, 0, 0), false);
		return;
	}
	if (botFrames.empty()) return;
	const int n = static_cast<int>(botFrames.size());
	const Frame& f = botFrames[std::min(botTick, n - 1)];
	auto place = [&](const Frame& g) {
		bot.SetLocation(Vector(g.carX, g.carY, g.carZ));
		bot.SetRotation(Rotator(g.carPitch, g.carYaw, g.carRoll));
		bot.SetVelocity(Vector(g.carVX, g.carVY, g.carVZ));
		bot.SetAngularVelocity(Vector(g.carAVX, g.carAVY, g.carAVZ), false);
	};
	if (botMode == 2) { bot.SetInput(ControllerInput{}); place(f); }
	else
	{
		if (botTick == 0) place(botFrames[0]);
		ControllerInput in{};
		in.Throttle = f.throttle; in.Steer = f.steer; in.Pitch = f.pitch; in.Yaw = f.yaw; in.Roll = f.roll;
		in.Handbrake = f.handbrake ? 1 : 0;
		in.Jump = f.jump ? 1 : 0;
		in.ActivateBoost = f.boost ? 1 : 0;
		in.HoldingBoost = in.ActivateBoost;
		bot.SetInput(in);
	}
	if (++botTick >= n) botTick = 0;
}
