// The Combine (Oct 28): short scored events, each run as a set built from a seed: saves, recovery, reads, first
// touches, and with a bot: 50s, shadow defense and kickoffs. (Rings, the speed course and boost pathing were tried and
// cut; their code was removed Nov 1 and is in the git history.)
// A set is 10 shots at your net. Every shot was checked in the simulator before you get it: it's on target, and a
// perfect car could reach it in time (Combine.cpp). How much spare time that perfect car has sets the level.
// Scoring per shot: a goal 0, a save 2, a save that clears it out of danger 3.
#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"
#include "UiFonts.h"
#include "Overlay3D.h"
#include "HudDraw.h"
#include "ResultCard.h"
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace
{
	constexpr int kCountdownTicks = 96;     // 0.8 s held in place before each shot
	constexpr int kResultTicks = 72;        // 0.6 s showing how the last shot went
	constexpr int kSettleTicks = 30;        // after your last touch, the ball has to stay out of danger this long
	constexpr int kShotTimeoutTicks = 720;  // 6 s
	constexpr int kResetsPerItem = 2;       // free play resets allowed per shot / throw / route before it's skipped
	constexpr float kU2R = 3.14159265f / 32768.0f;

	// Would the ball, left alone, end up in the blue goal within 2.5 s? (gravity, floor and side-wall bounces)
	bool Threatening(Vector p, Vector v)
	{
		const float dt = 1.0f / 60.0f;
		for (int i = 0; i < 150; i++)
		{
			if (p.Y <= -5120.0f - 92.75f) return std::fabs(p.X) < 893.0f && p.Z < 643.0f;
			if (p.Z < 93.0f && v.Z < 0) v.Z = -0.6f * v.Z;
			if (p.Z > 1950.0f && v.Z > 0) v.Z = -v.Z;
			if (std::fabs(p.X) > 4000.0f && p.X * v.X > 0) v.X = -0.6f * v.X;
			if (p.Y < -5030.0f && !(std::fabs(p.X) < 893.0f && p.Z < 643.0f) && v.Y < 0) v.Y = -0.6f * v.Y;
			p = Vector(p.X + v.X * dt, p.Y + v.Y * dt, p.Z + v.Z * dt);
			v.Z -= 650.0f * dt;
		}
		return false;
	}

	struct FontPush
	{
		explicit FontPush(ImFont* f) { if (f) ImGui::PushFont(f); pushed = f != nullptr; }
		~FontPush() { if (pushed) ImGui::PopFont(); }
		bool pushed = false;
	};

	// Where the ball waits during a recovery throw: in the middle of the ring, so ball cam points you at it
	Vector ParkFor(const Combine::RecoveryThrow& t)
	{
		return Vector(t.target.x, t.target.y, 93.0f);
	}

	const char* EventName(const std::string& code)
	{
		return code == "RC" ? "RECOVERY" : code == "RD" ? "READS" : code == "FT" ? "FIRST TOUCHES" : code == "FF" ? "50s" : code == "SD" ? "SHADOW DEFENSE" : code == "KO" ? "KICKOFFS" : code == "SG" ? "SCORE ON A GOALIE" : "SAVES";
	}
	const char* ItemName(const std::string& code)
	{
		return code == "RC" ? "Throw" : code == "RD" ? "Read" : code == "FT" ? "Pass" : code == "FF" ? "Challenge" : code == "SD" ? "Attack" : code == "KO" ? "Kickoff" : code == "SG" ? "Attack" : "Shot";
	}

	// ---- The in-game text in style A (Nov 4): slanted navy bars, team blue for the event, orange for the call ----
	constexpr HudDraw::Rgba kNavyA{ 11, 15, 26, 225 }, kBlueA{ 46, 139, 255, 255 }, kOrangeA{ 255, 138, 31, 255 },
		kWhiteA{ 255, 255, 255, 255 }, kGoodA{ 61, 220, 132, 255 }, kWarnA{ 255, 176, 32, 255 }, kBadA{ 255, 90, 90, 255 };
	std::string Upper(std::string s) { for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return s; }
	// A slanted bar holding the text, centred at (cx, y top); returns its height
	float SlantText(CanvasWrapper& c, float cx, float y, const std::string& text, float scale, HudDraw::Rgba fill, HudDraw::Rgba ink, float padX)
	{
		const Vector2F ts = HudDraw::TextSize(c, text, scale);
		const float h = ts.Y + 10.0f * scale, w = ts.X + 2.0f * padX + h * 0.2126f;
		HudDraw::SlantFill(c, cx - w * 0.5f, y, w, h, fill);
		HudDraw::Text(c, cx - ts.X * 0.5f, y + (h - ts.Y) * 0.5f, text, scale, ink, false);
		return h;
	}
	// The game's font has no middle dot (it drew as a square), so text with " · " in it is split into its own slanted
	// boxes, side by side with a small gap: the first in `first`, the rest navy
	std::vector<std::string> Parts(const std::string& text)
	{
		std::vector<std::string> out;
		const std::string dot = "\xC2\xB7";
		size_t a = 0;
		while (true)
		{
			const size_t b = text.find(dot, a);
			std::string part = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
			while (!part.empty() && part.front() == ' ') part.erase(part.begin());
			while (!part.empty() && part.back() == ' ') part.pop_back();
			if (!part.empty()) out.push_back(part);
			if (b == std::string::npos) break;
			a = b + dot.size();
		}
		return out;
	}
	void SlantRow(CanvasWrapper& c, float cx, float y, const std::vector<std::string>& parts, float scale, HudDraw::Rgba first,
		HudDraw::Rgba firstInk, float padX)
	{
		if (parts.empty()) return;
		std::vector<Vector2F> ts;
		float h = 0.0f;
		for (const std::string& p : parts) { ts.push_back(HudDraw::TextSize(c, p, scale)); h = std::max(h, ts.back().Y); }
		h += 10.0f * scale;
		const float gap = 4.0f * scale;
		std::vector<float> w;
		float total = 0.0f;
		for (const Vector2F& t : ts) { w.push_back(t.X + 2.0f * padX + h * 0.2126f); total += w.back(); }
		total += gap * (parts.size() - 1);
		float x = cx - total * 0.5f;
		for (size_t k = 0; k < parts.size(); k++)
		{
			HudDraw::SlantFill(c, x, y, w[k], h, k == 0 ? first : kNavyA);
			HudDraw::Text(c, x + (w[k] - ts[k].X) * 0.5f, y + (h - ts[k].Y) * 0.5f, parts[k], scale, k == 0 ? firstInk : kWhiteA, false);
			x += w[k] + gap;
		}
	}
	void HudTopBar(CanvasWrapper& c, const Vector2& sz, const std::string& text)
	{
		// [COMBINE] in team blue, then each part of the event line in its own navy box
		std::vector<std::string> parts{ "COMBINE" };
		for (const std::string& p : Parts(Upper(text))) parts.push_back(p);
		SlantRow(c, sz.X * 0.5f, sz.Y * 0.07f, parts, 1.6f, kBlueA, kWhiteA, 10.0f);
	}
	void HudBig(CanvasWrapper& c, const Vector2& sz, const std::string& text, int points)
	{
		// The countdown number, or how the item went: the first part in its grade colour, the details after it
		const HudDraw::Rgba fill = points < 0 ? kNavyA : points == 0 ? kBadA : points >= 3 ? kGoodA : kWarnA;
		const HudDraw::Rgba ink = points < 0 ? kWhiteA : HudDraw::Rgba{ 11, 15, 26, 255 };
		std::vector<std::string> parts = Parts(Upper(text));
		if (parts.size() == 1) { SlantText(c, sz.X * 0.5f, sz.Y * 0.125f, parts[0], 3.0f, fill, ink, 22.0f); return; }
		SlantRow(c, sz.X * 0.5f, sz.Y * 0.125f, parts, 2.2f, fill, ink, 14.0f);
	}
	void HudCall(CanvasWrapper& c, const Vector2& sz, const std::string& text)
	{
		// The call for this item (what to do), orange: the thing to look at
		SlantText(c, sz.X * 0.5f, sz.Y * 0.205f, Upper(text), 2.0f, kOrangeA, HudDraw::Rgba{ 11, 15, 26, 255 }, 18.0f);
	}

	std::string ReadAllText(const std::filesystem::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}
}

int RLCoach::CombineMedal(int points, int maxPoints)
{
	if (maxPoints <= 0) return 0;
	const float r = static_cast<float>(points) / maxPoints;
	return r >= 0.85f ? 3 : r >= 0.70f ? 2 : r >= 0.50f ? 1 : 0;
}

void RLCoach::CombineLoadBests()
{
	combineBest.clear();
	if (!writer) return;
	std::stringstream ss(ReadAllText(writer->Folder() / "combine" / "bests.csv"));
	std::string line;
	while (std::getline(ss, line))
	{
		const size_t c = line.find(',');
		if (c == std::string::npos) continue;
		try { combineBest[line.substr(0, c)] = std::stoi(line.substr(c + 1)); } catch (...) {}
	}
}

void RLCoach::CombineSaveBests()
{
	if (!writer) return;
	std::error_code ec;
	std::filesystem::create_directories(writer->Folder() / "combine", ec);
	std::ofstream f(writer->Folder() / "combine" / "bests.csv", std::ios::trunc);
	for (const auto& [k, v] : combineBest) f << k << "," << v << "\n";
}

void RLCoach::CombineScore(int& pts, int& maxPts, bool countCurrent) const
{
	pts = 0;
	maxPts = 0;
	const int upTo = countCurrent ? combineIndex : combineIndex - 1;
	for (int i = 0; i < static_cast<int>(combinePoints.size()) && i <= upTo; i++)
	{
		if (combinePoints[i] < 0) continue;
		pts += combinePoints[i];
		maxPts += 3;
	}
}

void RLCoach::CombinePublish()
{
	UiCombine u;
	u.running = combinePhase != CombinePhase::Off;
	u.building = combinePhase == CombinePhase::Building;
	u.done = combinePhase == CombinePhase::Done;
	u.level = combineLevel;
	u.levelName = Combine::SaveLevelName(combineLevel);
	u.code = combineSeed ? Combine::SetCode(combineEvent.empty() ? "SV" : CombineCodeEvent().c_str(), combineLevel, combineSeed) : "";
	u.status = combineStatus;
	u.shot = std::min(combineIndex + 1, CombineCount());
	u.shots = CombineCount();
	u.event = EventName(combineEvent);
	u.item = ItemName(combineEvent);
	u.eventCode = combineEvent.empty() ? "SV" : CombineCodeEvent();
	{
		// every item counts toward the total from the start
		for (int p : combinePoints) if (p >= 0) { u.points += p; u.maxPoints += 3; }
	}
	u.outcomes = combineOutcomes;
	u.pointsEach = combinePoints;
	u.bests = combineBest;
	auto it = combineBest.find(u.code);
	u.best = it == combineBest.end() ? -1 : it->second;
	if (u.done)
	{
		static const char* kMedal[4] = { "", "Bronze", "Silver", "Gold" };
		u.medal = kMedal[CombineMedal(u.points, u.maxPoints)];
	}
	std::lock_guard<std::mutex> lock(uiCombineMutex);
	uiCombine = std::move(u);
}

