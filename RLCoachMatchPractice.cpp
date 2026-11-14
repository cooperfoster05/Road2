// Practise from here (Nov 13): a moment of a scanned match, in free play.
//
// You pick a moment in the match review. In free play the ball and every car start where they were 1.5 s before it,
// moving the way they were; you take one player (the one selected on the page), and Road2 spawns a bot for each of
// the others. Up to the moment they drive exactly what the real players did (so it sets up the way it really
// happened); from the moment on Nexto drives them, so they react to whatever you do. A bot is handed to Nexto early
// if the ball or you get close to it. The ball stays on its real path until someone is about to touch it.
//
// Everything waits, frozen, until you move. A try ends with a goal or six seconds after the moment, and is judged by
// the scoring chance (the same model as the review) against what happened in the game at the same point. Then it
// goes back to the start for another try. Free play's reset button also starts it again.
//
// If you were orange, everything is turned round (x and y the other way) so you're blue, as free play has you.
#include "pch.h"
#include "RLCoach.h"
#include "HudDraw.h"
#include "bakkesmod/wrappers/gameobject/replaymanagerwrapper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace
{
	constexpr float kU2R = 3.14159265f / 32768.0f;
	constexpr float kLeadSecs = 1.5f;        // the start: this long before the moment
	constexpr float kAfterSecs = 6.0f;       // a try runs this long after the moment (or to a goal)
	constexpr int kGraceTicks = 18;          // frozen this long after a (re)start before a move can begin the try
	constexpr int kResultTicks = 150;        // how long the end of a try plays on before it goes back to the start
	constexpr int kSpawnEvery = 36;          // a bot every 0.3 s (the game fills the shorter team, one at a time)
	constexpr int kSpawnWaitTicks = 720;     // after the last one, wait up to 6 s for them all to show up
	constexpr int kNextoDelay = 6;           // Nexto's reaction time in ticks, as in RLBot

	bool AnyInput(const ControllerInput& in)
	{
		const float dz = 0.1f;
		return std::fabs(in.Throttle) > dz || std::fabs(in.Steer) > dz || std::fabs(in.Pitch) > dz || std::fabs(in.Yaw) > dz
			|| std::fabs(in.Roll) > dz || in.Jump || in.ActivateBoost || in.HoldingBoost || in.Handbrake;
	}

	struct Pose
	{
		Vector p, v, w;
		Rotator r;
		float boost = 0;   // 0-100
	};

	int WrapYaw(int y) { return ((y + 32768) % 65536 + 65536) % 65536 - 32768; }

	// A car's state at t seconds after the start frame, between the scan's frames (they're about 0.045 s apart).
	// Turned round when `mirror` (you were orange).
	bool CarAt(const MatchData& d, int start, int player, float t, bool mirror, Pose& out)
	{
		const int n = static_cast<int>(d.frames.size());
		if (start < 0 || start >= n) return false;
		const float T = d.frames[start].time + t;
		int k = start;
		while (k + 1 < n && d.frames[k + 1].time <= T) k++;
		const int k2 = std::min(k + 1, n - 1);
		if (player < 0 || player >= static_cast<int>(d.frames[k].cars.size())) return false;
		const MatchCar& a = d.frames[k].cars[player];
		if (!a.present) return false;
		const MatchCar& b = player < static_cast<int>(d.frames[k2].cars.size()) && d.frames[k2].cars[player].present ? d.frames[k2].cars[player] : a;
		const float span = d.frames[k2].time - d.frames[k].time;
		const float u = span > 1e-4f ? std::clamp((T - d.frames[k].time) / span, 0.0f, 1.0f) : 0.0f;
		auto L = [u](float x, float y) { return x + (y - x) * u; };
		const MatchCar& r = u < 0.5f ? a : b;
		const float s = mirror ? -1.0f : 1.0f;
		out.p = Vector(s * L(a.x, b.x), s * L(a.y, b.y), L(a.z, b.z));
		out.v = Vector(s * L(a.vx, b.vx), s * L(a.vy, b.vy), L(a.vz, b.vz));
		out.w = Vector(s * L(a.avx, b.avx), s * L(a.avy, b.avy), L(a.avz, b.avz));
		out.r = Rotator(r.pitch, mirror ? WrapYaw(r.yaw + 32768) : r.yaw, r.roll);
		out.boost = std::clamp(a.boost, 0.0f, 100.0f);
		return true;
	}

	bool BallAt(const MatchData& d, int start, float t, bool mirror, Vector& p, Vector& v)
	{
		const int n = static_cast<int>(d.frames.size());
		if (start < 0 || start >= n) return false;
		const float T = d.frames[start].time + t;
		int k = start;
		while (k + 1 < n && d.frames[k + 1].time <= T) k++;
		const int k2 = std::min(k + 1, n - 1);
		const MatchFrame& a = d.frames[k];
		const MatchFrame& b = d.frames[k2].hasBall ? d.frames[k2] : a;
		if (!a.hasBall) return false;
		const float span = b.time - a.time;
		const float u = span > 1e-4f ? std::clamp((T - a.time) / span, 0.0f, 1.0f) : 0.0f;
		auto L = [u](float x, float y) { return x + (y - x) * u; };
		const float s = mirror ? -1.0f : 1.0f;
		p = Vector(s * L(a.bx, b.bx), s * L(a.by, b.by), L(a.bz, b.bz));
		v = Vector(s * L(a.bvx, b.bvx), s * L(a.bvy, b.bvy), L(a.bvz, b.bvz));
		return true;
	}

	void Put(CarWrapper& c, const Pose& q, bool frozen)
	{
		c.SetLocation(q.p);
		c.SetRotation(q.r);
		c.SetVelocity(frozen ? Vector(0, 0, 0) : q.v);
		c.SetAngularVelocity(frozen ? Vector(0, 0, 0) : q.w, false);
	}

	// ---- Nexto's view of a car, and its controls as the game's input (as in RLCoachNexto.cpp) ----
	Nexto::Car NextoCar(CarWrapper& c, int team)
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

	// ---- The in-game text, in the Combine's style ----
	constexpr HudDraw::Rgba kNavyA{ 11, 15, 26, 225 }, kBlueA{ 46, 139, 255, 255 }, kOrangeA{ 255, 138, 31, 255 }, kWhiteA{ 255, 255, 255, 255 },
		kGoodA{ 61, 220, 132, 255 }, kWarnA{ 255, 176, 32, 255 }, kBadA{ 255, 90, 90, 255 }, kInkA{ 11, 15, 26, 255 };
	std::string Upper(std::string s) { for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return s; }
	void SlantRow(CanvasWrapper& c, float cx, float y, const std::vector<std::string>& parts, float scale, HudDraw::Rgba first, HudDraw::Rgba firstInk, float padX)
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
}

