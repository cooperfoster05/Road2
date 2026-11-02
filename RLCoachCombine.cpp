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
		return code == "RC" ? "RECOVERY" : code == "RD" ? "READS" : code == "FT" ? "FIRST TOUCHES" : code == "FF" ? "50s" : code == "SD" ? "SHADOW DEFENSE" : code == "KO" ? "KICKOFFS" : "SAVES";
	}
	const char* ItemName(const std::string& code)
	{
		return code == "RC" ? "Throw" : code == "RD" ? "Read" : code == "FT" ? "Pass" : code == "FF" ? "Challenge" : code == "SD" ? "Attack" : code == "KO" ? "Kickoff" : "Shot";
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
	u.code = combineSeed ? Combine::SetCode(combineEvent.empty() ? "SV" : combineEvent.c_str(), combineLevel, combineSeed) : "";
	u.status = combineStatus;
	u.shot = std::min(combineIndex + 1, CombineCount());
	u.shots = CombineCount();
	u.event = EventName(combineEvent);
	u.item = ItemName(combineEvent);
	u.eventCode = combineEvent.empty() ? "SV" : combineEvent;
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
	if (!predictor.Ready()) { combineStatus = "The simulator is still loading; try again in a moment"; CombinePublish(); return; }
	if (combineGenBusy) return;
	if (combineGenThread.joinable()) combineGenThread.join();
	PracticeExit();
	combineEvent = event;
	combineLevel = std::clamp(level, 1, Combine::kSaveLevels);
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
	const std::string ev = event;
	const std::vector<Combine::SaveSituation>* sits = combineSituations.empty() ? nullptr : &combineSituations;
	if (ev == "FT" && combinePasses.empty())
	{
		combinePhase = CombinePhase::Off;
		combineGenBusy = false;
		combineStatus = "No pass data yet (data/RLCoach/combine/pass_situations.csv)";
		CombinePublish();
		return;
	}
	const std::vector<Combine::SaveSituation>* passes = &combinePasses;
	const std::filesystem::path folder = writer ? writer->Folder() : std::filesystem::path();
	combineGenThread = std::thread([this, sd, lv, ev, sits, passes, folder]() {
		auto built = std::make_shared<CombineBuilt>();
		try
		{
			if (ev == "RC") built->throws = Combine::MakeRecoverySet(sd, lv, 10);
			else if (ev == "RD") built->reads = Combine::MakeReadSet(sd, lv, 10);
			else if (ev == "FT") built->touches = Combine::MakeTouchSet(sd, lv, 10, *passes);
			else if (ev == "FF" || ev == "SD" || ev == "KO")
			{
				if (!combineChallengesLoaded)
				{
					combineChallengesAll = Combine::LoadChallenges(ReadAllText(folder / "combine" / "challenges.csv"));
					combineChallengesLoaded = true;
				}
				built->challenges = Combine::MakeChallengeSet(sd, lv, 10, ev == "FF" ? 'F' : ev == "KO" ? 'K' : 'D', combineChallengesAll);
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
		combineStatus = CombineUsesBot() && combineChallengesAll.empty() ? "No 50s / shadow data yet (data/RLCoach/combine/challenges.csv)"
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

void RLCoach::CombineEndShot(int points, const std::string& what)
{
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
		if (CombineUsesBot() && BotCar().IsNull())
		{
			// the bot takes a moment to show up after it's spawned
			combinePhaseTicks = 1;
			if (++combineBotWait > 600) { combineStatus = "Couldn't spawn the bot (open free play and try again)"; CombineExit(); }
			return;
		}
		if (combinePhaseTicks < kCountdownTicks) { CombinePlace(car, ball, true); return; }
		CombinePlace(car, ball, false);
		combinePhase = CombinePhase::Live;
		combineLiveTicks = 0;
		combineTouched = false;
		combineLastTouch = -1;
		combineFirstTouch = -1;
		combineCtrlTicks = combineCtrlTotal = 0;
		combineBallFree = false;
		combineBotTouched = false;
		combineBotFirstTouch = -1;
		combineKoYou = combineKoBot = -1;
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
			const std::string code = Combine::SetCode(combineEvent.c_str(), combineLevel, combineSeed);
			const std::string levelKey = std::format("{}{}", combineEvent, combineLevel);
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
		// the bot on its real path; the ball on its real path until you're about to touch it (or its path ends)
		if (t <= c.botEnd) CombinePlaceBot(t, false);
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
				if (t > c.tContact + 1.5f) CombineEndShot(0, "Never got to it");
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
		if (!combineTouched && combineBallFree && t > c.ballEnd + 2.0f && !danger) { CombineEndShot(1, "They didn't score"); return; }
		if (t > c.botEnd + 4.0f) { CombineEndShot(combineTouched ? 2 : 1, combineTouched ? "Stopped it" : "They didn't score"); return; }
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
		// The momentum camera (try-out, rlcoach_combine_momentum_cam): until you land, look the way you're travelling,
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
		std::string line = BotCar().IsNull() ? "Waiting for the bot..." : combineEvent == "FF" ? "50/50: win the challenge" : "Shadow: stay goal-side, win it back";
		if (combineEvent == "KO" && !BotCar().IsNull() && combineIndex < static_cast<int>(combineChallenges.size()))
		{
			const float ax = std::fabs(combineChallenges[combineIndex].you.pos.x);
			line = std::format("Kickoff: {}", ax > 1500.0f ? "diagonal" : ax > 100.0f ? "off-centre" : "back centre");
		}
		canvas.SetColor(255, 255, 255, 230);
		const Vector2F ls = canvas.GetStringSize(line, 2.0f, 2.0f);
		canvas.SetPosition(Vector2F{ (sz.X - ls.X) * 0.5f, sz.Y * 0.20f });
		canvas.DrawString(line, 2.0f, 2.0f, true);
	}
	if (combineEvent == "RD" && combineIndex < static_cast<int>(combineReads.size()) && combinePhase == CombinePhase::Countdown)
	{
		static const char* kWhat[3] = { "Off your backboard: read it, clear it", "Off the side wall: read it, score it", "Off their backboard: read it, score it" };
		static const char* kHigh[Combine::kSaveLevels] = { "", " (before it drops low)", " (in the air)", " (in the air)", " (above crossbar height)" };
		const std::string line = std::string(kWhat[std::clamp(combineReads[combineIndex].kind, 0, 2)]) + kHigh[std::clamp(combineLevel, 1, Combine::kSaveLevels) - 1];
		canvas.SetColor(255, 255, 255, 230);
		const Vector2F ls = canvas.GetStringSize(line, 2.0f, 2.0f);
		canvas.SetPosition(Vector2F{ (sz.X - ls.X) * 0.5f, sz.Y * 0.20f });
		canvas.DrawString(line, 2.0f, 2.0f, true);
	}
	int pts = 0, maxPts = 0;
	CombineScore(pts, maxPts, combinePhase == CombinePhase::Result || combinePhase == CombinePhase::Done);
	std::string top;
	if (combinePhase == CombinePhase::Building) top = "COMBINE  ·  building your set...";
	else if (combinePhase == CombinePhase::Done) top = std::format("COMBINE  ·  {}  ·  {}  ·  done: {} of {} points", EventName(combineEvent), Combine::SaveLevelName(combineLevel), pts, maxPts);
	else top = std::format("COMBINE  ·  {}  ·  {}  ·  {} {} of {}  ·  {} points", EventName(combineEvent), Combine::SaveLevelName(combineLevel),
		combineEvent == "RC" ? "throw" : combineEvent == "RD" ? "read" : combineEvent == "FT" ? "pass" : combineEvent == "FF" ? "50" : combineEvent == "SD" ? "attack" : combineEvent == "KO" ? "kickoff" : "shot", combineIndex + 1, CombineCount(), pts);
	canvas.SetColor(255, 255, 255, 235);
	const Vector2F ts = canvas.GetStringSize(top, 1.6f, 1.6f);
	canvas.SetPosition(Vector2F{ (sz.X - ts.X) * 0.5f, sz.Y * 0.08f });
	canvas.DrawString(top, 1.6f, 1.6f, true);
	std::string mid;
	if (combinePhase == CombinePhase::Countdown)
		mid = combineResets > 0 ? std::format("{}   (retry, {} reset{} left)", (kCountdownTicks - combinePhaseTicks) / 40 + 1, kResetsPerItem - combineResets, kResetsPerItem - combineResets == 1 ? "" : "s")
			: std::format("{}", (kCountdownTicks - combinePhaseTicks) / 40 + 1);
	else if (combinePhase == CombinePhase::Result && combineIndex < static_cast<int>(combineOutcomes.size())) mid = combineOutcomes[combineIndex];
	if (!mid.empty())
	{
		const bool good = combinePhase == CombinePhase::Result && combinePoints[combineIndex] > 0;
		const bool bad = combinePhase == CombinePhase::Result && combinePoints[combineIndex] == 0;
		canvas.SetColor(good ? 74 : bad ? 248 : 255, good ? 222 : bad ? 113 : 255, good ? 128 : bad ? 113 : 255, 240);
		const Vector2F ms = canvas.GetStringSize(mid, 3.0f, 3.0f);
		canvas.SetPosition(Vector2F{ (sz.X - ms.X) * 0.5f, sz.Y * 0.14f });
		canvas.DrawString(mid, 3.0f, 3.0f, true);
	}
}

// ---- The Combine page in the window ----
void RLCoach::RenderCombine()
{
	using namespace UiStyle;
	const float S = Sc();   // the window's size right now (UiStyle.h)
	ImFont* fLabel = uiFonts[UiFonts::Label];
	ImFont* fHead = uiFonts[UiFonts::Head];
	auto onGame = [this](std::function<void()> fn) { gameWrapper->Execute([fn](GameWrapper*) { fn(); }); };
	UiCombine u;
	{
		std::lock_guard<std::mutex> lock(uiCombineMutex);
		u = uiCombine;
	}

	{
		FontPush f(fLabel);
		ImGui::TextColored(kAccent, "ROAD2");
		ImGui::SameLine();
		ImGui::TextColored(kMuted, "COMBINE");
	}
	ImGui::SameLine(std::max(200.0f * S, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Back to reps").x - 16.0f * S));
	if (LinkButton("Back to reps"))
	{
		uiCombineMode = false;
		onGame([this]() { CombineClose(); });
	}
	ImGui::Spacing();

	if (u.running)
	{
		{
			FontPush f(fHead);
			ImGui::TextColored(kText, "%s  ·  %s", u.event.c_str(), u.levelName.c_str());
		}
		if (!u.code.empty())
		{
			ImGui::TextColored(kSub, "Set code:");
			ImGui::SameLine();
			std::string code = u.code;
			ImGui::PushItemWidth(150 * S);
			ImGui::InputText("##code", &code, ImGuiInputTextFlags_ReadOnly);
			ImGui::PopItemWidth();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Share this code: it gives anyone the exact same set.");
		}
		if (u.building) ImGui::TextColored(kSub, "%s", u.status.c_str());
		else
		{
			if (u.done) ImGui::TextColored(kText, "Done: %d of %d points", u.points, u.maxPoints);
			else ImGui::TextColored(kText, "%s %d of %d  ·  %d points so far", u.item.c_str(), u.shot, u.shots, u.points);
			if (u.done && !u.medal.empty()) { ImGui::SameLine(); ImGui::TextColored(kGold, "%s", u.medal.c_str()); }
			if (u.best >= 0) ImGui::TextColored(kMuted, "Your best on this set: %d", u.best);
			for (size_t i = 0; i < u.outcomes.size(); i++)
				if (!u.outcomes[i].empty())
				{
					const int pt = i < u.pointsEach.size() ? u.pointsEach[i] : 0;
					ImGui::TextColored(pt < 0 ? kMuted : pt == 0 ? kBad : pt >= 3 ? kGood : kWarn, "%zu. %s", i + 1, u.outcomes[i].c_str());
				}
		}
		ImGui::Spacing();
		const int lv = u.level;
		const std::string code = u.code;
		if (ImGui::Button(u.done ? "Play this set again" : "Restart this set", ImVec2(170 * S, 0)))
		{
			std::string ev; int l = lv; uint32_t sd = 0;
			if (Combine::ParseSetCode(code, ev, l, sd)) onGame([this, ev, l, sd]() { CombineStart(ev, l, sd); });
		}
		const std::string ev = u.eventCode;
		ImGui::SameLine();
		if (ImGui::Button("New set", ImVec2(110 * S, 0))) onGame([this, ev, lv]() { CombineStart(ev, lv, 0); });
		ImGui::SameLine();
		if (OutlineButton("Stop", ImVec2(80 * S, 0))) onGame([this]() { CombineExit(); });
		return;
	}

	{
		FontPush f(fHead);
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
		if (on ? OutlineButton(Combine::SaveLevelName(l), ImVec2(92 * S, 0)) : ImGui::Button(Combine::SaveLevelName(l), ImVec2(92 * S, 0))) uiCombineLevel = l;
		ImGui::PopID();
	}
	const int pick = uiCombineLevel;
	auto eventCard = [&](const char* code, const char* title, const char* about, const std::string& levelNote) {
		ImGui::Spacing();
		{
			FontPush f(fLabel);
			ImGui::TextColored(kAccent, "%s", title);
		}
		ImGui::PushStyleColor(ImGuiCol_Text, kSub);
		ImGui::TextWrapped("%s", about);
		ImGui::PopStyleColor();
		ImGui::PushID(code);
		const std::string ev = code;
		if (ImGui::Button("Start a new set", ImVec2(170 * S, 0))) onGame([this, ev, pick]() { CombineStart(ev, pick, 0); });
		ImGui::PopID();
		ImGui::SameLine();
		auto it = u.bests.find(std::format("{}{}", code, pick));
		ImGui::TextColored(kMuted, "%s%s", levelNote.c_str(), it == u.bests.end() ? "" : std::format("  ·  your best: {} points", it->second).c_str());
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
		"you land (rlcoach_combine_momentum_cam 0 turns that off).",
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
		"even 1, lost 0.",
		std::format("{}: against {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick)));
	eventCard("KO", "KICKOFFS", "10 real kickoffs from ranked and pro games: you take one taker's spawn, and the bot plays the other "
		"taker's real kickoff (speedflips and all). Where the ball is 1.5 s after the hit: their half 3, the middle 1, yours 0. You see "
		"your time to the ball against theirs.",
		std::format("{}: against {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick)));
	eventCard("SD", "SHADOW DEFENSE", "10 real attacks from ranked and pro games. The bot brings the ball at you as the real attacker "
		"did (every one of these ended in a goal or a shot on target), and you're the defender. Stop it and win it back 3, stop it 2, "
		"they don't score 1, scored on 0.",
		std::format("{}: against {}", Combine::SaveLevelName(pick), Combine::ChallengeLevelNote(pick)));
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "Play a code:");
	ImGui::SameLine();
	ImGui::PushItemWidth(150 * S);
	ImGui::InputTextWithHint("##play", "SV3-1A2B3C4D", &uiCombineCode);
	ImGui::PopItemWidth();
	ImGui::SameLine();
	if (ImGui::Button("Play"))
	{
		std::string ev; int l = 1; uint32_t sd = 0;
		if (Combine::ParseSetCode(uiCombineCode, ev, l, sd) && (ev == "SV" || ev == "RC" || ev == "RD" || ev == "FT" || ev == "FF" || ev == "SD" || ev == "KO")) onGame([this, ev, l, sd]() { CombineStart(ev, l, sd); });
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
}