void RLCoach::CombineOpen()
{
	if (viewerActive && !learnWatching) ViewerExit();
	LearnStopWatch();
	PracticeExit();
	if (learnActive || learnInListOpen) LearnClose();
	CombineLoadBests();
	if (!combineSituationsLoaded && writer)
	{
		combineSituations = Combine::LoadSaveSituations(ReadAllText(writer->Folder() / "combine" / "save_situations.csv"));
		combineSituationsLoaded = true;
		LOG("Combine: {} real save situations loaded", combineSituations.size());
	}
	if (!combinePassesLoaded && writer)
	{
		combinePasses = Combine::LoadSaveSituations(ReadAllText(writer->Folder() / "combine" / "pass_situations.csv"));
		combinePassesLoaded = true;
		LOG("Combine: {} real passes loaded", combinePasses.size());
	}
	frames.clear();
	state = RepState::Idle;
	CombinePublish();
}

void RLCoach::CombineClose()
{
	CombineExit();
	havePrev = false;
	state = *recordingEnabled ? RepState::WaitReset : RepState::Idle;
}

void RLCoach::CombineStart(const std::string& event, int level, uint32_t seed)
{
	if (!gameWrapper->IsInFreeplay()) { combineStatus = "Open free play to play the Combine"; CombinePublish(); return; }
	if (!predictor.Ready())
	{
		combineStatus = predictor.Status().rfind("No arena files", 0) == 0 ? "The Combine needs the arena files first: a one-time setup, on the setup card in the settings (F2, Plugins, Road2)"
			: "The simulator is still loading; try again in a moment";
		CombinePublish();
		return;
	}
	if (combineGenBusy) return;
	if (combineGenThread.joinable()) combineGenThread.join();
	PracticeExit();
	// "FFN" / "KON" / "SDN": the event against Nexto; "SG" is only ever against it
	std::string ev0 = event;
	bool vsNexto = ev0 == "SG";
	if (ev0.size() == 3 && ev0.back() == 'N' && (ev0.rfind("FF", 0) == 0 || ev0.rfind("SD", 0) == 0 || ev0.rfind("KO", 0) == 0)) { ev0.pop_back(); vsNexto = true; }
	if (vsNexto && !NextoReady())
	{
		combineStatus = "Nexto isn't there (" + nextoError + "). It comes in the Road2 download, in data\\Road2\\nexto.";
		CombinePublish();
		return;
	}
	combineNexto = vsNexto;
	combineEvent = ev0;
	combineLevel = std::clamp(level, 1, Combine::kSaveLevels);
	// Nexto's strength by level: how long it takes to react (Legend is Nexto as it really plays, 6 ticks from looking to
	// acting). Measured in RocketSim on 60 of these attacks with Nexto attacking too (Nov 4): it scored on 18% at 6 ticks,
	// 48% at 18, 75% at 30. Picking more loosely by its own odds barely changed anything, so it always plays its best.
	static const int kDelay[Combine::kSaveLevels] = { 26, 20, 14, 10, 6 };
	combineNextoDelay = kDelay[combineLevel - 1];
	combineNextoBeta = 1.0f;
	combineSeed = seed ? seed : std::random_device{}();
	combineShots.clear();
	combineThrows.clear();
	combineReads.clear();
	combineTouches.clear();
	combineChallenges.clear();
	combinePoints.clear();
	// The bot: spawned for 50s and shadow defense (the Combine drives it), gone for everything else
	combineBotWait = 0;
	if (CombineUsesBot())
	{
		botMode = 4;
		if (BotCar().IsNull()) BotFind();   // a car already there (a leftover) is adopted, not doubled
		if (BotCar().IsNull()) BotSpawn();
		else { CarWrapper b = BotCar(); AIControllerWrapper ai = b.GetAIController(); if (!ai.IsNull()) ai.DoNothing(); }
	}
	else if (botMode == 4 && botCarAddr) BotRemove();
	combineOutcomes.clear();
	combineIndex = 0;
	combinePhase = CombinePhase::Building;
	combineStatus = "Building your set (every one is checked in the simulator)...";
	CombinePublish();
	combineGenBusy = true;
	const uint32_t sd = combineSeed;
	const int lv = combineLevel;
	const std::string ev = combineEvent;
	const bool vsN = combineNexto;
	const std::vector<Combine::SaveSituation>* sits = combineSituations.empty() ? nullptr : &combineSituations;
	if (ev == "FT" && combinePasses.empty())
	{
		combinePhase = CombinePhase::Off;
		combineGenBusy = false;
		combineStatus = "No pass data yet (data/Road2/combine/pass_situations.csv)";
		CombinePublish();
		return;
	}
	const std::vector<Combine::SaveSituation>* passes = &combinePasses;
	const std::filesystem::path folder = writer ? writer->Folder() : std::filesystem::path();
	combineGenThread = std::thread([this, sd, lv, ev, sits, passes, folder, vsN]() {
		auto built = std::make_shared<CombineBuilt>();
		try
		{
			if (ev == "RC") built->throws = Combine::MakeRecoverySet(sd, lv, 10);
			else if (ev == "RD") built->reads = Combine::MakeReadSet(sd, lv, 10);
			else if (ev == "FT") built->touches = Combine::MakeTouchSet(sd, lv, 10, *passes);
			else if (ev == "FF" || ev == "SD" || ev == "KO" || ev == "SG")
			{
				if (!combineChallengesLoaded)
				{
					combineChallengesAll = Combine::LoadChallengesCached(folder / "combine" / "challenges.csv");
					combineChallengesLoaded = true;
				}
				if (ev == "SG")
				{
					// Score on a goalie: real takes (takes.csv), 7 on the ground and 3 off the wall, mixed
					if (!combineTakesLoaded)
					{
						combineTakesAll = Combine::LoadChallengesCached(folder / "combine" / "takes.csv");
						combineTakesLoaded = true;
					}
					auto g = Combine::MakeChallengeSet(sd, lv, 7, 'G', combineTakesAll);
					auto w = Combine::MakeChallengeSet(sd + 1u, lv, 3, 'W', combineTakesAll);
					std::mt19937 mix(sd * 747796405u + 1u);
					for (auto& c : w) g.insert(g.begin() + static_cast<long>(mix() % (g.size() + 1)), std::move(c));
					built->challenges = std::move(g);
				}
				else if (ev == "KO" && vsN)
				{
					// Called kickoffs (Nov 6): only kickoffs where the real taker went for the ball (got there within 3 s).
					// When the real taker faked or delayed too, a DELAY or FAKE call left you first to the ball and it read
					// "not what was called". 87% of the kickoffs qualify.
					std::vector<Combine::Challenge> going;
					for (const auto& c : combineChallengesAll) if (c.kind == 'K' && Combine::BotArrival(c) <= 3.0f) going.push_back(c);
					built->challenges = Combine::MakeChallengeSet(sd, lv, 10, 'K', going);
					if (built->challenges.size() < 10) built->challenges = Combine::MakeChallengeSet(sd, lv, 10, 'K', combineChallengesAll);
				}
				else built->challenges = Combine::MakeChallengeSet(sd, lv, 10, ev == "FF" ? 'F' : ev == "KO" ? 'K' : 'D', combineChallengesAll);
			}
			else built->shots = Combine::MakeSaveSet(sd, lv, 10, sits);
		}
		catch (...) { *built = CombineBuilt(); }
		combineGenBusy = false;
		gameWrapper->Execute([this, built](GameWrapper*) { CombineBegin(std::move(*built)); });
	});
}

void RLCoach::CombineBegin(CombineBuilt built)
{
	if (combinePhase != CombinePhase::Building) return;   // stopped while it was building
	if (built.shots.empty() && built.throws.empty() && built.reads.empty() && built.touches.empty() && built.challenges.empty())
	{
		combinePhase = CombinePhase::Off;
		combineStatus = combineEvent == "SG" && combineTakesAll.empty() ? "No takes yet (data/Road2/combine/takes.csv)"
			: CombineUsesBot() && combineChallengesAll.empty() ? "No 50s / shadow data yet (data/Road2/combine/challenges.csv)"
			: "Couldn't build a set from that code; try a new one";
		if (botMode == 4 && botCarAddr) BotRemove();
		CombinePublish();
		return;
	}
	combineShots = std::move(built.shots);
	combineThrows = std::move(built.throws);
	combineReads = std::move(built.reads);
	combineTouches = std::move(built.touches);
	combineChallenges = std::move(built.challenges);
	combinePoints.assign(CombineCount(), 0);
	combineOutcomes.assign(CombineCount(), "");
	combineIndex = 0;
	combinePhase = CombinePhase::Countdown;
	combinePhaseTicks = 0;
	combineResets = 0;
	combineStatus.clear();
	frames.clear();
	state = RepState::Idle;
	PracticeGoalsOff();
	// No unlimited boost in the Combine: you play with the boost you're given (the real defender's, on saves)
	{
		CarWrapper me = gameWrapper->GetLocalCar();
		BoostWrapper bc = me.IsNull() ? BoostWrapper(0) : me.GetBoostComponent();
		if (!bc.IsNull() && combineSavedUnlimited < 0) combineSavedUnlimited = bc.GetUnlimitedBoostRefCount();
	}
	CombinePublish();
}

void RLCoach::CombineExit()
{
	if (combinePhase == CombinePhase::Off) return;
	combinePhase = CombinePhase::Off;
	combineShots.clear();
	combineThrows.clear();
	combineReads.clear();
	combineTouches.clear();
	combineChallenges.clear();
	PracticeGoalsRestore();
	if (botMode == 4 && botCarAddr) BotRemove();
	if (combineSavedUnlimited >= 0)
	{
		CarWrapper me = gameWrapper->GetLocalCar();
		BoostWrapper bc = me.IsNull() ? BoostWrapper(0) : me.GetBoostComponent();
		if (!bc.IsNull()) bc.SetUnlimitedBoostRefCount(combineSavedUnlimited);
		combineSavedUnlimited = -1;
	}
	CombinePublish();
}