void RLCoach::MatchPracticePublish(const std::string& status)
{
	std::lock_guard<std::mutex> lock(uiMatchMutex);
	uiMp.active = mpPhase != MpPhase::Off;
	uiMp.label = mpLabel;
	if (!status.empty() || mpPhase == MpPhase::Off) uiMp.status = status;
	uiMp.result = mpResult;
	uiMp.real = mpReal;
	uiMp.tries = mpTries;
	uiMp.good = mpResultGood;
	uiMp.loading = mpTripWait >= 0;
	uiMp.canReturn = !mpReturnId.empty();
	uiMp.listPos = mpListPos;
	uiMp.listGood = mpListGood;
	uiMp.listResult = mpListResult;
}

namespace
{
	std::chrono::steady_clock::time_point gTripStart;
	std::string gReturnPath;   // the replay file, for the second way back
}

// ---- The round trip (Nov 13): from the replay into free play and back, so you never go looking for anything ----

// Free play on the replay's arena. road2_match_freeplay_load picks how: 0 the game's own map-load command (the way
// free play is started from the menu), 1 BakkesMod's load_freeplay.
void RLCoach::MatchLoadFreeplay(const std::string& map)
{
	const std::string m = map.empty() ? std::string("Stadium_P") : map;
	if (*matchFreeplayLoad == 1)
	{
		cvarManager->executeCommand("load_freeplay");
		LOG("Road2 practise from here: load_freeplay");
	}
	else
	{
		const std::string cmd = std::format("start {}?Game=TAGame.GameInfo_Tutorial_TA?GameTags=Freeplay", m);
		gameWrapper->ExecuteUnrealCommand(cmd);
		LOG("Road2 practise from here: {}", cmd);
	}
}

void RLCoach::MatchPracticeGo(std::vector<MpItem> items, bool asList)
{
	if (items.empty()) return;
	// (a replay counts as "online" to the game, so only refuse a real online match)
	const bool inReplay = gameWrapper->IsInReplay();
	LOG("Road2 practise from here: {} moment(s), from {}", items.size(), inReplay ? "a replay" : gameWrapper->IsInFreeplay() ? "free play" : "elsewhere");
	if (!inReplay && gameWrapper->IsInOnlineGame()) { LOG("Road2 practise from here: refused, in an online match"); MatchPracticePublish("Not from an online match"); return; }
	std::shared_ptr<const MatchData> data;
	{
		std::lock_guard<std::mutex> lock(uiMatchMutex);
		data = uiMatchData;
	}
	if (!data) { LOG("Road2 practise from here: no scan loaded"); MatchPracticePublish("Open a scanned match first"); return; }
	std::string map;
	// where to come back to: the replay you're watching, at the moment you're on; otherwise the scan's replay file at
	// the first moment
	if (inReplay)
	{
		ReplayServerWrapper server = gameWrapper->GetGameEventAsReplay();
		ReplayWrapper replay = server.GetReplay();
		if (!server.IsNull() && !replay.IsNull())
		{
			mpReturnId = replay.GetId().ToString();
			gReturnPath = replay.GetFilePath().ToString();
			mpReturnTime = static_cast<float>(server.GetCurrentReplayFrame()) / static_cast<float>(std::max(1, server.GetReplayFPS()));
			map = replay.GetMapName();
		}
	}
	else if (!gameWrapper->IsInFreeplay() || mpReturnId.empty())
	{
		const std::filesystem::path f(data->replayFile);
		mpReturnId = f.stem().string();
		gReturnPath = data->replayFile;
		const int idx = std::clamp(items.front().frameIndex, 0, std::max(0, static_cast<int>(data->frames.size()) - 1));
		mpReturnTime = data->frames.empty() ? 0.0f : std::max(0.0f, data->frames[idx].frame / static_cast<float>(std::max(1, data->fps)) - 1.5f);
	}
	if (asList)
	{
		mpList = items;
		mpListPos = 0;
		mpListGood.assign(items.size(), -2);
		mpListResult.assign(items.size(), std::string());
	}
	else
	{
		mpList.clear();
		mpListPos = -1;
		mpListGood.clear();
		mpListResult.clear();
	}
	if (gameWrapper->IsInFreeplay()) { MatchPracticeStart(items.front().frameIndex, items.front().player); return; }
	// Not in free play yet: load it, and pick this up once it's there (MatchPracticeTripTick)
	mpQueue = std::move(items);
	mpQueueList = asList;
	mpTripWait = 0;
	gTripStart = std::chrono::steady_clock::now();
	MatchPracticePublish("Loading free play...");
	MatchLoadFreeplay(map);
}

// Every frame (from the HUD's draw): once free play has loaded and settled for a second, start what was asked for
void RLCoach::MatchPracticeTripTick()
{
	if (mpTripWait < 0) return;
	const float waited = std::chrono::duration<float>(std::chrono::steady_clock::now() - gTripStart).count();
	if (!gameWrapper->IsInFreeplay() || gameWrapper->GetLocalCar().IsNull())
	{
		mpTripWait = 0;
		if (waited > 30.0f) { mpTripWait = -1; mpQueue.clear(); MatchPracticePublish("Free play didn't load. Try road2_match_freeplay_load 1 in the console (F6), then again."); }
		return;
	}
	if (++mpTripWait < 60) return;   // about a second in free play first
	mpTripWait = -1;
	const MpItem first = mpQueue.front();
	mpQueue.clear();
	MatchPracticeStart(first.frameIndex, first.player);
}

// The open replay's moment now, as the player selected on the page (the hotkey and the page's button)
bool RLCoach::MatchItemNow(MpItem& out)
{
	if (!gameWrapper->IsInReplay()) return false;
	ReplayServerWrapper server = gameWrapper->GetGameEventAsReplay();
	if (server.IsNull()) return false;
	std::shared_ptr<const MatchData> data;
	std::shared_ptr<const MatchAnalysis> an;
	{
		std::lock_guard<std::mutex> lock(uiMatchMutex);
		data = uiMatchData;
		an = uiMatchAnalysis;
	}
	if (!data || !an || data->frames.empty()) return false;
	if (!matchOpenReplayName.empty() && an->replayName != matchOpenReplayName) return false;   // the page shows another match
	const int now = server.GetCurrentReplayFrame();
	int idx = static_cast<int>(std::lower_bound(data->frames.begin(), data->frames.end(), now, [](const MatchFrame& f, int fr) { return f.frame < fr; }) - data->frames.begin());
	idx = std::clamp(idx, 0, static_cast<int>(data->frames.size()) - 1);
	out.frameIndex = idx;
	out.player = matchWho.load();
	const MatchFrame& f = data->frames[idx];
	out.label = std::format("{}:{:02d}", f.clock / 60, f.clock % 60);
	return out.player >= 0 && out.player < static_cast<int>(data->players.size());
}

void RLCoach::MatchPracticeHere()
{
	MpItem it;
	if (!MatchItemNow(it)) { MatchPracticePublish("Open the scanned replay, and pick who you'll play on the Road2 page"); return; }
	MatchPracticeGo({ it }, false);
}

// Back to the replay, where you were. road2_match_return_mode: 0 by the replay's id, 1 by its file, 2 BakkesMod's
// replay-from-time.
void RLCoach::MatchPracticeBack()
{
	const std::string id = mpReturnId, path = gReturnPath;
	const float t = std::max(0.0f, mpReturnTime);
	if (id.empty() && path.empty()) { MatchPracticePublish("No replay to go back to"); return; }
	MatchPracticeStop();
	mpList.clear();
	mpListPos = -1;
	const int how = *matchReturnMode;
	LOG("Road2 practise from here: back to replay {} at {:.1f} s (way {})", id, t, how);
	if (how == 2) { gameWrapper->PlayReplayFromTime(std::wstring(path.begin(), path.end()), t); return; }
	ReplayManagerWrapper rm = gameWrapper->GetReplayManagerWrapper();
	if (rm.IsNull()) { MatchPracticePublish("Couldn't reach the game's replays"); return; }
	if (how == 1) rm.PlayReplayFile(path, "", t);
	else rm.PlayReplayId(id, "", t);
}