void RLCoach::CombinePlace(CarWrapper& car, BallWrapper& ball, bool frozen)
{
	if (combineEvent == "RC")
	{
		// The throw: you in the air, tilted and spinning; the ball parked in a far corner, out of the way
		const Combine::RecoveryThrow& t = combineThrows[combineIndex];
		car.SetLocation(Vector(t.pos.x, t.pos.y, t.pos.z));
		car.SetRotation(Rotator(static_cast<int>(t.pitch / kU2R), static_cast<int>(t.yaw / kU2R), static_cast<int>(t.roll / kU2R)));
		car.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(t.vel.x, t.vel.y, t.vel.z));
		car.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(t.angVel.x, t.angVel.y, t.angVel.z), false);
		BoostWrapper bc = car.GetBoostComponent();
		if (!bc.IsNull()) { bc.SetUnlimitedBoostRefCount(0); bc.SetCurrentBoostAmount(1.0f); }
		ball.SetLocation(ParkFor(t));
		ball.SetVelocity(Vector(0, 0, 0));
		ball.SetAngularVelocity(Vector(0, 0, 0), false);
		return;
	}
	if (CombineUsesBot())
	{
		// You where the real player was; the bot where the other one was; the ball where it was
		const Combine::Challenge& c = combineChallenges[combineIndex];
		float p, y, r;
		Combine::QuatToPYR(c.you.q, p, y, r);
		car.SetLocation(Vector(c.you.pos.x, c.you.pos.y, c.you.pos.z));
		car.SetRotation(Rotator(static_cast<int>(p / kU2R), static_cast<int>(y / kU2R), static_cast<int>(r / kU2R)));
		car.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(c.you.vel.x, c.you.vel.y, c.you.vel.z));
		car.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(c.you.angVel.x, c.you.angVel.y, c.you.angVel.z), false);
		BoostWrapper bc = car.GetBoostComponent();
		if (!bc.IsNull()) { bc.SetUnlimitedBoostRefCount(0); bc.SetCurrentBoostAmount(std::clamp(c.boost, 0.0f, 100.0f) / 100.0f); }
		CombinePlaceBot(0.0f, frozen);
		Combine::BallFrame bf;
		if (Combine::BallAt(c, 0.0f, bf))
		{
			ball.SetLocation(Vector(bf.pos.x, bf.pos.y, bf.pos.z));
			ball.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(bf.vel.x, bf.vel.y, bf.vel.z));
			ball.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(bf.angVel.x, bf.angVel.y, bf.angVel.z), false);
		}
		return;
	}
	if (combineEvent == "FT")
	{
		// The pass: the ball just after your teammate hit it, you where the receiver was
		const Combine::TouchShot& t = combineTouches[combineIndex];
		car.SetLocation(Vector(t.car.pos.x, t.car.pos.y, t.car.pos.z));
		car.SetRotation(Rotator(static_cast<int>(t.car.pitch / kU2R), static_cast<int>(t.car.yaw / kU2R), static_cast<int>(t.car.roll / kU2R)));
		car.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(t.car.vel.x, t.car.vel.y, t.car.vel.z));
		car.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(t.carAngVel.x, t.carAngVel.y, t.carAngVel.z), false);
		BoostWrapper bc = car.GetBoostComponent();
		if (!bc.IsNull()) { bc.SetUnlimitedBoostRefCount(0); bc.SetCurrentBoostAmount(std::clamp(t.boost, 0.0f, 100.0f) / 100.0f); }
		ball.SetLocation(Vector(t.ball.pos.x, t.ball.pos.y, t.ball.pos.z));
		ball.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(t.ball.vel.x, t.ball.vel.y, t.ball.vel.z));
		ball.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(t.ball.angVel.x, t.ball.angVel.y, t.ball.angVel.z), false);
		return;
	}
	if (combineEvent == "RD")
	{
		// The read: the ball on its way to the wall or backboard, you where the setup put you
		const Combine::ReadSetup& r = combineReads[combineIndex];
		car.SetLocation(Vector(r.car.pos.x, r.car.pos.y, 17.02f));
		car.SetRotation(Rotator(0, static_cast<int>(r.car.yaw / kU2R), 0));
		car.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(r.car.vel.x, r.car.vel.y, r.car.vel.z));
		car.SetAngularVelocity(Vector(0, 0, 0), false);
		BoostWrapper bc = car.GetBoostComponent();
		if (!bc.IsNull()) { bc.SetUnlimitedBoostRefCount(0); bc.SetCurrentBoostAmount(std::clamp(r.boost, 0.0f, 100.0f) / 100.0f); }
		ball.SetLocation(Vector(r.ball.pos.x, r.ball.pos.y, r.ball.pos.z));
		ball.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(r.ball.vel.x, r.ball.vel.y, r.ball.vel.z));
		ball.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(r.ball.angVel.x, r.ball.angVel.y, r.ball.angVel.z), false);
		return;
	}
	// You start where the defender was (a real save: their speed, facing, spin and boost; made-up shots: nearly still)
	const Combine::SaveShot& s = combineShots[combineIndex];
	car.SetLocation(Vector(s.car.pos.x, s.car.pos.y, s.car.pos.z));
	car.SetRotation(Rotator(static_cast<int>(s.car.pitch / kU2R), static_cast<int>(s.car.yaw / kU2R), static_cast<int>(s.car.roll / kU2R)));
	car.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(s.car.vel.x, s.car.vel.y, s.car.vel.z));
	car.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(s.carAngVel.x, s.carAngVel.y, s.carAngVel.z), false);
	BoostWrapper bc = car.GetBoostComponent();
	if (!bc.IsNull()) { bc.SetUnlimitedBoostRefCount(0); bc.SetCurrentBoostAmount(std::clamp(s.boost, 0.0f, 100.0f) / 100.0f); }
	ball.SetLocation(Vector(s.ball.pos.x, s.ball.pos.y, s.ball.pos.z));
	ball.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(s.ball.vel.x, s.ball.vel.y, s.ball.vel.z));
	ball.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(s.ball.angVel.x, s.ball.angVel.y, s.ball.angVel.z), false);
}

// The bot at time t on its real path (held still when frozen, or once its path has ended)
void RLCoach::CombinePlaceBot(float t, bool frozen)
{
	if (combineIndex >= static_cast<int>(combineChallenges.size())) return;
	CarWrapper bot = BotCar();
	if (bot.IsNull()) return;
	const Combine::Challenge& c = combineChallenges[combineIndex];
	const Combine::PathFrame f = Combine::BotPoseAt(c, t);
	float p, y, r;
	Combine::QuatToPYR(f.q, p, y, r);
	bot.SetInput(ControllerInput{});
	bot.SetLocation(Vector(f.pos.x, f.pos.y, f.pos.z));
	bot.SetRotation(Rotator(static_cast<int>(p / kU2R), static_cast<int>(y / kU2R), static_cast<int>(r / kU2R)));
	bot.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(f.vel.x, f.vel.y, f.vel.z));
	bot.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(f.angVel.x, f.angVel.y, f.angVel.z), false);
}

// Which kickoff is called for an item (the same for the same set code): mostly speedflips, as in real games, and a
// spanish now and then (a second bot comes on as their last back)
int RLCoach::KickoffCall(int index) const
{
	std::mt19937 rng(combineSeed * 2246822519u + static_cast<uint32_t>(index) * 3266489917u + 7u);
	const float r = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
	return r < 0.45f ? 0 : r < 0.6f ? 1 : r < 0.75f ? 2 : 3;   // speedflip 45%, delay 15%, fake 15%, spanish 25%
}

// The hold before each item: 0.8 s, or a real kickoff's 3 s for called kickoffs (time to see your spawn and the call)
int RLCoach::CombineCountdownTicks() const
{
	return combineEvent == "KO" && combineNexto ? 360 : kCountdownTicks;
}

void RLCoach::CombineEndShot(int points, const std::string& what)
{
	if (combineNexto && botMode == 5) botMode = 4;   // Nexto stops; the Combine holds the bot again
	combinePoints[combineIndex] = points;
	combineOutcomes[combineIndex] = what;
	combinePhase = CombinePhase::Result;
	combinePhaseTicks = 0;
	CombinePublish();
}