void RLCoach::MatchPracticeStart(int frameIndex, int player)
{
	if (!gameWrapper->IsInFreeplay()) { MatchPracticePublish("Open free play first: practise from here runs there (close the replay)"); return; }
	std::shared_ptr<const MatchData> data;
	std::shared_ptr<const MatchAnalysis> an;
	{
		std::lock_guard<std::mutex> lock(uiMatchMutex);
		data = uiMatchData;
		an = uiMatchAnalysis;
	}
	if (!data || !an || data->frames.empty()) { MatchPracticePublish("Open a scanned match first"); return; }
	const int n = static_cast<int>(data->frames.size()), P = static_cast<int>(data->players.size());
	if (player < 0 || player >= P || (data->players[player].team != 0 && data->players[player].team != 1)) { MatchPracticePublish("Pick a player first"); return; }
	if (!NextoReady()) { MatchPracticePublish("Nexto isn't there (" + nextoError + "). It comes in the Road2 download, in data\\Road2\\nexto."); return; }
	frameIndex = std::clamp(frameIndex, 0, n - 1);

	// Whatever else was running in free play stops (they all drive the car and ball themselves). Moving to another
	// moment of the same match keeps the bots already on the pitch.
	const bool keep = mpPhase != MpPhase::Off && mpData == data;
	if (mpPhase != MpPhase::Off && !keep) MatchPracticeStop();
	CombineExit();
	PracticeExit();
	ViewerExit();

	// The start: 1.5 s before the moment, but not back through a kickoff wait or a goal
	int start = frameIndex;
	while (start > 0 && data->frames[frameIndex].time - data->frames[start - 1].time <= kLeadSecs
		&& (an->live.empty() || an->live[start - 1]) && data->frames[start - 1].hasBall) start--;
	if (!data->frames[start].cars.empty() && (player >= static_cast<int>(data->frames[start].cars.size()) || !data->frames[start].cars[player].present))
	{
		MatchPracticePublish("That player isn't on the pitch there (demolished?): pick a moment a little later");
		return;
	}
	mpData = data;
	mpAnalysis = an;
	mpStart = start;
	mpMoment = frameIndex;
	mpMe = player;
	mpTeam = data->players[player].team;
	mpMirror = mpTeam == 1;
	mpBots.clear();
	for (int p = 0; p < P; p++)
	{
		if (p == player) continue;
		const int tm = data->players[p].team;
		if (tm != 0 && tm != 1) continue;
		if (p >= static_cast<int>(data->frames[start].cars.size()) || !data->frames[start].cars[p].present) continue;   // demolished then
		MpBot b;
		b.player = p;
		b.team = tm == mpTeam ? 0 : 1;
		mpBots.push_back(b);
	}
	const MatchFrame& mf = data->frames[frameIndex];
	mpLabel = std::format("{} at {}:{:02d}", data->players[player].name, mf.clock / 60, mf.clock % 60);
	mpTries = 0;
	mpResult.clear();
	mpReal.clear();
	mpResultGood = -2;
	mpPhaseTicks = 0;
	mpPhase = MpPhase::Spawning;
	if (keep)
	{
		ServerWrapper sv = gameWrapper->GetGameEventAsServer();
		const int have = sv.IsNull() ? 0 : std::max(0, sv.GetCars().Count() - 1);
		mpSpawned = std::min(have, static_cast<int>(mpBots.size()));
	}
	else
	{
		mpSpawned = 0;
		BotRemove();   // a bot left from the Combine, or from a try before
	}
	PracticeGoalsOff();
	{
		CarWrapper me = gameWrapper->GetLocalCar();
		BoostWrapper bc = me.IsNull() ? BoostWrapper(0) : me.GetBoostComponent();
		if (!bc.IsNull() && mpSavedUnlimited < 0) mpSavedUnlimited = bc.GetUnlimitedBoostRefCount();
	}
	MatchPracticePublish(mpBots.empty() ? "Setting up..." : std::format("Spawning {} bot{}...", mpBots.size(), mpBots.size() == 1 ? "" : "s"));
	LOG("Road2 practise from here: {} ({} bots, start frame {}, moment frame {})", mpLabel, mpBots.size(), start, frameIndex);
}

void RLCoach::MatchPracticeStop()
{
	if (mpPhase == MpPhase::Off) return;
	mpPhase = MpPhase::Off;
	mpBots.clear();
	mpListPos = -1;
	mpTripWait = -1;
	mpQueue.clear();
	BotRemove();
	PracticeGoalsRestore();
	if (mpSavedUnlimited >= 0)
	{
		CarWrapper me = gameWrapper->GetLocalCar();
		BoostWrapper bc = me.IsNull() ? BoostWrapper(0) : me.GetBoostComponent();
		if (!bc.IsNull()) bc.SetUnlimitedBoostRefCount(mpSavedUnlimited);
		mpSavedUnlimited = -1;
	}
	mpData.reset();
	mpAnalysis.reset();
	MatchPracticePublish();
}

void RLCoach::MatchPracticeInput(CarWrapper& car, void* params)
{
	if (mpPhase == MpPhase::Off || !params || car.IsNull()) return;
	for (const MpBot& b : mpBots)
	{
		if (!b.addr || car.memory_address != b.addr) continue;
		const bool driving = b.nexto && (mpPhase == MpPhase::Live || mpPhase == MpPhase::Result);
		*reinterpret_cast<ControllerInput*>(params) = driving ? ToInput(b.now) : ControllerInput{};
		return;
	}
}

void RLCoach::MatchPracticeTouch(CarWrapper& car)
{
	if (car.IsNull() || (mpPhase != MpPhase::Live && mpPhase != MpPhase::Result)) return;
	CarWrapper me = gameWrapper->GetLocalCar();
	if (!me.IsNull() && me.memory_address == car.memory_address) { mpTouched = true; mpBallFree = true; mpPoss = 0; return; }
	for (const MpBot& b : mpBots) if (b.addr && b.addr == car.memory_address) { mpPoss = b.team; mpBallFree = true; return; }
}

void RLCoach::MatchPracticeTick(CarWrapper& car)
{
	if (!gameWrapper->IsInFreeplay()) { MatchPracticeStop(); return; }
	if (!mpData || !mpAnalysis) { MatchPracticeStop(); return; }
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	BallWrapper ball = server.GetBall();
	if (ball.IsNull()) return;
	const MatchData& d = *mpData;
	const MatchAnalysis& an = *mpAnalysis;
	mpPhaseTicks++;

	// The bots' cars by address (a free play reset brings every car back as a new one)
	auto findCar = [&](uintptr_t addr) {
		if (!addr) return CarWrapper(0);
		ArrayWrapper<CarWrapper> cars = server.GetCars();
		for (int i = 0; i < cars.Count(); i++) { CarWrapper c = cars.Get(i); if (!c.IsNull() && c.memory_address == addr) return c; }
		return CarWrapper(0);
	};
	// Which car plays whom: a car on the right team for each bot (the game puts a new bot on the shorter team, so after
	// spawning one per player the teams come out right), any car if a team came up short. Spare cars go.
	auto gather = [&]() {
		ArrayWrapper<CarWrapper> cars = server.GetCars();
		std::vector<CarWrapper> others;
		for (int i = 0; i < cars.Count(); i++)
		{
			CarWrapper c = cars.Get(i);
			if (!c.IsNull() && c.memory_address != car.memory_address) others.push_back(c);
		}
		std::vector<bool> used(others.size(), false);
		for (MpBot& b : mpBots) b.addr = 0;
		const int mine = car.GetTeamNum2() == 1 ? 1 : 0;   // free play puts you on blue, but don't count on it
		for (int pass = 0; pass < 2; pass++)
			for (MpBot& b : mpBots)
			{
				if (b.addr) continue;
				for (size_t k = 0; k < others.size(); k++)
				{
					if (used[k]) continue;
					if (pass == 0 && others[k].GetTeamNum2() != (b.team == 0 ? mine : 1 - mine)) continue;
					used[k] = true;
					b.addr = others[k].memory_address;
					break;
				}
			}
		for (size_t k = 0; k < others.size(); k++)
		{
			AIControllerWrapper ai = others[k].GetAIController();
			if (!ai.IsNull()) ai.DoNothing();   // the game's own bot brain off: we drive it
			if (used[k])
			{
				BoostWrapper bb = others[k].GetBoostComponent();
				if (!bb.IsNull()) bb.SetUnlimitedBoostRefCount(0);
				continue;
			}
			server.RemoveCar(others[k]);
			if (!ai.IsNull()) server.RemovePlayer(ai);
		}
	};
	// Everything back to the start (frozen: nothing moving yet)
	auto placeStart = [&](bool frozen) {
		Pose q;
		if (CarAt(d, mpStart, mpMe, 0.0f, mpMirror, q))
		{
			Put(car, q, frozen);
			BoostWrapper bc = car.GetBoostComponent();
			if (!bc.IsNull()) { bc.SetUnlimitedBoostRefCount(0); bc.SetCurrentBoostAmount(q.boost / 100.0f); }
		}
		for (MpBot& b : mpBots)
		{
			CarWrapper c = findCar(b.addr);
			if (c.IsNull() || !CarAt(d, mpStart, b.player, 0.0f, mpMirror, q)) continue;
			c.SetInput(ControllerInput{});
			Put(c, q, frozen);
			BoostWrapper bb = c.GetBoostComponent();
			if (!bb.IsNull()) { bb.SetUnlimitedBoostRefCount(0); bb.SetCurrentBoostAmount(q.boost / 100.0f); }
		}
		Vector bp, bv;
		if (BallAt(d, mpStart, 0.0f, mpMirror, bp, bv))
		{
			ball.SetLocation(bp);
			ball.SetVelocity(frozen ? Vector(0, 0, 0) : bv);
			ball.SetAngularVelocity(Vector(0, 0, 0), false);
		}
	};
	const float handoff = std::max(0.3f, d.frames[mpMoment].time - d.frames[mpStart].time);

	// ---- Spawning: one bot every 0.3 s, then wait for them all to show up ----
	if (mpPhase == MpPhase::Spawning)
	{
		const int want = static_cast<int>(mpBots.size());
		if (mpSpawned < want)
		{
			if (mpPhaseTicks % kSpawnEvery == 1)
			{
				server.SetbRandomizedBotLoadouts(0);
				server.SpawnBot(23, "Road2 Bot");   // an Octane
				mpSpawned++;
			}
			placeStart(true);
			return;
		}
		const int others = server.GetCars().Count() - 1;
		placeStart(true);
		if (others < want && mpPhaseTicks < want * kSpawnEvery + kSpawnWaitTicks) return;
		gather();
		int found = 0;
		for (const MpBot& b : mpBots) if (b.addr) found++;
		if (found < want)
		{
			// Fewer bots than players: the ones without a car sit out, and the page says so
			mpBots.erase(std::remove_if(mpBots.begin(), mpBots.end(), [](const MpBot& b) { return b.addr == 0; }), mpBots.end());
			LOG("Road2 practise from here: only {} of {} bots showed up", found, want);
		}
		mpPhase = MpPhase::Frozen;
		mpPhaseTicks = 0;
		placeStart(true);
		MatchPracticePublish(found < want ? std::format("Only {} of {} bots showed up: the rest sit out. Move to start.", found, want) : std::string("Move to start"));
		return;
	}

	// ---- Frozen at the start until you move ----
	if (mpPhase == MpPhase::Frozen)
	{
		bool missing = false;
		for (const MpBot& b : mpBots) if (findCar(b.addr).IsNull()) missing = true;
		if (missing && mpPhaseTicks % 30 == 1) gather();   // after a free play reset: the new cars
		if (mpPhaseTicks == 2) server.ResetPickups();
		placeStart(true);
		if (mpPhaseTicks < kGraceTicks || !AnyInput(car.GetInput())) return;
		// Go: everything moving the way it really was
		server.ResetPickups();
		placeStart(false);
		mpPhase = MpPhase::Live;
		mpPhaseTicks = 0;
		mpLiveTicks = 0;
		mpBallFree = false;
		mpTouched = false;
		mpNextoTick = 0;
		mpCarPrev = car.GetLocation();
		{
			const int rp = mpStart < static_cast<int>(an.possTeam.size()) ? an.possTeam[mpStart] : -1;
			mpPoss = rp < 0 ? -1 : rp == mpTeam ? 0 : 1;
		}
		for (int i = 0; i < Nexto::kPads; i++) { mpPadsUp[i] = true; mpPadDown[i] = 0; }
		for (MpBot& b : mpBots) { b.nexto = false; b.prev = b.now = Nexto::Controls{}; b.queue.clear(); }
		mpTries++;
		MatchPracticePublish(std::string());
		return;
	}

	// ---- Live (and the end of a try playing on for a moment) ----
	mpLiveTicks++;
	const float t = mpLiveTicks / 120.0f;
	if (mpPhase == MpPhase::Live)
	{
		// Free play's reset (it teleports you to a kickoff spot): straight back to the start
		const Vector cl = car.GetLocation();
		const Vector jump = cl - mpCarPrev;
		mpCarPrev = cl;
		if (mpLiveTicks > 2 && jump.magnitude() > 400.0f)
		{
			mpPhase = MpPhase::Frozen;
			mpPhaseTicks = 0;
			MatchPracticePublish("Back to the start: move to go again");
			return;
		}
	}
	const Vector bl = ball.GetLocation();
	const Vector meLoc = car.GetLocation();
	// Each bot: its real path up to the moment, then Nexto (sooner if the ball or you get close to it)
	for (MpBot& b : mpBots)
	{
		CarWrapper c = findCar(b.addr);
		if (c.IsNull()) continue;
		if (!b.nexto)
		{
			const Vector cp = c.GetLocation();
			Pose q;
			const bool onPath = CarAt(d, mpStart, b.player, t, mpMirror, q);
			if (t >= handoff || !onPath || (cp - bl).magnitude() < 500.0f || (cp - meLoc).magnitude() < 250.0f || mpPhase == MpPhase::Result) b.nexto = true;
			else { c.SetInput(ControllerInput{}); Put(c, q, false); }
		}
	}
	// The ball on its real path until someone's about to touch it (or the moment's reached)
	if (!mpBallFree)
	{
		bool nearSomeone = (meLoc - bl).magnitude() < 92.75f + 150.0f;
		for (const MpBot& b : mpBots) { CarWrapper c = findCar(b.addr); if (b.nexto && !c.IsNull() && (c.GetLocation() - bl).magnitude() < 92.75f + 150.0f) nearSomeone = true; }
		Vector bp, bv;
		if (mpTouched || nearSomeone || t >= handoff || !BallAt(d, mpStart, t, mpMirror, bp, bv)) mpBallFree = true;
		else { ball.SetLocation(bp); ball.SetVelocity(bv); }
	}
	// Nexto: the pads (down while a car that isn't full drives over them), then a decision per bot every 8 ticks
	// (staggered so they don't all think on the same tick), on the car 6 ticks after it looked
	{
		std::vector<CarWrapper> all{ car };
		for (const MpBot& b : mpBots) { CarWrapper c = findCar(b.addr); if (!c.IsNull()) all.push_back(c); }
		for (int i = 0; i < Nexto::kPads; i++) if (!mpPadsUp[i] && --mpPadDown[i] <= 0) mpPadsUp[i] = true;
		for (CarWrapper& c : all)
		{
			const Vector p = c.GetLocation();
			BoostWrapper bb = c.GetBoostComponent();
			if (bb.IsNull() || bb.GetCurrentBoostAmount() >= 0.999f || p.Z > 200.0f) continue;
			const int pad = Nexto::PadAt(p.X, p.Y);
			if (pad < 0 || !mpPadsUp[pad]) continue;
			const bool big = Nexto::kPadPos[pad][2] > 72.0f;
			const float dx = p.X - Nexto::kPadPos[pad][0], dy = p.Y - Nexto::kPadPos[pad][1];
			if (dx * dx + dy * dy > (big ? 208.0f * 208.0f : 144.0f * 144.0f)) continue;
			mpPadsUp[pad] = false;
			mpPadDown[pad] = big ? 1200 : 480;
		}
		Nexto::Ball nb;
		{
			const Vector bv = ball.GetVelocity(), ba = ball.GetAngularVelocity();
			const float bp[3] = { bl.X, bl.Y, bl.Z }, bvv[3] = { bv.X, bv.Y, bv.Z }, bav[3] = { ba.X, ba.Y, ba.Z };
			for (int k = 0; k < 3; k++) { nb.pos[k] = bp[k]; nb.vel[k] = bvv[k]; nb.ang[k] = bav[k]; }
		}
		for (size_t bi = 0; bi < mpBots.size(); bi++)
		{
			MpBot& b = mpBots[bi];
			if (!b.nexto) continue;
			CarWrapper self = findCar(b.addr);
			if (self.IsNull()) continue;
			if ((mpNextoTick + static_cast<int>(bi)) % Nexto::kTickSkip == 0)
			{
				std::vector<Nexto::Car> others{ NextoCar(car, 0) };
				for (const MpBot& o : mpBots)
				{
					if (&o == &b) continue;
					CarWrapper oc = findCar(o.addr);
					if (!oc.IsNull()) others.push_back(NextoCar(oc, o.team));
				}
				const int act = nexto.Act(NextoCar(self, b.team), others, nb, mpPadsUp, b.prev, 1.0f,
					static_cast<uint32_t>(mpTries * 7919 + bi * 104729 + mpNextoTick));
				const Nexto::Controls ctl = nexto.Action(act);
				b.prev = ctl;
				b.queue.push_back({ mpNextoTick + kNextoDelay, ctl });
			}
			while (!b.queue.empty() && b.queue.front().first <= mpNextoTick) { b.now = b.queue.front().second; b.queue.erase(b.queue.begin()); }
			self.SetInput(ToInput(b.now));
		}
		mpNextoTick++;
	}

	// The scoring chance, eight times a second, from a one-frame copy of the pitch
	if (mpLiveTicks % 15 == 1)
	{
		MatchData snap;
		MatchFrame f;
		f.hasBall = true;
		const Vector bv = ball.GetVelocity();
		f.bx = bl.X; f.by = bl.Y; f.bz = bl.Z; f.bvx = bv.X; f.bvy = bv.Y; f.bvz = bv.Z;
		auto add = [&](CarWrapper& c, int team) {
			MatchPlayer mp;
			mp.id = static_cast<int>(snap.players.size());
			mp.team = team;
			snap.players.push_back(mp);
			MatchCar mc;
			mc.present = true;
			const Vector p = c.GetLocation(), v = c.GetVelocity();
			mc.x = p.X; mc.y = p.Y; mc.z = p.Z; mc.vx = v.X; mc.vy = v.Y; mc.vz = v.Z;
			BoostWrapper bb = c.GetBoostComponent();
			mc.boost = bb.IsNull() ? 0.0f : bb.GetCurrentBoostAmount() * 100.0f;
			f.cars.push_back(mc);
		};
		add(car, 0);
		for (const MpBot& b : mpBots) { CarWrapper c = findCar(b.addr); if (!c.IsNull()) add(c, b.team); }
		snap.frames.push_back(f);
		for (int tm = 0; tm < 2; tm++)
		{
			float fe[Match::kChanceFeatures];
			mpChance[tm] = Match::ChanceFeatures(snap, 0, tm, mpPoss, fe) ? Match::Chance(fe) : 0.0f;
		}
	}

	if (mpPhase == MpPhase::Result)
	{
		if (mpPhaseTicks < kResultTicks) return;
		// a list: on to the next moment (the bots stay), or done
		if (mpListPos >= 0 && mpListPos + 1 < static_cast<int>(mpList.size()))
		{
			mpListPos++;
			const MpItem next = mpList[mpListPos];
			MatchPracticeStart(next.frameIndex, next.player);
			return;
		}
		if (mpListPos >= 0 && mpListPos + 1 >= static_cast<int>(mpList.size()))
		{
			int better = 0, same = 0, worse = 0;
			for (int g : mpListGood) { if (g == 1) better++; else if (g == 0) same++; else if (g == -1) worse++; }
			mpPhase = MpPhase::Frozen;
			mpPhaseTicks = 0;
			mpListPos = static_cast<int>(mpList.size());   // done (the results stay on the page)
			MatchPracticePublish(std::format("List done: {} better than the game, {} about the same, {} worse. Move to try the last one again.", better, same, worse));
			return;
		}
		mpPhase = MpPhase::Frozen;
		mpPhaseTicks = 0;
		MatchPracticePublish("Move to go again");
		return;
	}

	// ---- The end of a try: a goal, or six seconds after the moment ----
	const bool inTheirs = bl.Y >= 5120.0f + 92.75f && std::fabs(bl.X) < 893.0f && bl.Z < 643.0f;   // you're blue: you score at +Y
	const bool inYours = bl.Y <= -5120.0f - 92.75f && std::fabs(bl.X) < 893.0f && bl.Z < 643.0f;
	if (!inTheirs && !inYours && t < handoff + kAfterSecs) return;
	// What happened in the game by the same point (or its goal, if one came first)
	const int n = static_cast<int>(d.frames.size());
	const float T = d.frames[mpStart].time + t;
	int ri = mpStart, realGoal = 0;
	while (ri + 1 < n && d.frames[ri + 1].time <= T)
	{
		ri++;
		if (d.frames[ri].score[mpTeam] > d.frames[ri - 1].score[mpTeam]) { realGoal = 1; break; }
		if (d.frames[ri].score[1 - mpTeam] > d.frames[ri - 1].score[1 - mpTeam]) { realGoal = -1; break; }
	}
	const float realOwn = ri < static_cast<int>(an.chance[mpTeam].size()) ? an.chance[mpTeam][ri] : 0.0f;
	const float realOpp = ri < static_cast<int>(an.chance[1 - mpTeam].size()) ? an.chance[1 - mpTeam][ri] : 0.0f;
	const int tryGoal = inTheirs ? 1 : inYours ? -1 : 0;
	const float tryV = tryGoal != 0 ? static_cast<float>(tryGoal) : mpChance[0] - mpChance[1];
	const float realV = realGoal != 0 ? static_cast<float>(realGoal) : realOwn - realOpp;
	const float secs = std::max(0.0f, t - handoff);
	mpResult = tryGoal > 0 ? "Goal!" : tryGoal < 0 ? "They scored"
		: std::format("Your chance {:.0f}%, theirs {:.0f}%", 100.0f * mpChance[0], 100.0f * mpChance[1]);
	mpReal = realGoal > 0 ? "In the game: your team scored" : realGoal < 0 ? "In the game: they scored"
		: std::format("In the game {:.0f} s on: your chance {:.0f}%, theirs {:.0f}%", secs, 100.0f * realOwn, 100.0f * realOpp);
	mpResultGood = tryV - realV > 0.1f ? 1 : tryV - realV < -0.1f ? -1 : 0;
	if (mpListPos >= 0 && mpListPos < static_cast<int>(mpListGood.size()) && mpListGood[mpListPos] == -2)
	{
		mpListGood[mpListPos] = mpResultGood;   // the first finished try is the one that counts
		mpListResult[mpListPos] = mpResult;
	}
	mpPhase = MpPhase::Result;
	mpPhaseTicks = 0;
	MatchPracticePublish(std::string());
}