void RLCoach::CombineTick(CarWrapper& car)
{
	if (combinePhase == CombinePhase::Off || combinePhase == CombinePhase::Building || combinePhase == CombinePhase::Done) return;
	if (!gameWrapper->IsInFreeplay()) { CombineExit(); return; }
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	BallWrapper ball = server.GetBall();
	if (ball.IsNull() || CombineCount() == 0) return;
	combinePhaseTicks++;

	if (combinePhase == CombinePhase::Countdown)
	{
		if (botMode == 5) botMode = 4;   // a reset mid-item: Nexto waits for the next go
		if (CombineUsesBot() && BotCar().IsNull())
		{
			// The bot takes a moment to show up after it's spawned. Free play's reset button also respawns every car,
			// the bot's too, as a new car (Nov 4: the set used to wait 5 s for the old one and then end): look for it
			// again twice a second, spawn a fresh one if it still isn't there after 2 s, and only give up after 10 s
			combinePhaseTicks = 1;
			++combineBotWait;
			if (combineBotWait % 60 == 0) BotFind();
			if (combineBotWait == 240 && BotCar().IsNull()) BotSpawn();
			if (combineBotWait > 1200) { combineStatus = "Couldn't spawn the bot (open free play and try again)"; CombineExit(); }
			if (BotCar().IsNull()) return;
			botMode = 4;
		}
		combineBotWait = 0;
		// The spanish: their last back, a second bot, comes on for that item only
		if (combineEvent == "KO" && combineNexto)
		{
			if (KickoffCall(combineIndex) == 3)
			{
				wantBot2 = true;
				if (Bot2Car().IsNull())
				{
					combinePhaseTicks = 1;
					++combineBot2Wait;
					if (combineBot2Wait == 1 || combineBot2Wait == 360) Bot2Spawn();
					else if (combineBot2Wait % 60 == 0) BotFind();
					if (combineBot2Wait > 900) { combineBot2Wait = 0; CombineEndShot(-1, "Spanish: the second bot didn't show up (doesn't count)"); }
					return;
				}
				combineBot2Wait = 0;
				// (Nov 6) a new bot goes on whichever team is short, and with one each it's a coin flip. On your team it
				// can't be demoed, so it goes and comes back until it lands on theirs (a bot's team can't be changed)
				{
					CarWrapper b2 = Bot2Car();
					PriWrapper pri2 = b2.GetPRI();
					const int team2 = pri2.IsNull() ? b2.GetTeamNum2() : pri2.GetTeamNum2();
					if (team2 == car.GetTeamNum2())
					{
						if (++combineBot2Retries > 8)
						{
							combineBot2Retries = 0;
							CombineEndShot(-1, "Spanish: the second bot kept landing on your team (doesn't count)");
							return;
						}
						LOG("Spanish: the second bot landed on your team, spawning it again ({})", combineBot2Retries);
						Bot2Remove();
						wantBot2 = true;
						combinePhaseTicks = 1;
						return;
					}
				}
				combineBot2Retries = 0;
				Bot2Place(combinePhaseTicks < CombineCountdownTicks());
			}
			else if (bot2Addr || (combinePhaseTicks % 30 == 2 && server.GetCars().Count() > 2)) Bot2Remove();   // incl. a demoed bot 2 coming back
		}
		// Every boost pad back up for each item (Nov 4): small pads take 4 s to come back, longer than the result and
		// the countdown, so on a diagonal kickoff the pad you drove over last time was still missing
		if (combinePhaseTicks == 2) server.ResetPickups();
		if (combinePhaseTicks < CombineCountdownTicks()) { CombinePlace(car, ball, true); return; }
		server.ResetPickups();
		CombinePlace(car, ball, false);
		combinePhase = CombinePhase::Live;
		combineOnTarget = false;
		if (combineNexto)
		{
			// Nexto takes the other car from here, and the ball is free from the start (it was only on its real path
			// because the real other player was)
			CarWrapper bot = BotCar();
			if (!bot.IsNull())
			{
				BoostWrapper bb = bot.GetBoostComponent();
				if (!bb.IsNull()) { bb.SetUnlimitedBoostRefCount(0); bb.SetCurrentBoostAmount(combineEvent == "KO" ? 0.333f : 1.0f); }
			}
			// Nexto takes over at once only for shadow defense; the others hand over later (NextoHandoff)
			if (NextoHandoff() <= 0.0f) NextoStart();
		}
		combineLiveTicks = 0;
		combineTouched = false;
		combineLastTouch = -1;
		combineFirstTouch = -1;
		combineCtrlTicks = combineCtrlTotal = 0;
		combineBallFree = combineNexto && NextoHandoff() <= 0.0f;
		combineBotLastTouch = -1;
		combineBotTouched = false;
		combineBotFirstTouch = -1;
		combineKoYou = combineKoBot = -1;
		combineKoYouDist = -1.0f;
		combineRecContact = combineRecSettle = -1;
		combineRecGround = 0;
		combineRecBodyHit = false;
		combineRecPrevVel = car.GetVelocity();
		combineCarPrev = car.GetLocation();
		return;
	}
	if (combinePhase == CombinePhase::Result)
	{
		if (CombineUsesBot())
		{
			CarWrapper bot = BotCar();
			if (!bot.IsNull()) { bot.SetInput(ControllerInput{}); bot.SetVelocity(Vector(0, 0, 0)); bot.SetAngularVelocity(Vector(0, 0, 0), false); }
		}
		if (combinePhaseTicks < kResultTicks) return;
		combineIndex++;
		if (combineIndex >= CombineCount())
		{
			// The set is done: score, medal, bests
			combinePhase = CombinePhase::Done;
			if (botMode == 4 && botCarAddr) BotRemove();   // the set's over: no bot left standing on the field (Oct 30)
			PracticeGoalsRestore();
			int pts = 0, maxPts = 0;
			CombineScore(pts, maxPts, true);
			const std::string code = Combine::SetCode(CombineCodeEvent().c_str(), combineLevel, combineSeed);
			const std::string levelKey = std::format("{}{}", CombineCodeEvent(), combineLevel);
			bool better = false;
			for (const std::string& k : { code, levelKey })
			{
				auto it = combineBest.find(k);
				if (it == combineBest.end() || pts > it->second) { combineBest[k] = pts; better = true; }
			}
			if (better) CombineSaveBests();
			CombinePublish();
			return;
		}
		combinePhase = CombinePhase::Countdown;
		combinePhaseTicks = 0;
		combineResets = 0;
		CombinePublish();
		return;
	}

	// ---- Live ----
	combineLiveTicks++;
	{
		// Reset shot in free play mid-item (it teleports you to kickoff and turns unlimited boost back on): back to
		// this item's start instead, twice; the third time it's skipped
		const Vector cl = car.GetLocation();
		const Vector jump = cl - combineCarPrev;
		combineCarPrev = cl;
		if (combineLiveTicks > 2 && jump.magnitude() > 400.0f)
		{
			if (combineResets < kResetsPerItem)
			{
				combineResets++;
				combinePhase = CombinePhase::Countdown;
				combinePhaseTicks = 0;
				CombinePublish();
				return;
			}
			CombineEndShot(0, "Reset a third time: skipped");
			return;
		}
	}
	if (combineEvent == "RC")
	{
		// Recovery: the race back into the play. Land however you like and get to the ring (by your back post) as
		// fast as you can: flip, wavedash, powerslide out of it, anything. Points by time against the throw's par
		// (a good player landing well and driving there); the cuts tighten with the level (Combine::RecoveryPointCuts).
		const Combine::RecoveryThrow& t = combineThrows[combineIndex];
		{
			const Vector bp = ball.GetLocation();
			const Vector park = ParkFor(t);
			if (std::fabs(bp.X - park.X) + std::fabs(bp.Y - park.Y) > 50.0f) { ball.SetLocation(park); ball.SetVelocity(Vector(0, 0, 0)); }
		}
		const Vector cl = car.GetLocation();
		const bool wheels = car.IsOnGround();
		if (combineRecContact < 0 && wheels) combineRecContact = combineLiveTicks;   // for the camera
		const float dx = cl.X - t.target.x, dy = cl.Y - t.target.y;
		const float secs = combineLiveTicks / 120.0f;
		if (dx * dx + dy * dy < Combine::kRecoveryTargetRadius * Combine::kRecoveryTargetRadius && cl.Z < 250.0f)
		{
			const float r = secs / std::max(0.5f, t.par);
			float c3, c2, c1;
			Combine::RecoveryPointCuts(combineLevel, c3, c2, c1);
			const int pts = r <= c3 ? 3 : r <= c2 ? 2 : r <= c1 ? 1 : 0;
			CombineEndShot(pts, std::format("Back in play in {:.2f} s (par {:.2f})", secs, t.par));
			return;
		}
		if (secs > t.par * 2.5f + 1.0f) { CombineEndShot(0, std::format("Too slow (over {:.1f} s, par {:.2f})", secs, t.par)); return; }
		return;
	}
	if (CombineUsesBot())
	{
		const Combine::Challenge& c = combineChallenges[combineIndex];
		const float t = combineLiveTicks / 120.0f;
		// the bot on its real path (or Nexto playing it); the ball on its real path until you're about to touch it
		// (or its path ends)
		if (wantBot2 && combineEvent == "KO") Bot2Drive();
		if (combineNexto && botMode == 5) NextoStep(car, ball);
		else if (combineNexto)
		{
			// The real player's path up to the hand-off (or sooner, if the ball gets to the bot), then Nexto from there
			CarWrapper bot = BotCar();
			const bool ballAtBot = !bot.IsNull() && (bot.GetLocation() - ball.GetLocation()).magnitude() < 500.0f;
			if (t >= NextoHandoff() || t > c.botEnd || ballAtBot)
			{
				if (!bot.IsNull())
				{
					BoostWrapper bb = bot.GetBoostComponent();
					if (!bb.IsNull() && combineEvent == "KO") bb.SetCurrentBoostAmount(0.15f);   // what a speedflip leaves
				}
				combineBallFree = true;
				NextoStart();
				NextoStep(car, ball);
			}
			else CombinePlaceBot(t, false);
		}
		else if (t <= c.botEnd) CombinePlaceBot(t, false);
		if (!combineBallFree)
		{
			Combine::BallFrame bf;
			const Vector cl = car.GetLocation();
			const bool near = (cl - ball.GetLocation()).magnitude() < 92.75f + 150.0f;
			if (combineTouched || near || !Combine::BallAt(c, t, bf)) combineBallFree = true;
			else
			{
				ball.SetLocation(Vector(bf.pos.x, bf.pos.y, bf.pos.z));
				ball.SetVelocity(Vector(bf.vel.x, bf.vel.y, bf.vel.z));
				ball.SetAngularVelocity(Vector(bf.angVel.x, bf.angVel.y, bf.angVel.z), false);
			}
		}
		const Vector p = ball.GetLocation(), v = ball.GetVelocity(), cl = car.GetLocation();
		const bool inBlue = p.Y <= -5120.0f - 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f;
		const bool inOrange = p.Y >= 5120.0f + 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f;
		if (combineEvent == "SG")
		{
			// Score on a goalie: you drive up to a real take, make it yours, and turn it into a goal with Nexto back.
			// Goal 3; a shot of yours heading in that Nexto saved 1; anything else 0. Over as soon as Nexto wins it
			// (its touch sends it back the other way) or 20 s after the take (Nov 6: air dribbles and ceiling shots take time).
			if (inOrange) { CombineEndShot(3, combineTouched ? "Goal" : "Goal (off Nexto)"); return; }
			if (inBlue) { CombineEndShot(0, "Nexto scored on the counter"); return; }
			if (combineTouched && combineLiveTicks - combineLastTouch < 30 && Threatening(Vector(p.X, -p.Y, p.Z), Vector(v.X, -v.Y, v.Z))) combineOnTarget = true;
			const bool nextoHasIt = combineBotLastTouch >= 0 && combineBotLastTouch > combineLastTouch && v.Y < -400.0f;
			if (nextoHasIt && combineLiveTicks - combineBotLastTouch > 36)
			{
				CombineEndShot(combineOnTarget ? 1 : 0, combineOnTarget ? "On target, Nexto saved it" : "Nexto won it");
				return;
			}
			if (t > c.tContact + 20.0f) { CombineEndShot(combineOnTarget ? 1 : 0, combineOnTarget ? "On target, Nexto saved it" : "No goal in time"); return; }
			return;
		}
		if (combineEvent == "KO" && combineNexto)
		{
			// Called kickoffs vs Nexto (scoring from Coop's test, Nov 4). Where the ball is 2 s after the first touch: well into
			// their half is a win (3), well into yours a loss (0). Down the middle, the boost decides: you came out with more
			// 2, even 1, less 0. A goal 3. Not doing what was called caps it at 1.
			// The spanish is only about the demo: a demo 3, a bump 1, neither 0 (where the ball goes doesn't count).
			static const char* kCall[4] = { "Speedflip", "Delay", "Fake", "Spanish" };
			const int call = KickoffCall(combineIndex);
			CarWrapper bot = BotCar();
			const Vector bl = ball.GetLocation();
			if (combineBotTouched && combineKoYouDist < 0.0f) combineKoYouDist = (cl - bl).magnitude();
			const bool youFirst = combineTouched && (!combineBotTouched || combineFirstTouch <= combineBotFirstTouch);
			if (call == 3)
			{
				if (!((bot2Demoed && combineLiveTicks - bot2DemoTick > 90) || t > 5.0f)) return;
				int pts = bot2Demoed ? 3 : bot2Bumped ? 1 : 0;
				if (youFirst) pts = std::min(pts, 1);
				CombineEndShot(pts, std::format("Spanish{} · {}", youFirst ? " (you went for the ball)" : "",
					bot2Demoed ? "demo" : bot2Bumped ? "a bump, not a demo" : "no demo"));
				return;
			}
			if (inOrange) { CombineEndShot(3, std::string(kCall[call]) + ": goal off the kickoff"); return; }
			if (inBlue) { CombineEndShot(0, std::string(kCall[call]) + ": Nexto scored off it"); return; }
			int first = -1;
			if (combineTouched) first = combineFirstTouch;
			if (combineBotTouched && (first < 0 || combineBotFirstTouch < first)) first = combineBotFirstTouch;
			if (first < 0) { if (t > 4.0f) CombineEndShot(0, std::string(kCall[call]) + ": nobody got to it"); return; }
			if (combineLiveTicks - first < 240) return;   // 2 s after the first touch
			// Did you do the call? Judged on what you did, not on who won (Nov 4: a speedflip that lost the race to the
			// ball read "not what was called"). Speedflip: you went for it (you hit it first, or met it within 0.3 s of
			// Nexto, or were on it when Nexto hit it). Delay: Nexto hit it first, you were coming in behind (within 2000)
			// and touched it within 1.5 s. Fake: Nexto hit it first and you held back (1400+ away).
			const bool youMet = youFirst || (combineTouched && combineFirstTouch - combineBotFirstTouch <= 36) || (combineKoYouDist >= 0.0f && combineKoYouDist < 350.0f);
			// (Nov 6 audit) a delay no longer needs your touch afterwards (the ball can go anywhere off their hit), and
			// delay and fake overlap a little (1300-1800 away counts as either), so a call you did isn't missed by a few uu
			const bool followed = call == 0 ? youMet
				: call == 1 ? (!youFirst && combineKoYouDist >= 250.0f && combineKoYouDist < 1800.0f)
				: (!youFirst && combineKoYouDist >= 1300.0f);
			BoostWrapper mb = car.GetBoostComponent();
			BoostWrapper bb = bot.IsNull() ? BoostWrapper(0) : bot.GetBoostComponent();
			const float myBoost = mb.IsNull() ? 0.0f : mb.GetCurrentBoostAmount(), botBoost = bb.IsNull() ? 0.0f : bb.GetCurrentBoostAmount();
			int pts;
			std::string how;
			if (p.Y > 1500.0f) { pts = 3; how = "ball on their side"; }
			else if (p.Y < -1500.0f) { pts = 0; how = "ball on your side"; }
			else if (myBoost >= botBoost + 0.15f) { pts = 2; how = "down the middle, you won the boost"; }
			else if (myBoost > botBoost - 0.15f) { pts = 1; how = "down the middle, even boost"; }
			else { pts = 0; how = "down the middle, they won the boost"; }
			if (!followed) pts = std::min(pts, 1);
			CombineEndShot(pts, std::format("{}{} · {}", kCall[call], followed ? "" : " (not what was called)", how));
			return;
		}
		if (combineEvent == "KO")
		{
			// Kickoffs: where the ball is 1.5 s after the hit (their half 3, the middle 1, yours 0), with each of your times
			// to the ball
			const Vector bl = ball.GetLocation();
			if (combineKoYou < 0 && (cl - bl).magnitude() < 200.0f) combineKoYou = combineLiveTicks;
			{
				CarWrapper bot = BotCar();
				if (combineKoBot < 0 && !bot.IsNull() && (bot.GetLocation() - bl).magnitude() < 200.0f) combineKoBot = combineLiveTicks;
			}
			auto times = [this]() {
				const std::string you = combineKoYou < 0 ? "never" : std::format("{:.2f} s", combineKoYou / 120.0f);
				const std::string them = combineKoBot < 0 ? "never" : std::format("{:.2f} s", combineKoBot / 120.0f);
				return std::format("you {}, them {}", you, them);
			};
			if (inOrange) { CombineEndShot(3, "Won it (and scored) · " + times()); return; }
			if (inBlue) { CombineEndShot(0, "Lost it (they scored) · " + times()); return; }
			int first = -1;
			if (combineTouched) first = combineFirstTouch;
			if (combineBotTouched && (first < 0 || combineBotFirstTouch < first)) first = combineBotFirstTouch;
			if (first < 0)
			{
				if (t > c.tContact + 2.0f) CombineEndShot(0, "Never got to it");
				return;
			}
			if (combineLiveTicks - first < 180) return;   // 1.5 s after the hit
			const int pts = p.Y > 400.0f ? 3 : p.Y < -400.0f ? 0 : 1;
			CombineEndShot(pts, std::format("{} · {}", pts == 3 ? "Won it" : pts == 0 ? "Lost it" : "Neutral", times()));
			return;
		}
		if (combineEvent == "FF")
		{
			// 50s: where the ball goes after the challenge. Your way 3, even 1, their way 0.
			if (inOrange) { CombineEndShot(3, "Won it (and scored)"); return; }
			if (inBlue) { CombineEndShot(0, "Lost it (they scored)"); return; }
			int first = -1;
			if (combineTouched) first = combineFirstTouch;
			if (combineBotTouched && (first < 0 || combineBotFirstTouch < first)) first = combineBotFirstTouch;
			if (first < 0)
			{
				if (t > c.tContact + (combineNexto ? 2.5f : 1.5f)) CombineEndShot(0, "Never got to it");
				return;
			}
			if (combineLiveTicks - first < 96) return;   // 0.8 s after the challenge
			if (!combineTouched) { CombineEndShot(0, "Didn't get a touch on it"); return; }
			const bool won = v.Y > 300.0f || (p.Y > cl.Y + 200.0f && v.Y > -200.0f);
			const bool lost = v.Y < -300.0f && p.Y < cl.Y;
			CombineEndShot(won ? 3 : lost ? 0 : 1, won ? "Won it" : lost ? "Lost it" : "Even");
			return;
		}
		// Shadow defense: stop them and win it back 3, stop them 2, they don't score 1, scored on 0
		if (inBlue) { CombineEndShot(0, "Scored on"); return; }
		if (inOrange) { CombineEndShot(3, "Won it back (and scored)"); return; }
		const bool danger = Threatening(p, v);
		if (combineTouched && !danger && combineLiveTicks - combineLastTouch >= kSettleTicks)
		{
			const bool cleared = p.Y > -2600.0f || (v.Y > 800.0f && p.Y > -4300.0f);
			CombineEndShot(cleared ? 3 : 2, cleared ? "Won it back" : "Stopped it");
			return;
		}
		// (against Nexto there's no real path to end, so it's a fixed 6 s and 8 s)
		if (!combineTouched && combineBallFree && t > (combineNexto ? 6.0f : c.ballEnd + 2.0f) && !danger) { CombineEndShot(1, "They didn't score"); return; }
		if (t > (combineNexto ? 8.0f : c.botEnd + 4.0f)) { CombineEndShot(combineTouched ? 2 : 1, combineTouched ? "Stopped it" : "They didn't score"); return; }
		return;
	}
	if (combineEvent == "FT")
	{
		// First touches: score off it (3), or take it under control (2: for the second after your first touch the ball
		// stays within 450 of you at a similar speed, at least 3/4 of the time), or at least get a touch on it (1)
		const Combine::TouchShot& t = combineTouches[combineIndex];
		const Vector p = ball.GetLocation(), v = ball.GetVelocity();
		const float secs = combineLiveTicks / 120.0f;
		const bool inBlue = p.Y <= -5120.0f - 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f;
		const bool inOrange = p.Y >= 5120.0f + 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f;
		if (inOrange) { CombineEndShot(combineTouched ? 3 : -1, combineTouched ? "Goal" : "Went in by itself (doesn't count)"); return; }
		if (inBlue) { CombineEndShot(0, "Own goal"); return; }
		if (!combineTouched)
		{
			if (secs > t.recvDt + 1.5f) CombineEndShot(0, "Missed the pass");
			return;
		}
		const float since = (combineLiveTicks - combineFirstTouch) / 120.0f;
		const Vector cl = car.GetLocation(), cv = car.GetVelocity();
		if (since <= 1.0f)
		{
			combineCtrlTotal++;
			if ((p - cl).magnitude() < 450.0f && (v - cv).magnitude() < 900.0f) combineCtrlTicks++;
		}
		const bool controlled = combineCtrlTotal > 0 && combineCtrlTicks * 4 >= combineCtrlTotal * 3;
		// heading for their goal (the blue-goal check, mirrored)
		const bool onTarget = Threatening(Vector(p.X, -p.Y, p.Z), Vector(v.X, -v.Y, v.Z));
		const bool gone = since > 1.2f && (p - cl).magnitude() > 2000.0f && !onTarget;
		if (since > 4.0f || gone)
		{
			CombineEndShot(controlled ? 2 : 1, controlled ? "Controlled it" : "Touched it, lost it");
			return;
		}
		return;
	}
	if (combineEvent == "RD")
	{
		// Reads: a touch after it comes off the wall / backboard and before it lands is the read. Your backboard: then
		// clear it (3), or at least keep it out (2). Their side: then score (3), or at least get there (1). A touch
		// before it reaches the wall or after it lands still counts for less.
		const Combine::ReadSetup& r = combineReads[combineIndex];
		const Vector p = ball.GetLocation(), v = ball.GetVelocity();
		const float secs = combineLiveTicks / 120.0f;
		const float ft = combineFirstTouch / 120.0f;
		const bool early = combineTouched && ft < r.tWall - 0.1f;
		const bool late = combineTouched && ft > r.tFloor + 0.08f;
		const bool read = combineTouched && !early && !late;
		const bool high = Combine::ReadMinHeight(combineLevel) > 1.0f;
		const char* how = early ? "beat it to the wall" : late ? (high ? "too low" : "after it landed") : "read it";
		const bool inBlue = p.Y <= -5120.0f - 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f;
		const bool inOrange = p.Y >= 5120.0f + 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f;
		if (r.kind == 0)
		{
			if (inBlue) { CombineEndShot(0, "Goal"); return; }
			if (inOrange) { CombineEndShot(3, "Cleared it into their net!"); return; }
			const bool danger = Threatening(p, v);
			if (combineTouched && !danger && combineLiveTicks - combineLastTouch >= kSettleTicks)
			{
				const bool cleared = p.Y > -2600.0f || (v.Y > 800.0f && p.Y > -4300.0f);
				const int pts = read ? (cleared ? 3 : 2) : 1;
				CombineEndShot(pts, std::format("{}{}", read ? "Read it" : early ? "Beat it to the wall" : high ? "Got it too low" : "Got it after it landed", cleared ? ", cleared" : ", kept it out"));
				return;
			}
			if (!combineTouched && !danger && secs > r.tFloor + 1.5f) { CombineEndShot(0, "Never got to it"); return; }
			if (combineLiveTicks >= kShotTimeoutTicks) { CombineEndShot(combineTouched ? (read ? 2 : 1) : 0, combineTouched ? std::format("Kept it out ({})", how) : "Never got to it"); return; }
			return;
		}
		if (inOrange)
		{
			if (!combineTouched) { CombineEndShot(-1, "Went in by itself (doesn't count)"); return; }
			CombineEndShot(read ? 3 : 2, std::format("Goal ({})", how));
			return;
		}
		if (inBlue) { CombineEndShot(0, "Own goal"); return; }
		if (combineTouched && combineLiveTicks - combineLastTouch >= 300) { CombineEndShot(read ? 1 : 0, std::format("No goal ({})", how)); return; }
		if (!combineTouched && secs > r.tFloor + 1.0f) { CombineEndShot(0, "Didn't get to it before it landed"); return; }
		if (combineLiveTicks >= 960) { CombineEndShot(combineTouched ? (read ? 1 : 0) : 0, combineTouched ? std::format("No goal ({})", how) : "Never got to it"); return; }
		return;
	}
	const Vector p = ball.GetLocation(), v = ball.GetVelocity();
	if (p.Y <= -5120.0f - 92.75f && std::fabs(p.X) < 893.0f && p.Z < 643.0f) { CombineEndShot(0, "Goal"); return; }
	const bool danger = Threatening(p, v);
	if (combineTouched && !danger && combineLiveTicks - combineLastTouch >= kSettleTicks)
	{
		const bool cleared = p.Y > -2600.0f || (v.Y > 800.0f && p.Y > -4300.0f);
		CombineEndShot(cleared ? 3 : 2, cleared ? "Saved and cleared" : "Saved");
		return;
	}
	if (!combineTouched && !danger && combineLiveTicks > 60) { CombineEndShot(-1, "Missed by itself (doesn't count)"); return; }
	if (combineLiveTicks >= kShotTimeoutTicks) { CombineEndShot(combineTouched ? 2 : -1, combineTouched ? "Saved" : "Missed by itself (doesn't count)"); return; }
}