void RLCoach::DrawMatchPracticeHud(CanvasWrapper& canvas)
{
	if (mpPhase == MpPhase::Off || !gameWrapper->IsInFreeplay()) return;
	const Vector2 sz = canvas.GetSize();
	std::vector<std::string> top{ "PRACTISE FROM HERE", Upper(mpLabel) };
	if (mpListPos >= 0 && mpListPos < static_cast<int>(mpList.size())) top.push_back(std::format("{} OF {}", mpListPos + 1, mpList.size()));
	else if (mpTries > 0) top.push_back(std::format("TRY {}", mpTries));
	SlantRow(canvas, sz.X * 0.5f, sz.Y * 0.07f, top, 1.6f, kBlueA, kWhiteA, 10.0f);
	auto big = [&](const std::string& a, const std::string& b, HudDraw::Rgba fill, HudDraw::Rgba ink) {
		std::vector<std::string> parts{ Upper(a) };
		if (!b.empty()) parts.push_back(Upper(b));
		SlantRow(canvas, sz.X * 0.5f, sz.Y * 0.125f, parts, 2.2f, fill, ink, 14.0f);
	};
	if (mpPhase == MpPhase::Spawning) { big("Setting up", std::format("{} bot{}", mpBots.size(), mpBots.size() == 1 ? "" : "s"), kNavyA, kWhiteA); return; }
	const bool cb = HudDraw::gColorBlind;
	const HudDraw::Rgba good = cb ? HudDraw::Rgba{ 86, 180, 233, 255 } : kGoodA, bad = cb ? HudDraw::Rgba{ 204, 121, 167, 255 } : kBadA,
		even = cb ? HudDraw::Rgba{ 240, 228, 66, 255 } : kWarnA;
	if (mpPhase == MpPhase::Frozen)
	{
		if (!mpResult.empty()) big(mpResult, mpReal, mpResultGood > 0 ? good : mpResultGood < 0 ? bad : even, kInkA);
		if (mpTries == 0) SlantRow(canvas, sz.X * 0.5f, sz.Y * 0.205f, { "MOVE TO START", "WHAT REALLY HAPPENED, THEN NEXTO" }, 2.0f, kOrangeA, kInkA, 18.0f);
		else SlantRow(canvas, sz.X * 0.5f, sz.Y * 0.205f, { "MOVE TO GO AGAIN" }, 2.0f, kOrangeA, kInkA, 18.0f);
		return;
	}
	if (mpPhase == MpPhase::Result) { big(mpResult, mpReal, mpResultGood > 0 ? good : mpResultGood < 0 ? bad : even, kInkA); return; }
	// Live: the chance as it stands
	SlantRow(canvas, sz.X * 0.5f, sz.Y * 0.125f,
		{ std::format("YOUR CHANCE {:.0f}%", 100.0f * mpChance[0]), std::format("THEIRS {:.0f}%", 100.0f * mpChance[1]) }, 1.6f, kBlueA, kWhiteA, 10.0f);
}