void RLCoach::DrawCombineHud(CanvasWrapper& canvas)
{
	if (combinePhase == CombinePhase::Off) return;
	const Vector2 sz = canvas.GetSize();
	if (combineEvent == "RC" && combineIndex < static_cast<int>(combineThrows.size()) &&
		(combinePhase == CombinePhase::Countdown || combinePhase == CombinePhase::Live))
	{
		const Combine::RecoveryThrow& t = combineThrows[combineIndex];
		CameraWrapper cam = gameWrapper->GetCamera();
		CarWrapper me = gameWrapper->GetLocalCar();
		// The momentum camera (try-out, road2_combine_momentum_cam): until you land, look the way you're travelling,
		// not the way the car faces
		if (*combineMomentumCam && !cam.IsNull() && !me.IsNull() &&
			(combinePhase == CombinePhase::Countdown || combineRecContact < 0 || combineLiveTicks - combineRecContact < 18))
		{
			const Vector v = combinePhase == CombinePhase::Countdown ? Vector(t.vel.x, t.vel.y, t.vel.z) : me.GetVelocity();
			const float sp = v.magnitude();
			if (sp > 300.0f)
			{
				const Vector dir = v * (1.0f / sp);
				const Vector carLoc = me.GetLocation();
				const Vector camLoc = carLoc - dir * 330.0f + Vector(0, 0, 110.0f);
				const Vector look = carLoc + dir * 250.0f - camLoc;
				const float hz = std::sqrt(look.X * look.X + look.Y * look.Y);
				cam.SetLocation(camLoc);
				cam.SetRotation(Rotator(static_cast<int>(std::atan2(look.Z, hz) / kU2R), static_cast<int>(std::atan2(look.Y, look.X) / kU2R), 0));
			}
		}
		// The ring to get to
		if (!cam.IsNull())
		{
			const Vector camLoc = cam.GetLocation();
			const Vector camFwd = RotatorToVector(cam.GetRotation());
			Vector2F prev{};
			bool prevOk = false;
			for (int i = 0; i <= 40; i++)
			{
				const float a = i * 6.2831853f / 40.0f;
				const Vector p(t.target.x + Combine::kRecoveryTargetRadius * std::cos(a), t.target.y + Combine::kRecoveryTargetRadius * std::sin(a), 20.0f);
				Vector2F s;
				const bool ok = Overlay3D::ToScreen(canvas, camLoc, camFwd, p, s);
				if (ok && prevOk) Overlay3D::OutlinedLine(canvas, prev, s, 6.0f, 56, 189, 248, 255);
				prev = s;
				prevOk = ok;
			}
		}
	}
	if (CombineUsesBot() && combinePhase == CombinePhase::Countdown)
	{
		std::string line = BotCar().IsNull() ? "Waiting for the bot..." : combineEvent == "FF" ? "50/50: win the challenge"
			: combineEvent == "SG" ? (combineIndex < static_cast<int>(combineChallenges.size()) && combineChallenges[combineIndex].kind == 'W'
				? "Wall take: make it yours, score on Nexto" : "Ground take: make it yours, score on Nexto") : "Shadow: stay goal-side, win it back";
		if (combineEvent == "KO" && !BotCar().IsNull() && combineIndex < static_cast<int>(combineChallenges.size()))
		{
			const float ax = std::fabs(combineChallenges[combineIndex].you.pos.x);
			line = std::format("Kickoff: {}", ax > 1500.0f ? "diagonal" : ax > 100.0f ? "off-centre" : "back centre");
		}
		if (combineNexto && combineEvent == "KO" && !BotCar().IsNull())
		{
			// The call, the way a teammate gives it, on the last beat of the 3-2-1 window
			static const char* kCall[4] = { "SPEEDFLIP!", "DELAY!", "FAKE!", "SPANISH!" };
			HudCall(canvas, sz, std::format("{}  (vs Nexto)", line));
			if (combinePhaseTicks > CombineCountdownTicks() / 3) SlantText(canvas, sz.X * 0.5f, sz.Y * 0.27f, kCall[KickoffCall(combineIndex)], 3.2f, kBlueA, kWhiteA, 26.0f);
		}
		else
		{
			if (combineNexto && combineEvent != "SG" && !BotCar().IsNull()) line += "  (vs Nexto)";
			HudCall(canvas, sz, line);
		}
	}
	if (combineEvent == "RD" && combineIndex < static_cast<int>(combineReads.size()) && combinePhase == CombinePhase::Countdown)
	{
		static const char* kWhat[3] = { "Off your backboard: read it, clear it", "Off the side wall: read it, score it", "Off their backboard: read it, score it" };
		static const char* kHigh[Combine::kSaveLevels] = { "", " (before it drops low)", " (in the air)", " (in the air)", " (above crossbar height)" };
		const std::string line = std::string(kWhat[std::clamp(combineReads[combineIndex].kind, 0, 2)]) + kHigh[std::clamp(combineLevel, 1, Combine::kSaveLevels) - 1];
		HudCall(canvas, sz, line);
	}
	int pts = 0, maxPts = 0;
	CombineScore(pts, maxPts, combinePhase == CombinePhase::Result || combinePhase == CombinePhase::Done);
	std::string top;
	if (combinePhase == CombinePhase::Building) top = "COMBINE  ·  building your set...";
	else if (combinePhase == CombinePhase::Done) top = std::format("{}  ·  {}  ·  done: {} of {} points", EventName(combineEvent), Combine::SaveLevelName(combineLevel), pts, maxPts);
	else top = std::format("{}{}  ·  {}  ·  {} {} of {}  ·  {} points", EventName(combineEvent), combineNexto && combineEvent != "SG" ? " vs NEXTO" : "", Combine::SaveLevelName(combineLevel),
		combineEvent == "RC" ? "throw" : combineEvent == "RD" ? "read" : combineEvent == "FT" ? "pass" : combineEvent == "FF" ? "50" : combineEvent == "SD" || combineEvent == "SG" ? "attack" : combineEvent == "KO" ? "kickoff" : "shot", combineIndex + 1, CombineCount(), pts);
	if (combinePhase == CombinePhase::Building) top = "building your set...";
	HudTopBar(canvas, sz, top);
	std::string mid;
	if (combinePhase == CombinePhase::Countdown)
	{
		const int cd = CombineCountdownTicks(), step = cd / 3;
		const int n = std::max(0, cd - combinePhaseTicks) / step + 1;
		mid = combineResets > 0 ? std::format("{}   (retry, {} reset{} left)", std::min(n, 3), kResetsPerItem - combineResets, kResetsPerItem - combineResets == 1 ? "" : "s")
			: std::format("{}", std::min(n, 3));
	}
	else if (combinePhase == CombinePhase::Result && combineIndex < static_cast<int>(combineOutcomes.size())) mid = combineOutcomes[combineIndex];
	if (!mid.empty())
	{
		const int pt = combinePhase == CombinePhase::Result ? combinePoints[combineIndex] : -1;
		HudBig(canvas, sz, mid, pt);
	}
}

void RLCoach::SaveResultCard(const UiCombine& u)
{
	if (!writer) return;
	ResultCard::Data d;
	d.event = u.event + (u.eventCode.size() == 3 && u.eventCode.back() == 'N' ? " vs Nexto" : "");
	d.level = u.levelName;
	d.medal = u.medal;
	d.code = u.code;
	d.points = u.points;
	d.maxPoints = u.maxPoints;
	d.each = u.pointsEach;
	const std::filesystem::path folder = writer->Folder() / "cards", file = folder / ("Road2_" + u.code + ".png");
	std::string error;
	if (ResultCard::Save(file, writer->Folder() / "fonts", d, error))
	{
		uiCardStatus = "Saved " + file.filename().string() + " (the cards folder is open: drag it into Discord).";
		OpenDataSubfolder("cards");
	}
	else uiCardStatus = "Couldn't save the card: " + error;
}

// ---- The Combine page in the window ----
void RLCoach::RenderCombine()
{
	using namespace UiStyle;
	const float S = Sc();   // the window's size right now (UiStyle.h)
	ImFont* fLabel = uiFonts[UiFonts::Label];
	ImFont* fHead = uiFonts[UiFonts::Head];
	ImFont* fBig = uiFonts[UiFonts::Big];
	ImFont* fDisplay = uiFonts[UiFonts::Display];
	auto onGame = [this](std::function<void()> fn) { gameWrapper->Execute([fn](GameWrapper*) { fn(); }); };
	UiCombine u;
	{
		std::lock_guard<std::mutex> lock(uiCombineMutex);
		u = uiCombine;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float W = ImGui::GetContentRegionAvail().x;

	Wordmark(fDisplay, "The Combine");
	ImGui::SameLine(std::max(200.0f * S, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Back to reps").x - 16.0f * S));
	if (LinkButton("Back to reps"))
	{
		uiCombineMode = false;
		onGame([this]() { CombineClose(); });
	}
	ImGui::Spacing();

	if (u.running)
	{
		// ---- A set in progress (style A, Nov 3): the level and code, the event big, the points, a pip per item ----
		const ImVec2 top = ImGui::GetCursorScreenPos();
		if (!u.levelName.empty()) { Pill(u.levelName, kOrange); ImGui::SameLine(0, 6.0f * S); }
		if (!u.code.empty())
		{
			Pill("Set " + u.code, kChip);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Share this code: it gives anyone the exact same set. Click to copy it.");
			if (ImGui::IsItemClicked()) ImGui::SetClipboardText(u.code.c_str());
		}
		{
			FontPush f(fBig ? fBig : fHead);
			ImGui::TextColored(kText, "%s", Caps(u.event.c_str()).c_str());
		}
		// The points, big and blue, top right
		{
			const std::string pts = std::to_string(u.points);
			float pw, lw;
			{ FontPush f(fBig ? fBig : fHead); pw = ImGui::CalcTextSize(pts.c_str()).x; }
			{ CapsFont cf; lw = ImGui::CalcTextSize("POINTS").x; }
			const float rx = top.x + W;
			{ CapsFont cf; dl->AddText(ImVec2(rx - lw, top.y), U32(kMuted), "POINTS"); }
			{ FontPush f(fBig ? fBig : fHead); dl->AddText(ImVec2(rx - pw - 4.0f * S, top.y + ImGui::GetTextLineHeight() * 0.1f + 14.0f * S), U32(kAccent), pts.c_str()); }
		}
		if (u.building) ImGui::TextColored(kSub, "%s", u.status.c_str());
		else
		{
			// Progress: THROW 4 / 10, then a slanted pip per item (green 3+, amber some, red none, white the one now)
			ImGui::Spacing();
			{
				CapsFont cf;
				ImGui::TextColored(kMuted, "%s", Caps(u.item.c_str()).c_str());
				const std::string n = u.done ? std::format("{} / {}", u.points, u.maxPoints) : std::format("{} / {}", u.shot, u.shots);
				ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(n.c_str()).x - 4.0f * S);
				ImGui::TextColored(kText, "%s", n.c_str());
			}
			{
				const int n = std::max(1, u.shots);
				const ImVec2 q = ImGui::GetCursorScreenPos();
				const float g = 5.0f * S, ph = 12.0f * S, pw = (W - g * (n - 1)) / n;
				for (int i = 0; i < n; i++)
				{
					const ImVec2 a(q.x + i * (pw + g), q.y), b(a.x + pw, a.y + ph);
					const bool scored = i < static_cast<int>(u.outcomes.size()) && !u.outcomes[i].empty();
					const int pt = i < static_cast<int>(u.pointsEach.size()) ? u.pointsEach[i] : 0;
					ImVec4 c = kTrack;
					if (scored) c = pt < 0 ? kMuted : pt == 0 ? kBad : pt >= 3 ? kGood : kWarn;
					else if (!u.done && i == u.shot - 1) c = kText;
					SlantQuad(dl, a, b, U32(c));
					if (!scored && !u.done && i == u.shot - 1) SlantQuad(dl, ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), U32(kAccent), false, 2.0f);
				}
				ImGui::Dummy(ImVec2(W, ph + 4.0f * S));
			}
			if (u.done && !u.medal.empty()) { ImGui::Spacing(); Pill(u.medal, kGold); }
			if (u.best >= 0) ImGui::TextColored(kMuted, "Your best on this set: %d", u.best);

			// This set so far: a row per item, its points on a slanted chip
			Heading("THIS SET");
			for (size_t i = 0; i < u.outcomes.size(); i++)
			{
				if (u.outcomes[i].empty()) continue;
				const int pt = i < u.pointsEach.size() ? u.pointsEach[i] : 0;
				const ImVec2 a = ImGui::GetCursorScreenPos();
				const float rh = ImGui::GetTextLineHeight() + 12.0f * S;
				dl->AddRectFilled(a, ImVec2(a.x + W, a.y + rh), U32(kPanel2));
				{
					FontPush f(fHead);
					dl->AddText(ImVec2(a.x + 10.0f * S, a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), U32(kMuted), std::to_string(i + 1).c_str());
				}
				dl->AddText(ImVec2(a.x + 36.0f * S, a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), U32(kText), u.outcomes[i].c_str());
				if (pt >= 0)
				{
					const std::string chip = std::format("+{}", pt);
					CapsFont cf;
					const float cw = ImGui::CalcTextSize(chip.c_str()).x + 14.0f * S + rh * kSlant;
					const float ch = ImGui::GetTextLineHeight() + 3.0f * S;
					ImGui::SetCursorScreenPos(ImVec2(a.x + W - cw - 10.0f * S, a.y + (rh - ch) * 0.5f));
					Pill(chip, pt == 0 ? kBad : pt >= 3 ? kGood : kWarn);
				}
				ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + 4.0f * S));
			}
		}
		ImGui::Spacing();
		const int lv = u.level;
		const std::string code = u.code;
		const float bw = (W - 20.0f * S) / 3.0f;
		if (QuietButton(u.done ? "Play this set again" : "Restart set", ImVec2(bw, 36.0f * S)))
		{
			std::string ev; int l = lv; uint32_t sd = 0;
			if (Combine::ParseSetCode(code, ev, l, sd)) onGame([this, ev, l, sd]() { CombineStart(ev, l, sd); });
		}
		const std::string ev = u.eventCode;
		ImGui::SameLine(0, 10.0f * S);
		if (QuietButton("New set", ImVec2(bw, 36.0f * S))) onGame([this, ev, lv]() { CombineStart(ev, lv, 0); });
		ImGui::SameLine(0, 10.0f * S);
		if (OutlineButton("Stop", ImVec2(bw, 36.0f * S), kBad)) onGame([this]() { CombineExit(); });
		// (Nov 10) the finished set as a picture to post: score, medal, each item, and the code to play the same set
		if (u.done)
		{
			ImGui::Spacing();
			if (AccentButton("Save result card", ImVec2(bw, 36.0f * S))) SaveResultCard(u);
			ImGui::SameLine(0, 10.0f * S);
			if (QuietButton("Copy the code", ImVec2(bw, 36.0f * S))) ImGui::SetClipboardText(u.code.c_str());
			if (!uiCardStatus.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(kMuted, "%s", uiCardStatus.c_str()); ImGui::PopTextWrapPos(); }
		}
		else uiCardStatus.clear();
		return;
	}

	// ---- Your Road2 rating (Nov 10): per skill, the highest level you've cleared, named by the rank that level was
	// measured from (saves and the bot events come from ranked and pro replays by rank; recovery, reads and first
	// touches are level bands, so they show the level's name). Cleared = your best set at that level is a silver or
	// better (21 of 30); score on a goalie = 12 of 30, four goals on Nexto. ----
	{
		struct Skill { const char* name; const char* codes[2]; bool byRank; int need; };
		static const Skill kSkills[] = {
			{ "SAVES", { "SV", nullptr }, true, 21 }, { "KICKOFFS", { "KO", "KON" }, true, 21 }, { "50s", { "FF", "FFN" }, true, 21 },
			{ "SHADOW", { "SD", "SDN" }, true, 21 }, { "SCORING", { "SG", nullptr }, true, 12 }, { "READS", { "RD", nullptr }, false, 21 },
			{ "FIRST TOUCH", { "FT", nullptr }, false, 21 }, { "RECOVERY", { "RC", nullptr }, false, 21 },
		};
		static const char* kRank[Combine::kSaveLevels + 1] = { "UNRATED", "GOLD-PLAT", "DIAMOND", "CHAMPION", "GRAND CHAMP", "SSL" };
		static const ImVec4 kRankCol[Combine::kSaveLevels + 1] = { kMuted, Hex(0xFFB020), Hex(0x7DB8FF), Hex(0xB98CFF), Hex(0xFF5A5A), Hex(0x2DE2E6) };
		{
			FontPush f(fBig ? fBig : fHead);
			ImGui::TextColored(kText, "YOUR ROAD2 RATING");
		}
		const float tileW = 124.0f * S, x0 = ImGui::GetCursorPosX();
		int rated = 0, sum = 0, col = 0;
		const Skill* weakest = nullptr;      // for the plan below: the lowest-rated skill (the first of them, on a tie)
		int weakestLevel = 99;
		const int perRow = std::max(1, static_cast<int>((W + 8.0f * S) / (tileW + 8.0f * S)));
		for (const Skill& sk : kSkills)
		{
			int level = 0;
			for (int l = 1; l <= Combine::kSaveLevels; l++)
				for (const char* code : sk.codes)
				{
					if (!code) continue;
					auto it = u.bests.find(std::format("{}{}", code, l));
					if (it != u.bests.end() && it->second >= sk.need) level = l;
				}
			if (sk.byRank && level > 0) { rated++; sum += level; }
			if (level < weakestLevel) { weakestLevel = level; weakest = &sk; }
			if (col > 0) ImGui::SameLine(x0 + col * (tileW + 8.0f * S));
			ImGui::BeginGroup();
			{
				FontPush f(fLabel);
				ImGui::TextColored(kMuted, "%s", sk.name);
			}
			{
				FontPush f(fHead);
				const std::string label = level == 0 ? "UNRATED" : sk.byRank ? kRank[level] : Upper(Combine::SaveLevelName(level));
				ImGui::TextColored(kRankCol[level], "%s", label.c_str());
			}
			ImGui::Dummy(ImVec2(tileW, 2.0f * S));
			ImGui::EndGroup();
			col = (col + 1) % perRow;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, kSub);
		if (rated >= 3)
			ImGui::TextWrapped("Overall: about %s (the average of your %d rank-measured skills). A skill's rating is the highest level where your "
				"best set is a silver or better.", kRank[std::clamp((sum + rated / 2) / rated, 1, Combine::kSaveLevels)], rated);
		else
			ImGui::TextWrapped("Each skill is rated by the highest level where your best set is a silver or better (21 of 30; scoring: 12). "
				"The levels are measured from ranked and pro replays, so a rating is the rank you play that skill at.");
		ImGui::PopStyleColor();
		ImGui::Spacing();

		// ---- Today's plan (Nov 10): about ten minutes, built from the ratings above. 1: your lowest-rated skill, one
		// level above where you've cleared it (against Nexto where it can be). 2: today's set. 3: a lesson. ----
		if (weakest)
		{
			{
				FontPush f(fBig ? fBig : fHead);
				ImGui::TextColored(kText, "TODAY'S PLAN");
			}
			const int target = std::clamp(weakestLevel + 1, 1, Combine::kSaveLevels);
			const std::string ev = weakest->codes[1] ? weakest->codes[1] : weakest->codes[0];
			{
				CapsFont f;
				ImGui::TextColored(kOrange, "1");
			}
			ImGui::SameLine(0, 10.0f * S);
			ImGui::TextColored(kSub, "%s at %s: your lowest rating. A silver or better moves it up.", weakest->name, Combine::SaveLevelName(target));
			ImGui::SameLine(0, 10.0f * S);
			ImGui::PushID("plan1");
			if (AccentButton("Start")) onGame([this, ev, target]() { CombineStart(ev, target, 0); });
			ImGui::PopID();
			{
				CapsFont f;
				ImGui::TextColored(kOrange, "2");
			}
			ImGui::SameLine(0, 10.0f * S);
			ImGui::TextColored(kSub, "Today's set, just below.");
			{
				CapsFont f;
				ImGui::TextColored(kOrange, "3");
			}
			ImGui::SameLine(0, 10.0f * S);
			ImGui::TextColored(kSub, "One lesson in Learn: the skill tree picks your next one.");
			ImGui::SameLine(0, 10.0f * S);
			ImGui::PushID("plan3");
			if (QuietButton("Open Learn"))
			{
				uiCombineMode = false;
				uiLearnMode = true;
				{
					std::lock_guard<std::mutex> lock(uiLearnMutex);
					uiLearn = UiLearn{};
				}
				onGame([this]() { CombineClose(); LearnOpen(); });
			}
			ImGui::PopID();
			ImGui::Spacing();
		}
	}

	// ---- The daily set (Nov 10): one code a day, the same for everyone (by the UTC date), a different event each day
	// of the week, at the level you've picked below. Your best on it is kept like any other code. ----
	{
		const long long day = std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24;
		static const char* kDaily[7] = { "SV", "KON", "SG", "FFN", "SDN", "RD", "FT" };
		const std::string ev = kDaily[day % 7];
		const uint32_t seed = static_cast<uint32_t>(day) * 2654435761u + 20261110u;
		const int lv = uiCombineLevel;
		const std::string base = ev.size() == 3 ? ev.substr(0, 2) : ev;
		{
			FontPush f(fBig ? fBig : fHead);
			ImGui::TextColored(kText, "TODAY'S SET");
		}
		ImGui::TextColored(kSub, "%s%s, %s: the same 10 for everyone today.", EventName(base), ev.size() == 3 ? " vs Nexto" : "", Combine::SaveLevelName(lv));
		if (AccentButton("Play today's set", ImVec2(170 * S, 32.0f * S))) onGame([this, ev, lv, seed]() { CombineStart(ev, lv, seed); });
		auto it = u.bests.find(Combine::SetCode(ev.c_str(), lv, seed));
		if (it != u.bests.end()) { ImGui::SameLine(0, 10.0f * S); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f * S); Pill(std::format("Today's best {}", it->second), kChip); }
		ImGui::Spacing();
	}

	{
		FontPush f(fBig ? fBig : fHead);
		ImGui::TextColored(kText, "EVENTS");
	}
	ImGui::PushStyleColor(ImGuiCol_Text, kSub);
	ImGui::TextWrapped("Short scored events. Each one runs as a set of 10 from a code: the same code always gives the same set, so you can "
		"grind it, roll a new one, or send it to a friend.");
	ImGui::PopStyleColor();
	if (!u.status.empty()) ImGui::TextColored(kWarn, "%s", u.status.c_str());
	ImGui::Spacing();

	// The level (the same five for every event), then a card per event
	{
		FontPush f(fLabel);
		ImGui::TextColored(kMuted, "LEVEL");
	}
	for (int l = 1; l <= Combine::kSaveLevels; l++)
	{
		if (l > 1) ImGui::SameLine();
		const bool on = uiCombineLevel == l;
		ImGui::PushID(l);
		if (on ? AccentButton(Combine::SaveLevelName(l), ImVec2(92 * S, 0)) : QuietButton(Combine::SaveLevelName(l), ImVec2(92 * S, 0))) uiCombineLevel = l;
		ImGui::PopID();
	}
	const int pick = uiCombineLevel;
	// An event card (style A): the title condensed, what it is, a blue start button, your best and what the level means
	auto eventCard = [&](const char* code, const char* title, const char* about, const std::string& levelNote, bool canNexto = false) {
		ImGui::Spacing();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float pad = 12.0f * S;
		ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + W - 2.0f * pad);
		{
			FontPush f(fHead);
			ImGui::TextColored(kText, "%s", title);
		}
		ImGui::PushStyleColor(ImGuiCol_Text, kSub);
		ImGui::TextWrapped("%s", about);
		ImGui::PopStyleColor();
		ImGui::PushID(code);
		// 50s, kickoffs and shadow defense: the other car played back as the real player did, or by Nexto from there
		bool& vsN = uiCombineNexto[code];
		if (canNexto)
		{
			ImGui::TextColored(kMuted, "THE OTHER CAR");
			ImGui::SameLine();
			if (RadioButton("Real player", !vsN)) vsN = false;
			ImGui::SameLine();
			if (RadioButton("Nexto", vsN)) vsN = true;
		}
		const std::string ev = std::string(code) + (canNexto && vsN ? "N" : "");
		if (AccentButton("Start a new set", ImVec2(170 * S, 32.0f * S))) onGame([this, ev, pick]() { CombineStart(ev, pick, 0); });
		ImGui::PopID();
		auto it = u.bests.find(std::format("{}{}", ev, pick));
		if (it != u.bests.end()) { ImGui::SameLine(0, 10.0f * S); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f * S); Pill(std::format("Best {}", it->second), kChip); }
		ImGui::TextColored(kMuted, "%s", levelNote.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p0, p1, U32(kPanel), 6.0f * S);
		dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f * S, p1.y), U32(kAccent));
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
		ImGui::Dummy(ImVec2(W, 2.0f * S));
	};
	// What each saves level is, measured on 11,000 ranked saves (Oct 30): the spare time of the saves made at each rank
	static const char* kSaveRankNote[Combine::kSaveLevels] = { "a typical Gold save", "a typical Champ to GC save",
		"a hard GC or SSL save", "the hardest 10% of SSL saves", "the hardest 3% of SSL and pro saves" };
	float slo, shi, rlo, rhi;
	Combine::SaveLevelBand(pick, slo, shi);
	Combine::RecoveryLevelBand(pick, rlo, rhi);
	eventCard("SV", "SAVES", "10 real saves from match replays: the shot comes in as it really did, and you start where the defender "
		"was, with their speed, facing and boost (mirrored and nudged a little, so you can't learn them). A save is 2 points, a save "
		"that clears it out of danger is 3.",
		std::format("{}: a good player has {:.2f}-{:.2f} s to spare ({})", Combine::SaveLevelName(pick), slo, shi, kSaveRankNote[pick - 1]));
	eventCard("RC", "RECOVERY", "10 throws: you're launched into the air, tilted and spinning, toward the floor or a wall. Land and get "
		"to the blue ring by your back post (the ball sits in it, so ball cam points you there) as fast as you can, any way you like. Points by time against the throw's par (a good "
		"player landing well and driving there); the higher the level, the closer to par you need to be. The camera follows your momentum until "
		"you land (road2_combine_momentum_cam 0 turns that off).",
		std::format("{}: {:.2f}-{:.2f} s to spare after the turn", Combine::SaveLevelName(pick), rlo, std::min(rhi, 2.5f)));
	// Rings (RG) was cut Nov 1: rings maps already do it. Its code stays (Combine.cpp) but it's off the menu, like the
	// speed course and boost pathing.
	{
		float dlo, dhi;
		Combine::ReadLevelBand(pick, dlo, dhi);
		eventCard("RD", "WALL AND BACKBOARD READS", "10 balls off a wall or a backboard: off your own backboard (read it and clear it), off "
			"the side wall in their half, or off their backboard (read it and score). The read is a touch after it comes off and before "
			"it lands, and at higher levels before it drops below a height: in the air from Sharp, above crossbar height at Legend. Your "
			"backboard: cleared 3, kept out 2. Their side: goal 3, read but no goal 1. A touch before the wall or too late counts for less. "
			"Higher levels: harder, higher shots with spin, starting later in the flight, with you further away.",
			std::format("{}: a good player has {:.2f}-{:.2f} s to spare{}", Combine::SaveLevelName(pick), dlo, dhi,
				pick >= 5 ? ", meet it above the crossbar" : pick >= 3 ? ", meet it in the air" : pick >= 2 ? ", meet it before it drops low" : ""));
	}
	eventCard("FT", "FIRST TOUCHES", "10 real passes from match replays: the ball just after your teammate hit it, and you where the "
		"receiver was (mirrored and nudged a little). Score off it for 3, take it under control for 2 (for a second after your first "
		"touch the ball stays with you), or at least get a touch for 1.",
		std::format("{}: {}", Combine::SaveLevelName(pick), Combine::TouchLevelNote(pick)));
	eventCard("FF", "50s", "10 real 50/50s from ranked and pro games. You're one of the two players, 1.5 s before they met it; a bot "
		"plays the other one's real path, and the ball follows its real path until you get close. Win it (the ball goes your way) 3, "
		"even 1, lost 0. Against Nexto, the other car still makes the real run-in, and Nexto takes over just before the 50.",
		std::format("{}: against {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick)), true);
	eventCard("KO", "KICKOFFS", "10 real kickoffs from ranked and pro games: you take one taker's spawn, and the bot plays the other "
		"taker's real kickoff (speedflips and all). Where the ball is 1.5 s after the hit: their half 3, the middle 1, yours 0. You see "
		"your time to the ball against theirs. Against Nexto, the kickoff is called during a real 3-2-1, as a teammate would "
		"call it (speedflip, delay, fake or spanish). Possession is what counts: the ball well into their half 3, well into "
		"yours 0, and down the middle the boost decides (more 2, even 1, less 0). A goal 3. Full points only if you did the "
		"call. On a spanish their last back comes on too: fake it, go round the ball and demo them (a demo 3, a bump 1).",
		std::format("{}: against {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick)), true);
	eventCard("SD", "SHADOW DEFENSE", "10 real attacks from ranked and pro games. The bot brings the ball at you as the real attacker "
		"did (every one of these ended in a goal or a shot on target), and you're the defender. Stop it and win it back 3, stop it 2, "
		"they don't score 1, scored on 0. Against Nexto, it attacks from the real attacker's spot and plays it its own way.",
		std::format("{}: against {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick)), true);
	{
		static const char* kNextoLevel[Combine::kSaveLevels] = { "Nexto waits 1 s after your take and reacts 0.22 s late",
			"Nexto waits 0.75 s and reacts 0.17 s late", "Nexto waits 0.5 s and reacts 0.12 s late",
			"Nexto waits 0.25 s and reacts 0.08 s late", "Nexto at full speed from your take (0.05 s, as it really plays)" };
		eventCard("SG", "SCORE ON A GOALIE", "10 real takes from ranked and pro games, 7 on the ground and 3 off the wall: a player "
			"won the ball in midfield or the other half with the defender back. You start a moment before, driving up to it, and "
			"the take is yours: dribble, bounce dribble, power shot, ground to air, double tap, anything. Nexto (the RLGym bot) "
			"defends with full boost, from where the real defender was. A goal 3, a shot on target it saves 1, anything else 0; "
			"it's over when Nexto wins it, or 20 s after the take.",
			std::format("{}: takes from {}; {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick), kNextoLevel[pick - 1]));
	}
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "Play a code:");
	ImGui::SameLine();
	ImGui::PushItemWidth(150 * S);
	ImGui::InputTextWithHint("##play", "SV3-1A2B3C4D", &uiCombineCode);
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (AccentButton("Play"))
	{
		std::string ev; int l = 1; uint32_t sd = 0;
		if (Combine::ParseSetCode(uiCombineCode, ev, l, sd) && (ev == "SV" || ev == "RC" || ev == "RD" || ev == "FT" || ev == "FF" || ev == "SD" || ev == "KO" || ev == "FFN" || ev == "SDN" || ev == "KON" || ev == "SG")) onGame([this, ev, l, sd]() { CombineStart(ev, l, sd); });
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
}
