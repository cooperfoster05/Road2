// Match review, step 1 (Nov 11): scanning a replay.
//
// You open a replay in Rocket League's own replay viewer; Road2 plays it from the start and writes down every car and
// the ball at every replay frame (replays are recorded 30 times a second), plus what the game itself counts per
// player (touches, shots, saves, goals, score). That goes into one file, data\Road2\matches\<name>.csv, and the
// timeline, grades and "practice from here" are all built from that file, not from the replay again.
//
// Nothing here runs outside a replay, and it only reads: it never changes the replay.
#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"
#include "UiFonts.h"

#include <chrono>
#include <fstream>
#include <functional>

namespace
{
	// Letters, digits, - and _ only, so it's safe as a file name
	std::string SafeName(const std::string& in)
	{
		std::string out;
		for (char ch : in)
			if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') out += ch;
			else if (ch == ' ' && !out.empty() && out.back() != '_') out += '_';
		if (out.size() > 60) out.resize(60);
		return out.empty() ? std::string("replay") : out;
	}
	// A font for a scope (null = keep the current one)
	struct FontPush
	{
		bool on;
		explicit FontPush(ImFont* f) : on(f != nullptr) { if (on) ImGui::PushFont(f); }
		~FontPush() { if (on) ImGui::PopFont(); }
	};
	std::string Upper(std::string s) { for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return s; }
	// A player's name in a CSV cell: no commas or line breaks
	std::string CsvName(std::string s)
	{
		for (char& ch : s) if (ch == ',' || ch == '\r' || ch == '\n') ch = ' ';
		return s;
	}
}

void RLCoach::MatchPublish()
{
	std::lock_guard<std::mutex> lock(uiMatchMutex);
	uiMatch.inReplay = matchInReplay;
	uiMatch.scanning = matchScanning;
	uiMatch.frame = matchLastFrame;
	uiMatch.frames = matchTotalFrames;
	uiMatch.captured = matchCaptured;
	uiMatch.gaps = matchGaps;
	uiMatch.rate = matchRate;
	uiMatch.status = matchStatus;
	uiMatch.replayName = matchReplayName;
	uiMatch.players = matchPlayers;
	uiMatch.savedFile = matchSavedFile;
}

// Starts the scan: the replay goes back to its first frame and plays through
void RLCoach::MatchScanStart()
{
	if (!writer || !gameWrapper->IsInReplay()) { matchStatus = "Open a replay first"; MatchPublish(); return; }
	ReplayServerWrapper server = gameWrapper->GetGameEventAsReplay();
	if (server.IsNull()) { matchStatus = "Couldn't reach the replay"; MatchPublish(); return; }
	ReplayWrapper replay = server.GetReplay();
	matchTotalFrames = replay.IsNull() ? 0 : replay.GetNumFrames();
	matchReplayName = replay.IsNull() ? std::string() : replay.GetReplayName().ToString();
	if (matchReplayName.empty() && !replay.IsNull()) matchReplayName = replay.GetId().ToString();
	matchRows.clear();
	matchRows.reserve(8u * 1024u * 1024u);
	matchRows += "# Road2 match scan v1\n";
	matchRows += std::format("# replay,{}\n# frames,{}\n# fps,{}\n", CsvName(matchReplayName), matchTotalFrames, server.GetReplayFPS());
	if (!replay.IsNull())
	{
		matchRows += std::format("# file,{}\n", CsvName(replay.GetFilePath().ToString()));
		matchRows += std::format("# me,{}\n", CsvName(replay.GetPlayerName().ToString()));
	}
	matchRows += "# rows: P = a player seen for the first time (id, team, name)\n";
	matchRows += "# rows: G = the game at a frame (frame, replay time, seconds left, overtime, blue score, orange score)\n";
	matchRows += "# rows: B = the ball (frame, x,y,z, vx,vy,vz, avx,avy,avz)\n";
	matchRows += "# rows: C = a car (frame, id, x,y,z, pitch,yaw,roll, vx,vy,vz, avx,avy,avz, boost 0-100, on ground, supersonic, jumped, double jumped, throttle, steer, handbrake)\n";
	matchRows += "# rows: S = a player's counters when one changed (frame, id, touches, shots, saves, goals, assists, score)\n";
	matchRows += "# rows: T = a ball touch the game reported (frame, id)\n";
	matchPlayers.clear();
	matchPlayerIds.clear();
	matchCounters.clear();
	matchCaptured = matchGaps = 0;
	matchLastFrame = -1;
	matchStall = 0;
	matchSavedFile.clear();
	matchScanning = true;
	matchScanBegan = std::chrono::steady_clock::now();
	matchRate = 0.0f;
	matchStatus = "Scanning: the replay plays through once";
	matchPausedNote = false;
	// (Nov 13) back to the start the way the replay's own scrub bar does it. StartPlaybackAtFrame restarted the
	// replay underneath the replay viewer, which could leave it broken: no cars, no play, no scrubbing.
	ReplaySeek(0);
	MatchPublish();
}

void RLCoach::MatchScanStop(bool save)
{
	if (!matchScanning) return;
	matchScanning = false;
	if (gameWrapper->IsInReplay())
	{
		ReplayServerWrapper server = gameWrapper->GetGameEventAsReplay();
		if (!server.IsNull() && *matchScanSpeed > 1.01f) server.SetGameSpeed(1.0f);
	}
	if (!save || matchCaptured < 30 || !writer)
	{
		matchStatus = save ? "Not enough of the replay was read to save" : "Stopped";
		matchRows.clear();
		MatchPublish();
		return;
	}
	std::error_code ec;
	const std::filesystem::path dir = writer->Folder() / "matches";
	std::filesystem::create_directories(dir, ec);
	const std::string name = SafeName(matchReplayName) + ".csv";
	std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
	if (out) out.write(matchRows.data(), static_cast<std::streamsize>(matchRows.size()));
	const bool ok = static_cast<bool>(out);
	out.close();
	const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - matchScanBegan).count();
	matchSavedFile = ok ? name : std::string();
	matchStatus = ok ? std::format("Read {} of {} frames in {:.0f} s ({} skipped). Saved to matches\\{}", matchCaptured, matchTotalFrames, secs, matchGaps, name)
		: "Couldn't write the match file";
	LOG("Road2 match scan: {}", matchStatus);
	matchRows.clear();
	matchRows.shrink_to_fit();
	MatchPublish();
	if (ok) MatchLoad(name);   // straight into the breakdown
}

// Every rendered frame (from the HUD's draw call). In a replay: notes that we're in one; while scanning, writes down
// each new replay frame.
void RLCoach::MatchSample()
{
	const bool in = gameWrapper->IsInReplay();
	if (in != matchInReplay)
	{
		matchInReplay = in;
		matchOpenReplayName.clear();
		{
			std::lock_guard<std::mutex> lock(uiMatchMutex);
			uiMatchOpenReplay.clear();
		}
		if (!in && matchScanning) MatchScanStop(true);   // the replay was closed: keep what was read
		MatchPublish();
	}
	if (!in) { uiMatchNowFrame = -1; return; }
	ReplayServerWrapper server = gameWrapper->GetGameEventAsReplay();
	if (server.IsNull()) return;
	const int frame = server.GetCurrentReplayFrame();
	uiMatchNowFrame = frame;
	if (matchOpenReplayName.empty())   // which replay this is, once (so the page knows if it's the one it's showing)
	{
		ReplayWrapper replay = server.GetReplay();
		if (!replay.IsNull())
		{
			matchOpenReplayName = replay.GetReplayName().ToString();
			if (matchOpenReplayName.empty()) matchOpenReplayName = replay.GetId().ToString();
			std::lock_guard<std::mutex> lock(uiMatchMutex);
			uiMatchOpenReplay = matchOpenReplayName;
		}
	}
	if (!matchScanning) return;
	if (frame == matchLastFrame)
	{
		// not moving: paused, or the end. Two seconds of that at the last frames means it's done.
		if (++matchStall > 240 && matchTotalFrames > 0 && frame >= matchTotalFrames - 5) MatchScanStop(true);
		else if (matchStall == 180 && !matchPausedNote)
		{
			matchPausedNote = true;
			matchStatus = "Paused: press play in the replay to keep scanning";
			MatchPublish();
		}
		return;
	}
	matchStall = 0;
	if (matchPausedNote) { matchPausedNote = false; matchStatus = "Scanning: the replay plays through once"; MatchPublish(); }
	if (frame < matchLastFrame) return;   // jumped back (you scrubbed): wait for it to come forward again
	if (matchLastFrame >= 0 && frame > matchLastFrame + 1) matchGaps += frame - matchLastFrame - 1;
	matchLastFrame = frame;
	if (*matchScanSpeed > 1.01f && matchCaptured % 30 == 0) server.SetGameSpeed(*matchScanSpeed);   // faster than real time, if the replay allows it

	// the game
	int score[2] = { 0, 0 };
	{
		ArrayWrapper<TeamWrapper> teams = server.GetTeams();
		for (int i = 0; i < teams.Count() && i < 2; i++) { TeamWrapper t = teams.Get(i); if (!t.IsNull()) score[i] = t.GetScore(); }
	}
	matchRows += std::format("G,{},{:.3f},{},{},{},{}\n", frame, server.GetReplayTimeElapsed(), server.GetSecondsRemaining(), server.GetbOverTime() ? 1 : 0, score[0], score[1]);
	// the ball
	{
		BallWrapper ball = server.GetBall();
		if (!ball.IsNull())
		{
			const Vector p = ball.GetLocation(), v = ball.GetVelocity(), a = ball.GetAngularVelocity();
			matchRows += std::format("B,{},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.2f},{:.2f},{:.2f}\n", frame, p.X, p.Y, p.Z, v.X, v.Y, v.Z, a.X, a.Y, a.Z);
		}
	}
	// every car (a demolished car is simply missing for a while)
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	for (int i = 0; i < cars.Count(); i++)
	{
		CarWrapper car = cars.Get(i);
		if (car.IsNull()) continue;
		PriWrapper pri = car.GetPRI();
		if (pri.IsNull()) continue;
		const std::string name = pri.GetPlayerName().ToString();
		int id;
		auto known = matchPlayerIds.find(name);
		if (known == matchPlayerIds.end())
		{
			id = static_cast<int>(matchPlayerIds.size());
			matchPlayerIds[name] = id;
			const int team = pri.GetTeamNum2();
			matchPlayers.push_back(std::format("{}{}", team == 0 ? "Blue: " : team == 1 ? "Orange: " : "", name));
			matchRows += std::format("P,{},{},{}\n", id, team, CsvName(name));
			matchCounters.push_back({});
		}
		else id = known->second;
		const Vector p = car.GetLocation(), v = car.GetVelocity(), a = car.GetAngularVelocity();
		const Rotator r = car.GetRotation();
		BoostWrapper bc = car.GetBoostComponent();
		const float boost = bc.IsNull() ? -1.0f : bc.GetCurrentBoostAmount() * 100.0f;
		matchRows += std::format("C,{},{},{:.1f},{:.1f},{:.1f},{},{},{},{:.1f},{:.1f},{:.1f},{:.2f},{:.2f},{:.2f},{:.0f},{},{},{},{},{},{},{}\n",
			frame, id, p.X, p.Y, p.Z, r.Pitch, r.Yaw, r.Roll, v.X, v.Y, v.Z, a.X, a.Y, a.Z, boost,
			car.IsOnGround() ? 1 : 0, car.GetbSuperSonic() ? 1 : 0, car.GetbJumped() ? 1 : 0, car.GetbDoubleJumped() ? 1 : 0,
			static_cast<int>(car.GetReplicatedThrottle()), static_cast<int>(car.GetReplicatedSteer()), car.GetbReplicatedHandbrake() ? 1 : 0);
		// what the game counts for this player: a row whenever any of it changes
		const std::array<int, 6> now = { pri.GetBallTouches(), pri.GetMatchShots(), pri.GetMatchSaves(), pri.GetMatchGoals(), pri.GetMatchAssists(), pri.GetMatchScore() };
		if (id < static_cast<int>(matchCounters.size()) && now != matchCounters[id])
		{
			matchCounters[id] = now;
			matchRows += std::format("S,{},{},{},{},{},{},{},{}\n", frame, id, now[0], now[1], now[2], now[3], now[4], now[5]);
		}
	}
	matchCaptured++;
	if (matchCaptured % 15 == 0)
	{
		const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - matchScanBegan).count();
		matchRate = secs > 0.5f ? matchCaptured / secs : 0.0f;
		MatchPublish();
	}
	if (matchTotalFrames > 0 && frame >= matchTotalFrames - 2) MatchScanStop(true);
}

// A ball touch the game reports while a replay is being scanned (if it reports them in replays at all)
void RLCoach::MatchOnTouch(CarWrapper& car)
{
	if (!matchScanning || car.IsNull()) return;
	PriWrapper pri = car.GetPRI();
	if (pri.IsNull()) return;
	auto known = matchPlayerIds.find(pri.GetPlayerName().ToString());
	if (known != matchPlayerIds.end()) matchRows += std::format("T,{},{}\n", matchLastFrame, known->second);
}

// Reads a scan and works out the breakdown, off the game and render threads (a few hundredths of a second)
void RLCoach::MatchLoad(const std::string& file)
{
	if (!writer || matchLoadBusy.exchange(true)) return;
	if (matchLoadThread.joinable()) matchLoadThread.join();
	const std::filesystem::path path = writer->Folder() / "matches" / file;
	matchLoadThread = std::thread([this, path, file]() {
		std::shared_ptr<MatchAnalysis> a;
		std::shared_ptr<MatchData> data = std::make_shared<MatchData>();
		std::string err;
		try
		{
			if (Match::Load(path, *data, err))
			{
				a = std::make_shared<MatchAnalysis>(Match::Analyze(*data));
				if (!a->ok) { err = a->error; a.reset(); }
			}
		}
		catch (const std::exception& e) { err = e.what(); a.reset(); }
		{
			std::lock_guard<std::mutex> lock(uiMatchMutex);
			uiMatchAnalysis = a;
			uiMatchData = a ? data : nullptr;
			uiMatchLoadedFile = a ? file : std::string();
			uiMatchLoadError = err;
		}
		matchLoadBusy = false;
	});
}

// Moves the open replay to a moment (a second and a half before it, so you see it coming)
void RLCoach::MatchJump(int frame)
{
	ReplaySeek(std::max(0, frame - 33));
}

// Moves the open replay to a frame. (Nov 13) The first version called the replay server's SkipToFrame directly, and
// Coop found it could break the replay: the cars vanished and play and the scrub bar stopped working. The replay
// viewer (the game's own replay screen) keeps its own state for a skip (paused for the scrub, the frame it's
// heading to), and a skip from underneath it left that state stale. So by default the skip goes through the viewer,
// like dragging its scrub bar. road2_match_seek_mode picks another way if that one misbehaves:
//   0  the replay viewer's own move to a frame (AdvanceToFrame)
//   1  the replay's skip with a full reload of what's on the pitch (what the viewer does after a scrub)
//   2  the old way (the replay server's SkipToFrame)
bool RLCoach::ReplaySeek(int frame)
{
	if (!gameWrapper->IsInReplay()) return false;
	ReplayServerWrapper server = gameWrapper->GetGameEventAsReplay();
	if (server.IsNull()) return false;
	const int mode = *matchSeekMode;
	if (mode == 0)
	{
		PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
		SpectatorHUDWrapper hud = pc.IsNull() ? SpectatorHUDWrapper(0) : pc.GetSpectatorHud();
		ReplayViewerDataWrapper viewer = hud.IsNull() ? ReplayViewerDataWrapper(0) : hud.GetViewerData();
		if (!viewer.IsNull())
		{
			LOG("Road2 replay seek: viewer to frame {} (was {}, pending {}, paused for scrub {})", frame, viewer.GetCurrentFrame(), viewer.GetPendingSkipToFrame(), viewer.GetbPausedForScrub());
			viewer.AdvanceToFrame(frame);
			return true;
		}
		LOG("Road2 replay seek: no replay viewer, using the replay's skip");
	}
	if (mode <= 1)
	{
		ReplayWrapper replay = server.GetReplay();
		if (!replay.IsNull()) { replay.SkipToFrame(frame, 1); return true; }
	}
	server.SkipToFrame(frame);
	return true;
}

// ---- The breakdown of a scanned replay ----
void RLCoach::RenderMatchReview(const MatchAnalysis& a, const std::shared_ptr<const MatchData>& data, bool sameReplayOpen)
{
	using namespace UiStyle;
	const float S = Sc();
	ImFont* fHead = uiFonts[UiFonts::Head];
	ImFont* fBig = uiFonts[UiFonts::Big];
	ImFont* fLabel = uiFonts[UiFonts::Label];
	ImFont* fDisplay = uiFonts[UiFonts::Display];
	ImFont* fCapsF = gCaps ? gCaps : ImGui::GetFont();
	auto onGame = [this](std::function<void()> fn) { gameWrapper->Execute([fn](GameWrapper*) { fn(); }); };
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float W = ImGui::GetContentRegionAvail().x;
	const int P = static_cast<int>(a.players.size());
	const ImVec4 kBlueT = kAccent, kOrangeT = kOrange;
	auto teamCol = [&](int team) { return team == 0 ? kBlueT : team == 1 ? kOrangeT : kMuted; };
	auto kindCol = [&](MomentKind k) {
		switch (k)
		{
		case MomentKind::Goal: return kGold;
		case MomentKind::Shot: return Hex(0x7DB8FF);
		case MomentKind::Save: return kGood;
		case MomentKind::Assist: return kGold;
		case MomentKind::Fifty: return kWarn;
		case MomentKind::Pass: return Hex(0x2DE2E6);
		case MomentKind::Clear: return Hex(0x9FB3D9);
		case MomentKind::Giveaway: return kBad;
		case MomentKind::Demo: return kBad;
		case MomentKind::Kickoff: return kText;
		case MomentKind::DoubleCommit: return kWarn;
		case MomentKind::Beaten: return kBad;
		case MomentKind::OpenNet: return kBad;
		case MomentKind::CutRotation: return kWarn;
		case MomentKind::BallSide: return kMuted;
		case MomentKind::Play: return kGold;
		case MomentKind::NoFollowUp: return kWarn;
		case MomentKind::TookMateBoost: return kWarn;
		case MomentKind::Shadow: return kGood;
		case MomentKind::Aerial: return Hex(0x7DB8FF);
		default: return kMuted;
		}
	};
	// which moments a filter shows: 0 all, 1 mine, 2 shots saves goals, 3 the ball, 4 boost, 5 the ones behind a
	// takeaway, 6 rotation
	std::vector<char> picked(a.moments.size(), 0);
	for (int k : uiMatchPick) if (k >= 0 && k < static_cast<int>(picked.size())) picked[k] = 1;
	if (uiMatchFilter == 5 && uiMatchPick.empty()) uiMatchFilter = 0;
	auto shown = [&](const Moment& m) {
		switch (uiMatchFilter)
		{
		case 5: return picked[static_cast<size_t>(&m - a.moments.data())] != 0;
		case 6: return m.kind == MomentKind::DoubleCommit || m.kind == MomentKind::Beaten || m.kind == MomentKind::OpenNet || m.kind == MomentKind::LeftForBoost
			|| m.kind == MomentKind::CutRotation || m.kind == MomentKind::BallSide || m.kind == MomentKind::NoFollowUp || m.kind == MomentKind::Shadow;
		case 1: return m.player == uiMatchMe || m.other == uiMatchMe;
		case 2: return m.kind == MomentKind::Goal || m.kind == MomentKind::Shot || m.kind == MomentKind::Save || m.kind == MomentKind::Assist;
		case 3: return m.kind == MomentKind::Kickoff || m.kind == MomentKind::Fifty || m.kind == MomentKind::Pass || m.kind == MomentKind::Clear || m.kind == MomentKind::Giveaway || m.kind == MomentKind::Aerial
			|| m.kind == MomentKind::Play;
		case 7: return m.kind == MomentKind::Play || m.kind == MomentKind::Goal;
		case 4: return m.kind == MomentKind::NoBoost || m.kind == MomentKind::WastedBoost || m.kind == MomentKind::LeftForBoost || m.kind == MomentKind::TookMateBoost;
		default: return true;
		}
	};
	auto jump = [&](int momentIndex) {
		uiMatchSel = momentIndex;
		if (sameReplayOpen) { const int frame = a.moments[momentIndex].frame; onGame([this, frame]() { MatchJump(frame); }); }
	};

	// ---- The scoreline: a slanted block per team, the score big ----
	{
		const ImVec2 o = ImGui::GetCursorScreenPos();
		const float h = 62.0f * S, gap = 8.0f * S, bw = (W - gap) * 0.5f;
		ImFont* hf = gHero ? gHero : ImGui::GetFont();
		for (int t = 0; t < 2; t++)
		{
			const ImVec2 p0(o.x + t * (bw + gap), o.y), p1(p0.x + bw, o.y + h);
			const ImVec4 c = teamCol(t);
			SlantQuad(dl, p0, p1, U32(kPanel));
			SlantQuad(dl, t == 0 ? p0 : ImVec2(p1.x - 6.0f * S, p0.y), t == 0 ? ImVec2(p0.x + 6.0f * S, p1.y) : p1, U32(c));
			const std::string sc = std::to_string(a.score[t]);
			const float fs = h * 0.78f;
			const ImVec2 ss = hf->CalcTextSizeA(fs, 1e9f, 0.0f, sc.c_str());
			const float sx = t == 0 ? p1.x - ss.x - 18.0f * S : p0.x + 18.0f * S;
			dl->AddText(hf, fs, ImVec2(sx, o.y + (h - ss.y) * 0.5f), U32(c), sc.c_str());
			const char* tn = t == 0 ? "BLUE" : "ORANGE";
			const float ls = (fLabel ? fLabel->FontSize : 12.0f) * gUser;
			ImFont* lf = fLabel ? fLabel : ImGui::GetFont();
			const float tx = t == 0 ? p0.x + 20.0f * S : p1.x - 20.0f * S - lf->CalcTextSizeA(ls, 1e9f, 0.0f, tn).x;
			dl->AddText(lf, ls, ImVec2(tx, o.y + 9.0f * S), U32(kMuted), tn);
			const std::string pos = std::format("{:.0f}% OF THE BALL", a.possessionPct[t]);
			const float px = t == 0 ? p0.x + 18.0f * S : p1.x - 22.0f * S - fCapsF->CalcTextSizeA(fCapsF->FontSize * gUser, 1e9f, 0.0f, pos.c_str()).x;
			dl->AddText(fCapsF, fCapsF->FontSize * gUser, ImVec2(px, o.y + h - fCapsF->FontSize * gUser - 9.0f * S), U32(kSub), pos.c_str());
		}
		ImGui::Dummy(ImVec2(W, h + 2.0f * S));
		{
			FontPush f(fLabel);
			const int secs = static_cast<int>(a.seconds);
			ImGui::TextColored(kMuted, "%s", std::format("{}V{}   /   {}:{:02d} OF PLAY   /   {}", a.teamSize, a.teamSize, secs / 60, secs % 60, Upper(a.replayName)).c_str());
		}
	}
	ImGui::Spacing();

	// ---- Who you are: click a name. The timeline's boost line and the "mine" filter follow it. ----
	{
		const float gap = 6.0f * S;
		const int perRow = P <= 4 ? std::max(1, P) : (P + 1) / 2;
		const float cw = (W - gap * (perRow - 1)) / perRow, ch = 26.0f * S;
		const ImVec2 o = ImGui::GetCursorScreenPos();
		// blue first, then orange, so each row reads as a team when it can
		std::vector<int> order;
		for (int t = 0; t < 2; t++) for (int i = 0; i < P; i++) if (a.players[i].team == t) order.push_back(i);
		for (int i = 0; i < P; i++) if (a.players[i].team != 0 && a.players[i].team != 1) order.push_back(i);
		for (int k = 0; k < static_cast<int>(order.size()); k++)
		{
			const int pl = order[k];
			const ImVec2 p0(o.x + (k % perRow) * (cw + gap), o.y + (k / perRow) * (ch + gap)), p1(p0.x + cw, p0.y + ch);
			ImGui::SetCursorScreenPos(p0);
			ImGui::PushID(1000 + pl);
			if (ImGui::InvisibleButton("who", ImVec2(cw, ch))) uiMatchMe = pl;
			const bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			const bool me = pl == uiMatchMe;
			const ImVec4 c = teamCol(a.players[pl].team);
			SlantQuad(dl, p0, p1, U32(me ? c : hov ? kChip : kPanel2));
			if (!me) SlantQuad(dl, p0, ImVec2(p0.x + 4.0f * S, p1.y), U32(c));
			std::string nm = Upper(a.players[pl].name);
			const float fs = fCapsF->FontSize * gUser;
			while (nm.size() > 3 && fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, nm.c_str()).x > cw - 22.0f * S) nm.pop_back();
			dl->AddText(fCapsF, fs, ImVec2(p0.x + 12.0f * S, p0.y + (ch - fs) * 0.5f), U32(me ? OnFill(c) : kText), nm.c_str());
			if (hov) ImGui::SetTooltip("%s\nClick to follow this player.", a.players[pl].name.c_str());
		}
		const int rows = (static_cast<int>(order.size()) + perRow - 1) / perRow;
		ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rows * (ch + gap)));
		ImGui::Dummy(ImVec2(W, 2.0f * S));
	}

	// ---- The timeline: who had the ball, every moment (blue above the line, orange below), your boost ----
	{
		const ImVec2 o = ImGui::GetCursorScreenPos();
		const float padX = 10.0f * S, possH = 10.0f * S, laneH = 28.0f * S, boostH = 40.0f * S, gapY = 7.0f * S;
		const bool hasChance = !a.chance[0].empty() && a.chance[0].size() == a.frameTime.size();
		const float chanceH = hasChance ? 44.0f * S : 0.0f;
		const float h = 10.0f * S + possH + gapY + laneH * 2.0f + gapY + (hasChance ? chanceH + gapY : 0.0f) + boostH + 10.0f * S;
		const float x0 = o.x + padX, x1 = o.x + W - padX, whole = std::max(1.0f, a.t1 - a.t0);
		// (Nov 11) zoom: the wheel zooms in on where the mouse is, a drag moves along, a double-click shows it all
		uiMatchZoom = std::clamp(uiMatchZoom, 1.0f, 24.0f);
		const float span = whole / uiMatchZoom;
		uiMatchView0 = std::clamp(uiMatchView0, a.t0, a.t1 - span);
		const float v0 = uiMatchView0;
		auto X = [&](float t) { return x0 + (x1 - x0) * (t - v0) / span; };
		dl->AddRectFilled(o, ImVec2(o.x + W, o.y + h), U32(kPanel2), 6.0f * S);
		dl->PushClipRect(ImVec2(x0, o.y), ImVec2(x1, o.y + h), true);
		float y = o.y + 10.0f * S;
		// possession
		dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + possH), U32(kTrack));
		for (const PossessionSpan& s : a.possession)
			if (s.team == 0 || s.team == 1)
				if (s.t1 >= v0 && s.t0 <= v0 + span)
					dl->AddRectFilled(ImVec2(X(s.t0), y), ImVec2(std::max(X(s.t1), X(s.t0) + 1.0f), y + possH), U32(teamCol(s.team), s.player == uiMatchMe ? 1.0f : 0.55f));
		y += possH + gapY;
		// the two lanes of moments, a centre line between them
		const float laneTop = y, mid = y + laneH;
		dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), U32(kLine), 1.0f);
		int hovered = -1;
		float bestDist = 7.0f * S;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool inLanes = mouse.y >= laneTop - 2.0f && mouse.y <= laneTop + laneH * 2.0f + 2.0f && mouse.x >= x0 - 4.0f && mouse.x <= x1 + 4.0f;
		for (int i = 0; i < static_cast<int>(a.moments.size()); i++)
		{
			const Moment& m = a.moments[i];
			if (!shown(m)) continue;
			const float x = X(m.time);
			const bool mine = m.player == uiMatchMe || m.other == uiMatchMe;
			const bool goal = m.kind == MomentKind::Goal;
			const bool up = m.team == 0 || (m.team < 0 && (i % 2) == 0);
			const float len = goal ? laneH : mine ? laneH * 0.78f : laneH * 0.45f;
			if (x < x0 - 6.0f || x > x1 + 6.0f) continue;
			const float w = (goal ? 4.0f : mine ? 3.0f : 2.0f) * S * std::min(2.5f, 1.0f + (uiMatchZoom - 1.0f) * 0.25f);   // wider as you zoom in
			const ImVec2 p0(x - w * 0.5f, up ? mid - len : mid + 1.0f), p1(x + w * 0.5f, up ? mid - 1.0f : mid + len);
			const ImVec4 mc = m.kind != MomentKind::Play ? kindCol(m.kind) : m.detail == MomentDetail::PlayCalculated ? kGold : m.detail == MomentDetail::PlayGreat ? kGood
				: m.detail == MomentDetail::PlayWhoops ? kWarn : kBad;
			dl->AddRectFilled(p0, p1, U32(mc, mine || goal ? 1.0f : 0.6f));
			if (i == uiMatchSel) dl->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f), ImVec2(p1.x + 2.0f, p1.y + 2.0f), U32(kText), 0.0f, 0, 1.5f);
			if (inLanes && ((up && mouse.y <= mid) || (!up && mouse.y >= mid) || goal))
			{
				const float dx = std::fabs(mouse.x - x);
				if (dx < bestDist) { bestDist = dx; hovered = i; }
			}
		}
		y += laneH * 2.0f + gapY;
		// (Nov 13) the scoring chance: blue's chance of scoring in the next ten seconds up from the middle, orange's down
		if (hasChance)
		{
			const float cm = y + chanceH * 0.5f, half = chanceH * 0.5f - 1.0f * S;
			dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + chanceH), U32(kPanel));
			for (float g : { 0.25f, 0.5f })   // faint marks at 25% and 50%
			{
				dl->AddLine(ImVec2(x0, cm - half * g), ImVec2(x1, cm - half * g), U32(kLine, 0.35f), 1.0f);
				dl->AddLine(ImVec2(x0, cm + half * g), ImVec2(x1, cm + half * g), U32(kLine, 0.35f), 1.0f);
			}
			dl->AddLine(ImVec2(x0, cm), ImVec2(x1, cm), U32(kLine), 1.0f);
			const int n = static_cast<int>(a.frameTime.size());
			const int step = std::max(1, static_cast<int>(n / std::max(1.0f, (x1 - x0) / S * uiMatchZoom)));   // about one point a pixel
			for (int tm = 0; tm < 2; tm++)
			{
				const std::vector<float>& c = a.chance[tm];
				const float dir = tm == 0 ? -1.0f : 1.0f;
				const ImVec4 col = teamCol(tm);
				int prev = -1;
				for (int i = 0; i < n; i += step)
				{
					if (prev >= 0)
					{
						const float xa = X(a.frameTime[prev]), xb = X(a.frameTime[i]);
						if (xb >= x0 - 2.0f && xa <= x1 + 2.0f && xb > xa)
						{
							const float ya = cm + dir * half * std::min(1.0f, c[prev]), yb = cm + dir * half * std::min(1.0f, c[i]);
							const ImVec2 q[4] = { ImVec2(xa, cm), ImVec2(xa, ya), ImVec2(xb, yb), ImVec2(xb, cm) };
							dl->AddConvexPolyFilled(q, 4, U32(col, 0.28f));
							dl->AddLine(ImVec2(xa, ya), ImVec2(xb, yb), U32(col), 1.2f * S);
						}
					}
					prev = i;
				}
			}
			{
				const float ls = (fLabel ? fLabel->FontSize : 12.0f) * gUser * 0.85f;
				dl->AddText(fLabel ? fLabel : ImGui::GetFont(), ls, ImVec2(x0 + 4.0f * S, y + 2.0f * S), U32(kMuted), "SCORING CHANCE");
			}
			// hover: both teams' chance at that moment
			const ImVec2 mp = ImGui::GetIO().MousePos;
			if (mp.y >= y && mp.y <= y + chanceH && mp.x >= x0 && mp.x <= x1 && n > 0)
			{
				const float t = v0 + span * (mp.x - x0) / (x1 - x0);
				const int i = std::clamp(static_cast<int>(std::lower_bound(a.frameTime.begin(), a.frameTime.end(), t) - a.frameTime.begin()), 0, n - 1);
				dl->AddLine(ImVec2(mp.x, y), ImVec2(mp.x, y + chanceH), U32(kText, 0.5f), 1.0f);
				uiMatchChanceTip = std::format("Chance of a goal in the next 10 s\nBlue {:.0f}%   Orange {:.0f}%", 100.0f * a.chance[0][i], 100.0f * a.chance[1][i]);
			}
			y += chanceH + gapY;
		}
		// your boost
		dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + boostH), U32(kPanel));
		if (uiMatchMe >= 0 && uiMatchMe < static_cast<int>(a.boost.size()))
		{
			const std::vector<float>& b = a.boost[uiMatchMe];
			const ImVec4 c = teamCol(a.players[uiMatchMe].team);
			for (size_t i = 1; i < b.size() && i < a.sampleTime.size(); i++)
			{
				if (b[i] < 0 || b[i - 1] < 0) continue;
				const float xa = X(a.sampleTime[i - 1]), xb = X(a.sampleTime[i]);
				const float ya = y + boostH - boostH * b[i - 1] / 100.0f, yb = y + boostH - boostH * b[i] / 100.0f;
				// filled under the line, red where it's empty
				const ImVec2 q[4] = { ImVec2(xa, y + boostH), ImVec2(xa, ya), ImVec2(xb, yb), ImVec2(xb, y + boostH) };
				dl->AddConvexPolyFilled(q, 4, U32(c, 0.22f));
				dl->AddLine(ImVec2(xa, ya), ImVec2(xb, yb), U32(b[i] <= 1.0f && b[i - 1] <= 1.0f ? kBad : c), 1.5f * S);
			}
		}
		{
			const float ls = (fLabel ? fLabel->FontSize : 12.0f) * gUser * 0.85f;
			dl->AddText(fLabel ? fLabel : ImGui::GetFont(), ls, ImVec2(x0 + 4.0f * S, y + 2.0f * S), U32(kMuted), uiMatchMe >= 0 && uiMatchMe < P ? (Upper(a.players[uiMatchMe].name) + "'S BOOST").c_str() : "BOOST");
		}
		// where the open replay is right now
		const int now = uiMatchNowFrame.load();
		if (sameReplayOpen && now >= a.frame0 && a.frame1 > a.frame0)
		{
			// by frame number, then to time through the samples
			float t = a.t0;
			for (size_t i = 0; i < a.sampleFrame.size(); i++) { if (a.sampleFrame[i] > now) break; t = a.sampleTime[i]; }
			dl->AddLine(ImVec2(X(t), o.y + 6.0f * S), ImVec2(X(t), o.y + h - 6.0f * S), U32(kText, 0.9f), 1.5f * S);
		}
		dl->PopClipRect();
		// zoomed in: a thin bar along the bottom shows which part of the match this is
		if (uiMatchZoom > 1.01f)
		{
			const float by = o.y + h - 4.0f * S;
			dl->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + 2.0f * S), U32(kTrack));
			dl->AddRectFilled(ImVec2(x0 + (x1 - x0) * (v0 - a.t0) / whole, by), ImVec2(x0 + (x1 - x0) * (v0 + span - a.t0) / whole, by + 2.0f * S), U32(kAccent));
		}
		ImGui::SetCursorScreenPos(o);
		ImGui::InvisibleButton("timeline", ImVec2(W, h));
		const bool over = ImGui::IsItemHovered();
		ImGuiIO& io = ImGui::GetIO();
		if (over && io.MouseWheel != 0.0f)
		{
			const float at = v0 + span * std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);   // the time under the mouse stays put
			const float z = std::clamp(uiMatchZoom * (io.MouseWheel > 0 ? 1.35f : 1.0f / 1.35f), 1.0f, 24.0f);
			uiMatchView0 = at - (at - v0) * (uiMatchZoom / z);
			uiMatchZoom = z;
			ImGui::SetScrollY(uiMatchKeepScroll);   // the wheel was for the timeline: the window stays where it was
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 4.0f * S))
		{
			uiMatchView0 -= io.MouseDelta.x / (x1 - x0) * span;
			uiMatchDragged = true;
		}
		if (over && ImGui::IsMouseDoubleClicked(0)) { uiMatchZoom = 1.0f; uiMatchView0 = a.t0; }
		if (hovered < 0 && !uiMatchDragged && !uiMatchChanceTip.empty()) ImGui::SetTooltip("%s", uiMatchChanceTip.c_str());
		uiMatchChanceTip.clear();
		if (hovered >= 0 && !uiMatchDragged)
		{
			const Moment& m = a.moments[hovered];
			ImGui::SetTooltip("%d:%02d  %s%s", m.clock / 60, m.clock % 60, m.text.c_str(), sameReplayOpen ? "\nClick to jump the replay there." : "");
		}
		if (ImGui::IsMouseReleased(0))
		{
			if (over && hovered >= 0 && !uiMatchDragged) jump(hovered);
			uiMatchDragged = false;
		}
		ImGui::Dummy(ImVec2(W, 2.0f * S));
		{
			FontPush f(fLabel);
			ImGui::TextColored(kMuted, uiMatchZoom > 1.01f ? "DRAG TO MOVE   /   SCROLL TO ZOOM   /   DOUBLE-CLICK FOR THE WHOLE MATCH" : "SCROLL ON THE TIMELINE TO ZOOM IN");
		}
	}

	// ---- A shot, opened like a rep (Nov 11): pick a shot, a goal or a save's shot and its breakdown shows here ----
	if (uiMatchSel >= 0 && uiMatchSel < static_cast<int>(a.moments.size()) && data)
	{
		const Moment& m = a.moments[uiMatchSel];
		const bool isShot = m.kind == MomentKind::Shot || m.kind == MomentKind::Goal;
		if (isShot && (uiMatchShotFor != uiMatchSel || uiMatchShotFile != a.replayName))
		{
			uiMatchShotFor = uiMatchSel;
			uiMatchShotFile = a.replayName;
			uiMatchShotSaved.clear();
			uiMatchShot = UiMatchShot{};
			uiMatchShot.player = m.player;
			try
			{
				if (Match::MakeShotRep(*data, m.player, m.frame, uiMatchShot.rep, uiMatchShot.error))
				{
					uiMatchShot.shot = ShotGeometry::Analyze(uiMatchShot.rep.frames);
					uiMatchShot.segs = Segmenter::Split(uiMatchShot.rep.frames, uiMatchShot.rep.eventTick);
					uiMatchShot.grade = Grader::GradeRep(uiMatchShot.rep.frames, uiMatchShot.segs, uiMatchShot.shot, uiMatchShot.rep.goal, false, ShotReference{});
					uiMatchShot.ok = uiMatchShot.shot.valid;
					if (!uiMatchShot.ok) uiMatchShot.error = "Couldn't read a shot out of that touch";
				}
			}
			catch (const std::exception&) { uiMatchShot.ok = false; uiMatchShot.error = "Couldn't read that shot"; }
		}
		if (isShot)
		{
			const UiMatchShot& sh = uiMatchShot;
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float pad = 12.0f * S, inW = W - 2.0f * pad;
			ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
			ImGui::BeginGroup();
			const std::string who = sh.player >= 0 && sh.player < P ? a.players[sh.player].name : std::string("?");
			if (!sh.ok)
			{
				{
					FontPush f(fHead);
					ImGui::TextColored(kText, "%s", Upper(who + "'s shot").c_str());
				}
				ImGui::TextColored(kSub, "%s.", sh.error.empty() ? "Couldn't read that shot" : sh.error.c_str());
			}
			else
			{
				// what came of it, as a slanted block: a match has defenders, so "saved" is its own result, not a miss
				const bool goal = sh.rep.goal, saved = sh.rep.endReason == "saved";
				const ImVec4 rc = goal ? kGold : saved ? kAccent : kBad;
				const char* rt = goal ? "GOAL" : saved ? "SAVED" : "MISS";
				const ImVec2 b0 = ImGui::GetCursorScreenPos();
				const float bw = 74.0f * S, bh = 40.0f * S;
				SlantQuad(dl, ImVec2(b0.x + 3.0f * S, b0.y + 3.0f * S), ImVec2(b0.x + bw + 3.0f * S, b0.y + bh + 3.0f * S), U32(ImVec4(rc.x * 0.25f, rc.y * 0.25f, rc.z * 0.25f, 1.0f)));
				SlantQuad(dl, b0, ImVec2(b0.x + bw, b0.y + bh), U32(rc));
				ImFont* bf = fHead ? fHead : ImGui::GetFont();
				const float bfs = bf->FontSize * gUser;
				const ImVec2 ts = bf->CalcTextSizeA(bfs, 1e9f, 0.0f, rt);
				dl->AddText(bf, bfs, ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + (bh - ts.y) * 0.5f), U32(OnFill(rc)), rt);
				ImGui::SetCursorScreenPos(ImVec2(b0.x + bw + 16.0f * S, b0.y - 2.0f * S));
				ImGui::BeginGroup();
				{
					FontPush f(fHead);
					ImGui::TextColored(kText, "%s", Upper(who + ": " + ShotGeometry::TypeName(sh.shot.type)).c_str());
				}
				std::string why = sh.grade.overallWhy;
				if (saved) why = sh.rep.stoppedBy >= 0 && sh.rep.stoppedBy < P ? "On target; " + a.players[sh.rep.stoppedBy].name + " got to it" : std::string("On target, and stopped");
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inW - bw - 16.0f * S);
				ImGui::TextColored(kSub, "%s", why.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndGroup();
				ImGui::SetCursorScreenPos(ImVec2(b0.x, std::max(ImGui::GetCursorScreenPos().y, b0.y + bh + 8.0f * S)));
				// each part of the shot, graded the way a rep's is (the result and the landing are left out: the block
				// above is the result, and the play carries on after a match shot)
				const float rowH = 24.0f * S, fs = fCapsF->FontSize * gUser;
				for (const SegmentGrade& sg : sh.grade.segs)
				{
					if (sg.segIndex < 0 || sg.segIndex >= static_cast<int>(sh.segs.segs.size())) continue;
					const SegKind kind = sh.segs.segs[sg.segIndex].kind;
					if (kind == SegKind::Result || kind == SegKind::Recovery || sg.grade == Grade::None) continue;
					const ImVec2 r0 = ImGui::GetCursorScreenPos();
					const std::string gname = Grader::Name(sg.grade);
					const ImVec4 gc = SegGradeColor(gname);
					dl->AddRectFilled(r0, ImVec2(r0.x + inW, r0.y + rowH), U32(kPanel2));
					dl->AddText(fCapsF, fs, ImVec2(r0.x + 8.0f * S, r0.y + (rowH - fs) * 0.5f), U32(kText), Upper(Segmenter::Name(kind)).c_str());
					const std::string gt = Upper(gname);
					const ImVec2 gs = fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, gt.c_str());
					const ImVec2 g0(r0.x + 104.0f * S, r0.y + (rowH - gs.y - 4.0f * S) * 0.5f), g1(g0.x + 64.0f * S, g0.y + gs.y + 4.0f * S);
					SlantQuad(dl, g0, g1, U32(gc));
					dl->AddText(fCapsF, fs, ImVec2(g0.x + (g1.x - g0.x - gs.x) * 0.5f, g0.y + 2.0f * S), U32(OnFill(gc)), gt.c_str());
					dl->PushClipRect(ImVec2(g1.x + 8.0f * S, r0.y), ImVec2(r0.x + inW - 4.0f * S, r0.y + rowH), true);
					dl->AddText(ImVec2(g1.x + 12.0f * S, r0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), U32(kSub), sg.why.c_str());
					dl->PopClipRect();
					ImGui::Dummy(ImVec2(inW, rowH));
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sg.why.c_str());
					ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH + 2.0f * S));
				}
				ImGui::Dummy(ImVec2(0, 2.0f * S));
				if (OutlineButton("Practise this shot", ImVec2(180.0f * S, 32.0f * S), kOrange))
				{
					// into your reps, so free play has it like any rep: watch it, find the best line, practise it
					RepSummary sum;
					sum.mode = "match";
					sum.map = a.replayName;
					sum.endReason = sh.rep.endReason == "saved" ? "floor" : sh.rep.endReason == "goal" ? "goal" : "floor";
					sum.ticks = static_cast<int>(sh.rep.frames.size());
					sum.durationS = sh.rep.frames.empty() ? 0.0f : sh.rep.frames.back().t;
					for (size_t k = 1; k < sh.rep.frames.size(); k++) if (sh.rep.frames[k].touch && !sh.rep.frames[k - 1].touch) sum.touches++;
					sum.goal = sh.rep.goal;
					sum.eventTick = sh.rep.eventTick;
					auto r50 = [](float v) { return static_cast<int>(std::lround(v / 50.0f)) * 50; };
					const Frame& f0 = sh.rep.frames.front();
					sum.shotKey = std::format("{}_{}_{}_{}_{}", r50(f0.ballX), r50(f0.ballY), r50(f0.ballZ), r50(f0.carX), r50(f0.carY));
					const bool okSave = writer && writer->SaveRep(sh.rep.frames, sum);
					uiMatchShotSaved = okSave ? "Saved to your reps. In free play it's at the top of the rep list: watch it, get the best line, practise it." : "Couldn't save it as a rep.";
					if (okSave) uiMatchRepsStale = true;
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves this shot as a rep, without the other cars.\nIn free play it works like any rep you recorded.");
				if (!uiMatchShotSaved.empty())
				{
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inW);
					ImGui::TextColored(kGood, "%s", uiMatchShotSaved.c_str());
					ImGui::PopTextWrapPos();
				}
				ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inW);
				ImGui::TextWrapped("Read from the replay, which keeps about 22 moments a second and no stick movements, so this is a little coarser than a rep you record yourself.");
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			ImGui::EndGroup();
			const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
			dl->ChannelsSetCurrent(0);
			dl->AddRectFilled(p0, p1, U32(kPanel), 6.0f * S);
			dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f * S, p1.y), U32(teamCol(m.team)));
			dl->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
			ImGui::Dummy(ImVec2(W, 6.0f * S));
		}
	}

	// ---- Moments or the numbers ----
	{
		const float tw = 110.0f * S;
		if (uiMatchTab == 2 ? AccentButton("Review", ImVec2(tw, 0)) : QuietButton("Review", ImVec2(tw, 0))) uiMatchTab = 2;
		ImGui::SameLine(0, 6.0f * S);
		if (uiMatchTab == 0 ? AccentButton("Moments", ImVec2(tw, 0)) : QuietButton("Moments", ImVec2(tw, 0))) uiMatchTab = 0;
		ImGui::SameLine(0, 6.0f * S);
		if (uiMatchTab == 1 ? AccentButton("Numbers", ImVec2(tw, 0)) : QuietButton("Numbers", ImVec2(tw, 0))) uiMatchTab = 1;
	}
	if (!sameReplayOpen)
	{
		FontPush f(fLabel);
		ImGui::TextColored(kMuted, "OPEN THIS REPLAY TO JUMP TO A MOMENT");
	}
	ImGui::Spacing();
	if (uiMatchTab == 2)
	{
		// ---- Takeaways (Nov 12): findings with reasons, for the player you're following, then their team, then the
		// chain behind each goal against them. Click another name above for anyone else's, the other team included. ----
		const int me = uiMatchMe >= 0 && uiMatchMe < P ? uiMatchMe : 0;
		const int myTeam = a.players[me].team;
		int cardId = 0;
		auto card = [&](const Takeaway& t) {
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float pad = 10.0f * S, inW = W - 2.0f * pad - 4.0f * S;
			ImGui::SetCursorScreenPos(ImVec2(p0.x + pad + 4.0f * S, p0.y + pad));
			ImGui::BeginGroup();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inW);
			{
				FontPush f(fHead);
				ImGui::PushStyleColor(ImGuiCol_Text, kText);
				ImGui::TextWrapped("%s", t.claim.c_str());
				ImGui::PopStyleColor();
			}
			if (!t.detail.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, kSub); ImGui::TextWrapped("%s", t.detail.c_str()); ImGui::PopStyleColor(); }
			if (!t.tryThis.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, kAccentText); ImGui::TextWrapped("Try: %s", t.tryThis.c_str()); ImGui::PopStyleColor(); }
			ImGui::PopTextWrapPos();
			if (!t.moments.empty())
			{
				ImGui::PushID(3000 + cardId);
				if (LinkButton(t.moments.size() == 1 ? "Show the moment" : std::format("Show the {} moments", t.moments.size()).c_str()))
				{
					uiMatchPick = t.moments;
					uiMatchFilter = 5;
					uiMatchTab = 0;
					uiMatchSel = t.moments.front();
				}
				ImGui::PopID();
			}
			cardId++;
			ImGui::EndGroup();
			const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
			dl->ChannelsSetCurrent(0);
			dl->AddRectFilled(p0, p1, U32(kPanel), 6.0f * S);
			dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f * S, p1.y), U32(t.good ? kGood : kOrange));
			dl->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
			ImGui::Dummy(ImVec2(W, 5.0f * S));
		};
		// up to three strengths and four things to work on, strongest first (the list is already in that order)
		std::vector<const Takeaway*> shownAbove;   // in "Work on these first": not repeated below
		auto section = [&](const char* title, int player, int team) {
			int good = 0, bad = 0, drawn = 0;
			for (const Takeaway& t : a.takeaways)
			{
				if (t.player != player || t.team != team) continue;
				if (std::find(shownAbove.begin(), shownAbove.end(), &t) != shownAbove.end()) continue;
				if (t.good ? good >= 3 : bad >= 4) continue;
				if (drawn++ == 0) Heading(title);
				(t.good ? good : bad)++;
				card(t);
			}
			return drawn;
		};
		// =====================================================================================================
		// The review (Nov 13): the selected player's grade and two sentences, everyone's grades side by side, the
		// scoring-chance graph, the turning points, a grade per area, and what to work on first. Then the rest of the
		// findings as cards. Grades use the rep review's scale; the marks are chess's.
		// =====================================================================================================
		const MatchPlayerStats& ms = a.stats[me];
		const bool graded = ms.overall >= 0;
		const int lobby = a.lobbyLevel >= 0 ? static_cast<int>(std::lround(a.lobbyLevel)) : -1;
		auto gradeCol = [&](int g) -> ImVec4 {
			if (gColorBlind)
			{
				static const unsigned cb[6] = { 0xF2F2F2, 0x56B4E9, 0x009E73, 0xF0E442, 0xE69F00, 0xCC79A7 };
				return g >= 0 && g < 6 ? Hex(cb[g]) : kMuted;
			}
			static const unsigned c[6] = { 0x2DE2E6, 0x3DDC84, 0x5EB8FF, 0xFFB020, 0xFF8A1F, 0xFF5A5A };
			return g >= 0 && g < 6 ? Hex(c[g]) : kMuted;
		};
		ImFont* fMark = fHead;   // the marks and the grade words share one face (Coop, Nov 13)
		// a tick for Solid: the fonts have no check mark
		auto tick = [&](ImDrawList* l, ImVec2 c, float h, ImU32 col, float th) {
			const ImVec2 pts[3] = { ImVec2(c.x - h * 0.42f, c.y), ImVec2(c.x - h * 0.1f, c.y + h * 0.32f), ImVec2(c.x + h * 0.45f, c.y - h * 0.36f) };
			l->AddPolyline(pts, 3, col, 0, th);
		};
		// the grade as a slanted chip: mark then word, one face, one size. Returns its width.
		auto chipW = [&](int g, float fs) {
			const float mark = g == ReviewGrade::Solid ? fs * 0.75f : fMark->CalcTextSizeA(fs, 1e9f, 0.0f, Match::GradeMark(g)).x;
			return fs * 0.7f + mark + fs * 0.32f + fMark->CalcTextSizeA(fs, 1e9f, 0.0f, Upper(Match::GradeName(g)).c_str()).x + fs * 0.7f;
		};
		auto gradeChip = [&](ImDrawList* l, ImVec2 p, int g, float fs) {
			const float w = chipW(g, fs), h = fs * 1.3f;
			const ImVec4 c = gradeCol(g);
			SlantQuad(l, p, ImVec2(p.x + w, p.y + h), U32(c));
			const ImU32 tc = U32(OnFill(c));
			float x = p.x + fs * 0.7f;
			const float ty = p.y + (h - fs) * 0.5f;
			if (g == ReviewGrade::Solid) { tick(l, ImVec2(x + fs * 0.36f, p.y + h * 0.52f), fs * 0.75f, tc, std::max(1.5f, fs * 0.13f)); x += fs * 0.75f; }
			else { const char* mk = Match::GradeMark(g); l->AddText(fMark, fs, ImVec2(x, ty), tc, mk); x += fMark->CalcTextSizeA(fs, 1e9f, 0.0f, mk).x; }
			x += fs * 0.32f;
			l->AddText(fMark, fs, ImVec2(x, ty), tc, Upper(Match::GradeName(g)).c_str());
			return w;
		};
		// the grade as a round dot with its mark
		auto gradeDot = [&](ImDrawList* l, ImVec2 c, float r, int g) {
			const ImVec4 col = gradeCol(g);
			l->AddCircleFilled(c, r, U32(col), 24);
			const ImU32 tc = U32(OnFill(col));
			if (g == ReviewGrade::Solid) { tick(l, ImVec2(c.x, c.y + r * 0.08f), r * 0.95f, tc, std::max(1.5f, r * 0.2f)); return; }
			const float fs = r * 1.25f;
			const char* mk = Match::GradeMark(g);
			const ImVec2 ts = fMark->CalcTextSizeA(fs, 1e9f, 0.0f, mk);
			l->AddText(fMark, fs, ImVec2(c.x - ts.x * 0.5f - fs * 0.04f, c.y - ts.y * 0.5f), tc, mk);
		};
		auto sub = [&](const char* t) { Heading(t); };
		auto shortRank = [](int lv) { static const char* n[] = { "Gold", "Platinum", "Diamond", "Champion", "Grand Champ", "SSL" }; return n[std::clamp(lv, 0, 5)]; };

		if (!graded)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kSub);
			ImGui::TextWrapped("Too short to grade: the review needs about two minutes of play. The findings below still apply.");
			ImGui::PopStyleColor();
		}
		else
		{
			// ---- 1. The grade and the two sentences ----
			{
				const float bigFs = (fDisplay ? fDisplay->FontSize : 27.0f) * gUser;
				const float cw = chipW(ms.overall, bigFs), gap = 14.0f * S;
				const ImVec2 p0 = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(ImVec2(p0.x + cw + gap, p0.y));
				ImGui::BeginGroup();
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - cw - gap);
				{
					FontPush f(fHead);
					ImGui::TextColored(kText, "%s", Upper(a.players[me].name).c_str());
				}
				ImGui::SameLine(0, 8.0f * S);
				{
					FontPush f(fHead);
					ImGui::TextColored(ms.impact >= 0 ? kGood : kBad, "%+.1f", ms.impact);
				}
				ImGui::SameLine(0, 5.0f * S);
				{
					FontPush f(fLabel);
					ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f * S);
					ImGui::TextColored(kMuted, "IMPACT");
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("The scoring chance your touches added, against the average player in this game.\nBetter than %.0f%% of %s players.", ms.impactPct, Match::LevelName(lobby));
				}
				ImGui::PushStyleColor(ImGuiCol_Text, kSub);
				ImGui::TextWrapped("%s", ms.summary.c_str());
				ImGui::PopStyleColor();
				ImGui::PopTextWrapPos();
				ImGui::EndGroup();
				const float textH = ImGui::GetItemRectSize().y;
				const float chipH = bigFs * 1.3f;
				gradeChip(dl, ImVec2(p0.x + 4.0f * S, p0.y + std::max(0.0f, (textH - chipH) * 0.5f)), ms.overall, bigFs);
				ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + std::max(textH, chipH)));
				ImGui::Dummy(ImVec2(W, 6.0f * S));
			}

			// ---- 2. Everyone in this game: grade, impact, touches by grade, played like ----
			sub("EVERYONE IN THIS GAME");
			{
				std::vector<int> order;
				for (int t = 0; t < 2; t++) for (int i = 0; i < P; i++) if (a.players[i].team == t) order.push_back(i);
				const float labelW = 112.0f * S, colW = order.empty() ? 0.0f : (W - labelW - 8.0f * S) / order.size();
				const float fs = fCapsF->FontSize * gUser;
				const ImVec2 o = ImGui::GetCursorScreenPos();
				float y = o.y + 6.0f * S;
				const float headH = 22.0f * S, tallH = 34.0f * S, rowH = 26.0f * S;
				const float total = headH + tallH * 2.0f + rowH * ReviewGrade::Count + tallH + 10.0f * S;
				dl->AddRectFilled(o, ImVec2(o.x + W, o.y + total), U32(kPanel), 6.0f * S);
				auto colX = [&](size_t k) { return o.x + labelW + colW * k; };
				for (size_t k = 0; k < order.size(); k++)   // your column, lit
					if (order[k] == me) dl->AddRectFilled(ImVec2(colX(k), o.y + 4.0f * S), ImVec2(colX(k) + colW, o.y + total - 4.0f * S), U32(kChip, 0.75f));
				for (size_t k = 0; k < order.size(); k++)
				{
					std::string nm = Upper(a.players[order[k]].name);
					while (nm.size() > 3 && fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, nm.c_str()).x > colW - 6.0f * S) nm.pop_back();
					const float tw = fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, nm.c_str()).x;
					dl->AddText(fCapsF, fs, ImVec2(colX(k) + (colW - tw) * 0.5f, y + (headH - fs) * 0.5f), U32(teamCol(a.players[order[k]].team)), nm.c_str());
				}
				y += headH;
				auto line = [&]() { dl->AddLine(ImVec2(o.x + 8.0f * S, y), ImVec2(o.x + W - 8.0f * S, y), U32(kLineSoft), 1.0f); };
				auto label = [&](const char* t, float h, float indent = 0.0f) {
					dl->AddText(ImVec2(o.x + 12.0f * S + indent, y + (h - ImGui::GetTextLineHeight()) * 0.5f), U32(kSub), t);
				};
				// grade
				line(); label("Grade", tallH);
				for (size_t k = 0; k < order.size(); k++)
				{
					const int g = a.stats[order[k]].overall;
					if (g < 0) continue;
					const float cfs = fs * 0.95f, w = chipW(g, cfs);
					gradeChip(dl, ImVec2(colX(k) + (colW - w) * 0.5f, y + (tallH - cfs * 1.3f) * 0.5f), g, cfs);
				}
				y += tallH;
				// impact
				line(); label("Impact", tallH);
				{
					const float bfs = (fHead ? fHead->FontSize : 20.0f) * gUser;
					float best = -1e9f;
					for (int pl : order) best = std::max(best, a.stats[pl].impact);
					for (size_t k = 0; k < order.size(); k++)
					{
						const float v = a.stats[order[k]].impact;
						const std::string t = std::format("{:+.1f}", v);
						const float tw = fHead->CalcTextSizeA(bfs, 1e9f, 0.0f, t.c_str()).x;
						dl->AddText(fHead, bfs, ImVec2(colX(k) + (colW - tw) * 0.5f, y + (tallH - bfs) * 0.5f), U32(v == best && v > 0 ? kGood : kText), t.c_str());
					}
				}
				y += tallH;
				// touches by grade
				for (int g = 0; g < ReviewGrade::Count; g++)
				{
					line();
					gradeDot(dl, ImVec2(o.x + 22.0f * S, y + rowH * 0.5f), 9.5f * S, g);
					label(Match::GradeName(g), rowH, 24.0f * S);
					for (size_t k = 0; k < order.size(); k++)
					{
						const int n = a.stats[order[k]].touchGrades[g];
						const std::string t = std::to_string(n);
						const float tw = fCapsF->CalcTextSizeA(fs * 1.1f, 1e9f, 0.0f, t.c_str()).x;
						dl->AddText(fCapsF, fs * 1.1f, ImVec2(colX(k) + (colW - tw) * 0.5f, y + (rowH - fs * 1.1f) * 0.5f), U32(n > 0 ? gradeCol(g) : kLine), t.c_str());
					}
					y += rowH;
				}
				// played like
				line(); label("Played like", tallH);
				for (size_t k = 0; k < order.size(); k++)
				{
					const MatchPlayerStats& st = a.stats[order[k]];
					if (st.level < 0) continue;
					const std::string t = Upper(shortRank(static_cast<int>(std::lround(Match::LevelOf(a.teamSize, Base_level, st.level)))));
					const float lfs = fs * 0.85f, tw = fCapsF->CalcTextSizeA(lfs, 1e9f, 0.0f, t.c_str()).x;
					dl->AddText(fCapsF, lfs, ImVec2(colX(k) + (colW - tw) * 0.5f, y + (tallH - lfs) * 0.5f), U32(kSub), t.c_str());
				}
				ImGui::Dummy(ImVec2(W, total + 4.0f * S));
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Grade: how the game went for a %s lobby, all parts together.\nImpact: the scoring chance their touches added, against the average player in this game.\nThe six rows: each touch, graded by how far it moved the scoring chance.", Match::LevelName(lobby));
			}

			// ---- 3. The scoring chance: who was closer to scoring, the selected player's team up ----
			if (!a.chance[0].empty() && a.chance[0].size() == a.frameTime.size())
			{
				sub("WHO WAS CLOSER TO SCORING");
				const int up = myTeam == 1 ? 1 : 0, down = 1 - up;
				const ImVec2 o = ImGui::GetCursorScreenPos();
				const float padX = 10.0f * S, labH = 18.0f * S, gh = 116.0f * S;
				const float h = labH + gh + labH + 4.0f * S;
				dl->AddRectFilled(o, ImVec2(o.x + W, o.y + h), U32(kPanel), 6.0f * S);
				const float x0 = o.x + padX, x1 = o.x + W - padX, gy0 = o.y + labH, mid = gy0 + gh * 0.5f, half = gh * 0.5f - 2.0f * S;
				const float lfs = (fLabel ? fLabel->FontSize : 12.0f) * gUser;
				dl->AddText(fLabel, lfs, ImVec2(x0, o.y + 3.0f * S), U32(teamCol(up)), up == 0 ? "BLUE'S CHANCE" : "ORANGE'S CHANCE");
				dl->AddText(fLabel, lfs, ImVec2(x0, gy0 + gh + 3.0f * S), U32(teamCol(down)), down == 0 ? "BLUE'S CHANCE" : "ORANGE'S CHANCE");
				dl->AddRectFilled(ImVec2(x0, gy0), ImVec2(x1, gy0 + gh), U32(kPanel2));
				const int n = static_cast<int>(a.frameTime.size());
				const float t0 = a.frameTime.front(), t1 = std::max(t0 + 1.0f, a.frameTime.back());
				auto X = [&](float t) { return x0 + (x1 - x0) * (t - t0) / (t1 - t0); };
				const int step = std::max(1, static_cast<int>(n / std::max(1.0f, (x1 - x0) / S)));
				// the difference between the two chances, filled toward whichever team was closer
				int prev = -1;
				for (int i = 0; i < n; i += step)
				{
					if (prev >= 0)
					{
						const float va = a.chance[up][prev] - a.chance[down][prev], vb = a.chance[up][i] - a.chance[down][i];
						const float xa = X(a.frameTime[prev]), xb = X(a.frameTime[i]);
						const float ya = mid - half * std::clamp(va, -1.0f, 1.0f), yb = mid - half * std::clamp(vb, -1.0f, 1.0f);
						const ImVec2 q[4] = { ImVec2(xa, mid), ImVec2(xa, ya), ImVec2(xb, yb), ImVec2(xb, mid) };
						const bool upSide = va + vb >= 0.0f;
						dl->AddConvexPolyFilled(q, 4, U32(teamCol(upSide ? up : down), 0.62f));
					}
					prev = i;
				}
				dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), U32(kLine), 1.0f);
				// goals: a dashed line in the scorers' colour
				const ImVec2 mouse = ImGui::GetIO().MousePos;
				int hov = -1;
				for (int k = 0; k < static_cast<int>(a.moments.size()); k++)
				{
					const Moment& m = a.moments[k];
					if (m.kind != MomentKind::Goal) continue;
					const float x = X(m.time);
					const ImU32 c = U32(teamCol(m.team), 0.9f);
					for (float yy = gy0 + 2.0f * S; yy < gy0 + gh - 2.0f * S; yy += 6.0f * S) dl->AddLine(ImVec2(x, yy), ImVec2(x, std::min(yy + 3.0f * S, gy0 + gh)), c, 1.5f * S);
					const bool top = m.team == up;
					const float tw = fLabel->CalcTextSizeA(lfs * 0.9f, 1e9f, 0.0f, "GOAL").x;
					const float tx = x + 3.0f * S + tw > x1 ? x - 3.0f * S - tw : x + 3.0f * S;
					dl->AddText(fLabel, lfs * 0.9f, ImVec2(tx, top ? gy0 + 2.0f * S : gy0 + gh - lfs - 2.0f * S), c, "GOAL");
				}
				// the big plays: a dot with the mark, on the side of the team that made them
				for (int k = 0; k < static_cast<int>(a.moments.size()); k++)
				{
					const Moment& m = a.moments[k];
					if (m.kind != MomentKind::Play) continue;
					const int g = m.detail == MomentDetail::PlayCalculated ? ReviewGrade::Calculated : m.detail == MomentDetail::PlayGreat ? ReviewGrade::Clean
						: m.detail == MomentDetail::PlayWhoops ? ReviewGrade::Costly : ReviewGrade::Throw;
					const ImVec2 c(X(m.time), mid + (m.team == up ? -1.0f : 1.0f) * 22.0f * S);
					const float r = (m.player == me ? 9.0f : 7.0f) * S;
					dl->AddCircleFilled(c, r + 2.0f * S, U32(kPanel2), 24);
					gradeDot(dl, c, r, g);
					if (std::fabs(mouse.x - c.x) <= r + 2.0f && std::fabs(mouse.y - c.y) <= r + 2.0f) hov = k;
				}
				ImGui::SetCursorScreenPos(o);
				ImGui::InvisibleButton("chancegraph", ImVec2(W, h));
				if (hov >= 0)
				{
					const Moment& m = a.moments[hov];
					ImGui::SetTooltip("%d:%02d  %s%s", m.clock / 60, m.clock % 60, m.text.c_str(), sameReplayOpen ? "\nClick to watch it." : "");
					if (ImGui::IsItemClicked(0)) jump(hov);
				}
				else if (ImGui::IsItemHovered() && mouse.x >= x0 && mouse.x <= x1)
				{
					const float t = t0 + (t1 - t0) * (mouse.x - x0) / (x1 - x0);
					const int i = std::clamp(static_cast<int>(std::lower_bound(a.frameTime.begin(), a.frameTime.end(), t) - a.frameTime.begin()), 0, n - 1);
					ImGui::SetTooltip("Chance of a goal in the next 10 s\nBlue %.0f%%   Orange %.0f%%", 100.0f * a.chance[0][i], 100.0f * a.chance[1][i]);
				}
				ImGui::Dummy(ImVec2(W, 2.0f * S));
			}

			// ---- 4. Turning points: this player's biggest plays (good and bad), and the open nets they left ----
			{
				std::vector<int> picks;
				for (int k = 0; k < static_cast<int>(a.moments.size()); k++)
				{
					const Moment& m = a.moments[k];
					if (m.player == me && (m.kind == MomentKind::Play || m.kind == MomentKind::OpenNet)) picks.push_back(k);
				}
				std::sort(picks.begin(), picks.end(), [&](int x, int y) {
					const float wx = a.moments[x].kind == MomentKind::Play ? std::fabs(a.moments[x].swing) : 0.3f, wy = a.moments[y].kind == MomentKind::Play ? std::fabs(a.moments[y].swing) : 0.3f;
					return wx > wy;
				});
				if (picks.size() > 4) picks.resize(4);
				std::sort(picks.begin(), picks.end());
				if (!picks.empty())
				{
					sub("TURNING POINTS");
					const float rowH = 30.0f * S;
					const ImVec2 o = ImGui::GetCursorScreenPos();
					dl->AddRectFilled(o, ImVec2(o.x + W, o.y + rowH * picks.size() + 4.0f * S), U32(kPanel), 6.0f * S);
					for (size_t r = 0; r < picks.size(); r++)
					{
						const Moment& m = a.moments[picks[r]];
						const ImVec2 p(o.x, o.y + 2.0f * S + rowH * r);
						ImGui::SetCursorScreenPos(p);
						ImGui::PushID(5000 + picks[r]);
						const bool click = ImGui::InvisibleButton("tp", ImVec2(W, rowH));
						const bool hovRow = ImGui::IsItemHovered();
						ImGui::PopID();
						if (hovRow) dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH), U32(kChip, 0.6f));
						if (r > 0) dl->AddLine(ImVec2(p.x + 8.0f * S, p.y), ImVec2(p.x + W - 8.0f * S, p.y), U32(kLineSoft), 1.0f);
						const float cy = p.y + rowH * 0.5f, ty = cy - ImGui::GetTextLineHeight() * 0.5f;
						const std::string clock = std::format("{}:{:02d}", m.clock / 60, m.clock % 60);
						dl->AddText(fCapsF, fCapsF->FontSize * gUser, ImVec2(p.x + 12.0f * S, cy - fCapsF->FontSize * gUser * 0.5f), U32(kMuted), clock.c_str());
						int g = ReviewGrade::Costly;
						std::string lab = "Open net", what = m.text;
						if (m.kind == MomentKind::Play)
						{
							g = m.detail == MomentDetail::PlayCalculated ? ReviewGrade::Calculated : m.detail == MomentDetail::PlayGreat ? ReviewGrade::Clean
								: m.detail == MomentDetail::PlayWhoops ? ReviewGrade::Costly : ReviewGrade::Throw;
							const size_t cut = std::min(m.text.find("! "), m.text.find(". "));
							lab = cut != std::string::npos ? m.text.substr(0, cut + 1) : Match::GradeName(g);
							what = m.brief.empty() ? m.text : m.brief;
						}
						else
						{
							// "Open net for 2 s: foster. was the last to leave" -> "Net open for 2 s after they left it."
							const size_t c = what.find(':');
							if (c != std::string::npos && what.rfind("Open net for ", 0) == 0) what = "Net open for " + what.substr(13, c - 13) + " after they left it.";
						}
						gradeDot(dl, ImVec2(p.x + 62.0f * S, cy), 9.0f * S, g);
						{
							FontPush f(uiFonts[UiFonts::Bold]);
							dl->AddText(ImVec2(p.x + 80.0f * S, ty), U32(kText), lab.c_str());
						}
						const float wx = p.x + 186.0f * S, wEnd = p.x + W - (sameReplayOpen ? 56.0f : 10.0f) * S;
						dl->PushClipRect(ImVec2(wx, p.y), ImVec2(wEnd, p.y + rowH), true);
						dl->AddText(ImVec2(wx, ty), U32(kSub), what.c_str());
						dl->PopClipRect();
						if (hovRow && ImGui::CalcTextSize(what.c_str()).x > wEnd - wx) ImGui::SetTooltip("%s", what.c_str());
						if (sameReplayOpen) dl->AddText(ImVec2(p.x + W - 50.0f * S, ty), U32(kAccentText), "Watch");
						if (click) jump(picks[r]);
					}
					ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rowH * picks.size() + 4.0f * S));
					ImGui::Dummy(ImVec2(W, 2.0f * S));
				}
			}

			// ---- 5. Each part, graded for the lobby's rank: a 3 x 2 grid of tiles ----
			sub(std::format("EACH PART, FOR A {} LOBBY", Upper(Match::LevelName(lobby))).c_str());
			{
				const int cols = W >= 420.0f * S ? 3 : 2;
				const float gap = 6.0f * S, tw = (W - gap * (cols - 1)) / cols, pad = 9.0f * S;
				const float lfs = (fLabel ? fLabel->FontSize : 12.0f) * gUser;
				for (int row = 0; row * cols < Area_Count; row++)
				{
					// a row is as tall as its tallest tile
					float rowH = 0.0f;
					for (int c = 0; c < cols && row * cols + c < Area_Count; c++)
					{
						const AreaGrade& ag = ms.area[row * cols + c];
						const ImVec2 ws = ImGui::CalcTextSize(ag.why.c_str(), nullptr, false, tw - 2.0f * pad);
						rowH = std::max(rowH, pad + 20.0f * S + lfs + 14.0f * S + ws.y + pad);
					}
					const ImVec2 o = ImGui::GetCursorScreenPos();
					for (int c = 0; c < cols && row * cols + c < Area_Count; c++)
					{
						const int ar = row * cols + c;
						const AreaGrade& ag = ms.area[ar];
						const ImVec2 p(o.x + (tw + gap) * c, o.y);
						dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + rowH), U32(kPanel), 6.0f * S);
						const float hfs = (fHead ? fHead->FontSize : 20.0f) * gUser * 0.8f;
						dl->AddText(fHead, hfs, ImVec2(p.x + pad, p.y + pad), U32(kText), Upper(Match::AreaName(ar)).c_str());
						gradeDot(dl, ImVec2(p.x + tw - pad - 10.0f * S, p.y + pad + 10.0f * S), 10.0f * S, ag.grade);
						float y = p.y + pad + 22.0f * S;
						dl->AddText(fLabel, lfs, ImVec2(p.x + pad, y), U32(gradeCol(ag.grade)), Upper(Match::GradeName(ag.grade)).c_str());
						y += lfs + 5.0f * S;
						dl->AddRectFilled(ImVec2(p.x + pad, y), ImVec2(p.x + tw - pad, y + 4.0f * S), U32(kTrack), 2.0f * S);
						dl->AddRectFilled(ImVec2(p.x + pad, y), ImVec2(p.x + pad + (tw - 2.0f * pad) * std::clamp(ag.score, 2.0f, 100.0f) / 100.0f, y + 4.0f * S), U32(gradeCol(ag.grade)), 2.0f * S);
						y += 9.0f * S;
						dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + pad, y), U32(kMuted), ag.why.c_str(), nullptr, tw - 2.0f * pad);
					}
					ImGui::Dummy(ImVec2(W, rowH + gap));
				}
			}

			// ---- 6. Work on these first: the heaviest things to fix for this player, theirs and their team's ----
			{
				std::vector<const Takeaway*> fix;
				for (const Takeaway& t : a.takeaways) if (!t.good && !t.tryThis.empty() && (t.player == me || (t.player < 0 && t.team == myTeam))) fix.push_back(&t);
				if (fix.size() > 3) fix.resize(3);
				shownAbove = fix;
				if (!fix.empty())
				{
					sub("WORK ON THESE FIRST");
					for (size_t k = 0; k < fix.size(); k++)
					{
						const Takeaway& t = *fix[k];
						dl->ChannelsSplit(2);
						dl->ChannelsSetCurrent(1);
						const ImVec2 p0 = ImGui::GetCursorScreenPos();
						const float pad = 10.0f * S;
						// the number big and orange on the left, the finding and what to try beside it
						const float numW = 22.0f * S;
						{
							const float nfs = (fDisplay ? fDisplay->FontSize : 27.0f) * gUser;
							dl->AddText(fDisplay, nfs, ImVec2(p0.x + pad + 6.0f * S, p0.y + pad - 3.0f * S), U32(kOrange), std::to_string(k + 1).c_str());
						}
						ImGui::SetCursorScreenPos(ImVec2(p0.x + pad + 6.0f * S + numW, p0.y + pad));
						ImGui::BeginGroup();
						ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - 2.0f * pad - 8.0f * S - numW);
						{
							FontPush f(fHead);
							// a team finding says so, so it doesn't read as this player's own count
							ImGui::TextWrapped("%s%s", t.player < 0 ? "As a team: " : "", t.claim.c_str());
						}
						ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
						ImGui::TextWrapped("%s", t.tryThis.c_str());
						ImGui::PopStyleColor();
						ImGui::PopTextWrapPos();
						if (!t.moments.empty())
						{
							ImGui::PushID(6000 + static_cast<int>(k));
							if (LinkButton(t.moments.size() == 1 ? "Show the moment" : std::format("Show the {} moments", t.moments.size()).c_str()))
							{
								uiMatchPick = t.moments;
								uiMatchFilter = 5;
								uiMatchTab = 0;
								uiMatchSel = t.moments.front();
							}
							ImGui::PopID();
						}
						ImGui::EndGroup();
						const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
						dl->ChannelsSetCurrent(0);
						dl->AddRectFilled(p0, p1, U32(kPanel), 6.0f * S);
						dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f * S, p1.y), U32(kOrange));
						dl->ChannelsMerge();
						ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
						ImGui::Dummy(ImVec2(W, 5.0f * S));
					}
				}
			}
		}

		// ---- The rest of the findings, as before: this player's, then their team's ----
		{
			const std::string more = std::format("MORE ON {}", Upper(a.players[me].name));
			if (section(more.c_str(), me, myTeam) == 0 && !graded)
			{
				Heading(more.c_str());
				ImGui::TextColored(kSub, "Nothing stood out as a pattern for this player in this match.");
			}
		}
		section(myTeam == 0 ? "BLUE, AS A TEAM" : "ORANGE, AS A TEAM", -1, myTeam);
		bool anyChain = false;
		for (const GoalChain& ch : a.chains)
		{
			if (ch.against != myTeam || ch.goalMoment < 0 || ch.goalMoment >= static_cast<int>(a.moments.size())) continue;
			if (!anyChain) { Heading(myTeam == 0 ? "BEHIND THE GOALS AGAINST BLUE" : "BEHIND THE GOALS AGAINST ORANGE"); anyChain = true; }
			const Moment& g = a.moments[ch.goalMoment];
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(4000 + ch.goalMoment);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - 8.0f * S);
			ImGui::BeginGroup();
			ImGui::Dummy(ImVec2(0, 2.0f * S));
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f * S);
			ImGui::TextColored(kGold, "%d:%02d  %s", g.clock / 60, g.clock % 60, g.text.c_str());
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f * S);
			ImGui::PushStyleColor(ImGuiCol_Text, kSub);
			ImGui::TextWrapped("%s.", ch.text.c_str());
			ImGui::PopStyleColor();
			ImGui::Dummy(ImVec2(0, 2.0f * S));
			ImGui::EndGroup();
			ImGui::PopTextWrapPos();
			const ImVec2 q(p.x + W, ImGui::GetItemRectMax().y);
			if (ImGui::IsMouseHoveringRect(p, q) && ImGui::IsMouseClicked(0)) jump(ch.goalMoment);
			dl->AddRectFilled(p, ImVec2(p.x + 3.0f * S, q.y), U32(kGold));
			ImGui::PopID();
			ImGui::Dummy(ImVec2(W, 3.0f * S));
		}
		ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		ImGui::TextWrapped("These say what happened, not what was called: a scan can't hear comms. \"A lot\" is measured against the other players in this match for now. Click any name above for that player's, the other team included.");
		ImGui::PopStyleColor();
	}
	else if (uiMatchTab == 0)
	{
		static const char* kFilterNames[8] = { "All", "Mine", "Shots", "Ball", "Boost", "From the takeaway", "Rotation", "Big plays" };
		static const int kOrder[8] = { 5, 0, 1, 7, 2, 3, 6, 4 };
		bool first = true;
		float rowRight = 0.0f;
		for (int o = 0; o < 8; o++)
		{
			const int i = kOrder[o];
			if (i == 5 && uiMatchPick.empty()) continue;
			// wrap to a second row when the next button wouldn't fit
			const float bw = ImGui::CalcTextSize(kFilterNames[i]).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f * S;
			if (!first && rowRight + 4.0f * S + bw <= W) ImGui::SameLine(0, 4.0f * S);
			else rowRight = 0.0f;
			rowRight += (rowRight > 0.0f ? 4.0f * S : 0.0f) + bw;
			first = false;
			ImGui::PushID(2000 + i);
			if (uiMatchFilter == i ? OutlineButton(kFilterNames[i]) : QuietButton(kFilterNames[i])) uiMatchFilter = i;
			ImGui::PopID();
		}
		ImGui::Spacing();
		const float rowH = 26.0f * S, listH = std::min(300.0f * S, ImGui::GetIO().DisplaySize.y * 0.3f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::BeginChild("moments", ImVec2(W, listH), false);
		ImDrawList* cl = ImGui::GetWindowDrawList();
		const float cw = ImGui::GetContentRegionAvail().x;
		int count = 0;
		for (int i = 0; i < static_cast<int>(a.moments.size()); i++)
		{
			const Moment& m = a.moments[i];
			if (!shown(m)) continue;
			count++;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("m", ImVec2(cw, rowH))) jump(i);
			const bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			const bool sel = i == uiMatchSel, mine = m.player == uiMatchMe || m.other == uiMatchMe;
			cl->AddRectFilled(p, ImVec2(p.x + cw, p.y + rowH), U32(sel ? kPanel : hov ? kChip : kPanel2));
			cl->AddRectFilled(p, ImVec2(p.x + 3.0f * S, p.y + rowH), U32(teamCol(m.team)));
			const float fs = fCapsF->FontSize * gUser, ty = p.y + (rowH - fs) * 0.5f;
			const std::string clock = std::format("{}:{:02d}", m.clock / 60, m.clock % 60);
			cl->AddText(fCapsF, fs, ImVec2(p.x + 12.0f * S, ty), U32(kMuted), clock.c_str());
			// the kind, as a slanted chip
			// (Nov 13) a big play's chip is its quick-chat label, and the text loses it
			std::string text = m.text;
			std::string kind = Upper(Match::KindName(m.kind));
			ImVec4 kc = kindCol(m.kind);
			if (m.kind == MomentKind::Play)
			{
				size_t cut = std::min(text.find("! "), text.find(". "));
				if (cut != std::string::npos) { kind = Upper(text.substr(0, cut + 1)); text = text.substr(cut + 2); }
				kc = m.detail == MomentDetail::PlayCalculated ? kGold : m.detail == MomentDetail::PlayGreat ? kGood : m.detail == MomentDetail::PlayWhoops ? kWarn : kBad;
			}
			const ImVec2 ks = fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, kind.c_str());
			const ImVec2 k0(p.x + 50.0f * S, p.y + (rowH - ks.y - 4.0f * S) * 0.5f), k1(k0.x + 104.0f * S, k0.y + ks.y + 4.0f * S);
			SlantQuad(cl, k0, k1, U32(kc, mine ? 1.0f : 0.28f));
			cl->AddText(fCapsF, fs, ImVec2(k0.x + (k1.x - k0.x - ks.x) * 0.5f, k0.y + 2.0f * S), U32(mine ? OnFill(kc) : kText), kind.c_str());
			// what happened (the kind is already on the chip, so the text drops its "Kind: " start)
			const std::string lead = std::string(Match::KindName(m.kind)) + ": ";
			if (text.rfind(lead, 0) == 0) text = text.substr(lead.size());
			if (m.kind == MomentKind::Kickoff && !text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
			cl->PushClipRect(ImVec2(k1.x + 8.0f * S, p.y), ImVec2(p.x + cw - 6.0f * S, p.y + rowH), true);
			cl->AddText(ImVec2(k1.x + 12.0f * S, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), U32(mine ? kText : kSub), text.c_str());
			cl->PopClipRect();
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 2.0f * S));
		}
		if (count == 0) ImGui::TextColored(kMuted, "Nothing of that kind in this match.");
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}
	else
	{
		// each player's numbers, side by side: yours in white, the best of each row marked
		struct Row { const char* label; std::function<float(const MatchPlayerStats&)> get; const char* fmt; int better; const char* tip; int base = -1; };   // better: 1 higher, -1 lower, 0 neither; base: its rank baseline
		const std::vector<Row> rows = {
			{ "Score", [](const MatchPlayerStats& s) { return static_cast<float>(s.score); }, "{:.0f}", 1, nullptr },
			{ "Chance added", [](const MatchPlayerStats& s) { return s.chanceAdded; }, "{:+.1f}", 1, "Every touch's effect on the scoring chance, added up: above zero, their touches made a goal for their team likelier (or one against less likely)." },
			{ "Big plays", [](const MatchPlayerStats& s) { return static_cast<float>(s.playsGood); }, "{:.0f}", 1, "Touches that swung the scoring chance a long way their team's way (labelled in Moments)." },
			{ "Goals", [](const MatchPlayerStats& s) { return static_cast<float>(s.goals); }, "{:.0f}", 1, nullptr },
			{ "Assists", [](const MatchPlayerStats& s) { return static_cast<float>(s.assists); }, "{:.0f}", 1, nullptr },
			{ "Shots", [](const MatchPlayerStats& s) { return static_cast<float>(s.shots); }, "{:.0f}", 1, nullptr, Base_shots },
			{ "Saves", [](const MatchPlayerStats& s) { return static_cast<float>(s.saves); }, "{:.0f}", 1, nullptr, Base_saves },
			{ "Touches", [](const MatchPlayerStats& s) { return static_cast<float>(s.touches); }, "{:.0f}", 0, nullptr, Base_touches },
			{ "On the ball", [](const MatchPlayerStats& s) { return s.onBallSecs; }, "{:.0f} s", 0, "Time as the last player to touch it." },
			{ "50s won", [](const MatchPlayerStats& s) { return static_cast<float>(s.fiftiesWon); }, "{:.0f}", 1, "Out of the 50/50s they were in (hover a name's row for the count in the list)." },
			{ "Passes", [](const MatchPlayerStats& s) { return static_cast<float>(s.passes); }, "{:.0f}", 1, "A touch a teammate got to next.", Base_passes },
			{ "Clears", [](const MatchPlayerStats& s) { return static_cast<float>(s.clears); }, "{:.0f}", 1, "Sent out of their own third.", Base_clears },
			{ "Giveaways", [](const MatchPlayerStats& s) { return static_cast<float>(s.giveaways); }, "{:.0f}", -1, "A touch that went straight to the other team (not a shot, a clear or a 50).", Base_giveaways },
			{ "Average boost", [](const MatchPlayerStats& s) { return s.avgBoost; }, "{:.0f}", 0, nullptr, Base_avgBoost },
			{ "Time on empty", [](const MatchPlayerStats& s) { return s.noBoostSecs; }, "{:.0f} s", -1, nullptr, Base_noBoostSecs },
			{ "Boost wasted", [](const MatchPlayerStats& s) { return s.boostWasted; }, "{:.0f}", -1, "What they already had when they took a full pad at 60 or more.", Base_boostWasted },
			{ "Burned at top speed", [](const MatchPlayerStats& s) { return s.supersonicBoost; }, "{:.0f}", -1, "Boost used on the ground while already at top speed, where it adds nothing.", Base_supersonicBoost },
			{ "Supersonic", [](const MatchPlayerStats& s) { return s.supersonicPct; }, "{:.0f}%", 0, nullptr, Base_supersonicPct },
			{ "In the air", [](const MatchPlayerStats& s) { return s.airPct; }, "{:.0f}%", 0, nullptr, Base_airPct },
			{ "Behind the ball", [](const MatchPlayerStats& s) { return s.behindBallPct; }, "{:.0f}%", 0, "Share of the time they were goal side of the ball.", Base_behindBallPct },
			{ "Closest to it", [](const MatchPlayerStats& s) { return s.closestPct; }, "{:.0f}%", 0, "Share of the time they were their team's nearest player to the ball." },
			{ "Last back", [](const MatchPlayerStats& s) { return s.lastBackPct; }, "{:.0f}%", 0, "Share of the time they were their team's furthest player back." },
			{ "Aerials won", [](const MatchPlayerStats& s) { return static_cast<float>(s.aerialsWon); }, "{:.0f}", 1, "First to a high ball with an opponent going up for it too.", Base_aerialsWon },
			{ "Shadows", [](const MatchPlayerStats& s) { return static_cast<float>(s.shadows); }, "{:.0f}", 1, "Spells of staying goal side and backing off with the ball carrier.", Base_shadows },
			{ "Near a teammate", [](const MatchPlayerStats& s) { return s.bunchedPct; }, "{:.0f}%", -1, "Share of the time within two car lengths of a teammate (kickoffs aside).", Base_bunchedPct },
			{ "Goal side, their ball", [](const MatchPlayerStats& s) { return s.goalSideTheirsPct; }, "{:.0f}%", 1, "Share of the time the other team had the ball that they were between it and their own goal.", Base_goalSideTheirsPct },
			{ "Out of the play", [](const MatchPlayerStats& s) { return s.farFromPlayPct; }, "{:.0f}%", -1, "Share of the time over 5,000 units (about 40 car lengths) from the ball.", Base_farFromPlayPct },
			{ "Support distance", [](const MatchPlayerStats& s) { return s.supportBack / 118.0f; }, "{:.0f} cars", -1, "As the second player with their team on the ball: how far behind the ball, in car lengths.", Base_supportBack },
			{ "Own third", [](const MatchPlayerStats& s) { return s.defPct; }, "{:.0f}%", 0, nullptr },
			{ "Their third", [](const MatchPlayerStats& s) { return s.offPct; }, "{:.0f}%", 0, nullptr },
		};
		std::vector<int> order;
		for (int t = 0; t < 2; t++) for (int i = 0; i < P; i++) if (a.players[i].team == t) order.push_back(i);
		// (Nov 13) the last column: the middle player at the rank this lobby plays like, scaled to this game's length
		const int lobby = a.lobbyLevel >= 0 ? static_cast<int>(std::lround(a.lobbyLevel)) : -1;
		const bool typical = lobby >= 0 && a.teamSize >= 1 && a.teamSize <= 3;
		if (typical)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kSub);
			ImGui::TextWrapped("This lobby's numbers look most like %s. %s", Match::LevelName(lobby),
				uiMatchMe >= 0 && uiMatchMe < P && a.stats[uiMatchMe].level >= 0
					? std::format("{}'s look like {}, and their positioning grades {} for that rank.", a.players[uiMatchMe].name,
						Match::LevelName(static_cast<int>(std::lround(Match::LevelOf(a.teamSize, Base_level, a.stats[uiMatchMe].level)))),
						Match::GradeName(a.stats[uiMatchMe].area[Area_Positioning].grade)).c_str() : "");
			ImGui::PopStyleColor();
			ImGui::Spacing();
		}
		const float labelW = 108.0f * S, typW = typical ? 74.0f * S : 0.0f;
		const float colW = order.empty() ? 0.0f : (W - labelW - typW) / order.size(), rowH = 22.0f * S;
		const float fs = fCapsF->FontSize * gUser;
		ImVec2 p = ImGui::GetCursorScreenPos();
		for (size_t k = 0; k < order.size(); k++)
		{
			const int pl = order[k];
			std::string nm = Upper(a.players[pl].name);
			while (nm.size() > 3 && fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, nm.c_str()).x > colW - 8.0f * S) nm.pop_back();
			const float tx = p.x + labelW + colW * k + colW - 6.0f * S - fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, nm.c_str()).x;
			dl->AddText(fCapsF, fs, ImVec2(tx, p.y + 2.0f * S), U32(teamCol(a.players[pl].team)), nm.c_str());
		}
		if (typical)
		{
			std::string nm = "TYPICAL";
			const float tx = p.x + W - 6.0f * S - fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, nm.c_str()).x;
			dl->AddText(fCapsF, fs, ImVec2(tx, p.y + 2.0f * S), U32(kMuted), nm.c_str());
		}
		ImGui::Dummy(ImVec2(W, rowH));
		for (size_t r = 0; r < rows.size(); r++)
		{
			p = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(W, rowH));
			const bool hov = ImGui::IsItemHovered();
			if (hov && rows[r].tip) ImGui::SetTooltip("%s", rows[r].tip);
			dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH - 2.0f * S), U32(hov ? kChip : (r % 2 == 0 ? kPanel2 : kPanel)));
			dl->AddText(ImVec2(p.x + 8.0f * S, p.y + (rowH - 2.0f * S - ImGui::GetTextLineHeight()) * 0.5f), U32(kSub), rows[r].label);
			float best = 0;
			bool any = false;
			if (rows[r].better != 0)
				for (int pl : order)
				{
					const float v = rows[r].get(a.stats[pl]);
					if (!any || (rows[r].better > 0 ? v > best : v < best)) best = v;
					any = true;
				}
			for (size_t k = 0; k < order.size(); k++)
			{
				const int pl = order[k];
				const float v = rows[r].get(a.stats[pl]);
				const std::string text = std::vformat(rows[r].fmt, std::make_format_args(v));
				const bool top = rows[r].better != 0 && any && v == best && (rows[r].better > 0 ? v > 0 : true);
				const float tx = p.x + labelW + colW * k + colW - 6.0f * S - fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, text.c_str()).x;
				dl->AddText(fCapsF, fs, ImVec2(tx, p.y + (rowH - 2.0f * S - fs) * 0.5f), U32(top ? kGood : pl == uiMatchMe ? kText : kSub), text.c_str());
			}
			if (typical && rows[r].base >= 0)
			{
				const float* b = Match::Baseline(a.teamSize, lobby, rows[r].base);
				const float v = Match::StatPerFive(rows[r].base) ? b[2] * a.seconds / 300.0f : rows[r].base == Base_supportBack ? b[2] / 118.0f : b[2];
				const std::string text = std::vformat(rows[r].fmt, std::make_format_args(v));
				const float tx = p.x + W - 6.0f * S - fCapsF->CalcTextSizeA(fs, 1e9f, 0.0f, text.c_str()).x;
				dl->AddText(fCapsF, fs, ImVec2(tx, p.y + (rowH - 2.0f * S - fs) * 0.5f), U32(kMuted), text.c_str());
			}
		}
		ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		ImGui::TextWrapped(typical ? "Green is the best in the row. Typical is the middle player at %s, from %s, for a game this long."
			: "Green is the best in the row. These are counts and shares, not grades.", lobby >= 0 ? Match::LevelName(lobby) : "", "ranked replays");
		ImGui::PopStyleColor();
	}
	uiMatchKeepScroll = ImGui::GetScrollY();
}

// ---- The page ----
void RLCoach::RenderMatch()
{
	using namespace UiStyle;
	const float S = Sc();
	ImFont* fHead = uiFonts[UiFonts::Head];
	ImFont* fBig = uiFonts[UiFonts::Big];
	ImFont* fDisplay = uiFonts[UiFonts::Display];
	ImFont* fLabel = uiFonts[UiFonts::Label];
	auto onGame = [this](std::function<void()> fn) { gameWrapper->Execute([fn](GameWrapper*) { fn(); }); };
	UiMatch u;
	{
		std::lock_guard<std::mutex> lock(uiMatchMutex);
		u = uiMatch;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float W = ImGui::GetContentRegionAvail().x;

	std::shared_ptr<const MatchAnalysis> review;
	std::shared_ptr<const MatchData> reviewData;
	std::string loadedFile, loadError, openReplay;
	{
		std::lock_guard<std::mutex> lock(uiMatchMutex);
		review = uiMatchAnalysis;
		reviewData = uiMatchData;
		loadedFile = uiMatchLoadedFile;
		loadError = uiMatchLoadError;
		openReplay = uiMatchOpenReplay;
	}
	// the scans already saved, newest first (looked up when the page opens and after a scan)
	if (uiMatchFilesStale && writer)
	{
		uiMatchFilesStale = false;
		uiMatchFiles.clear();
		std::error_code ec;
		std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
		for (const auto& e : std::filesystem::directory_iterator(writer->Folder() / "matches", ec))
			if (e.is_regular_file(ec) && e.path().extension() == ".csv") found.push_back({ e.last_write_time(ec), e.path().filename().string() });
		std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
		for (auto& f : found) uiMatchFiles.push_back(std::move(f.second));
		// nothing showing yet: open the newest
		if (!review && !u.scanning && !uiMatchFiles.empty() && !matchLoadBusy) MatchLoad(uiMatchFiles.front());
	}
	if (!u.savedFile.empty() && std::find(uiMatchFiles.begin(), uiMatchFiles.end(), u.savedFile) == uiMatchFiles.end()) uiMatchFilesStale = true;
	const bool showReview = review && !u.scanning && !uiMatchShowScan;

	Wordmark(fDisplay, "Match review");
	{
		const char* other = showReview ? "Scan a replay" : "Back to the match";
		const bool canSwitch = showReview || (review && !u.scanning);
		const float need = ImGui::CalcTextSize("Back to reps").x + (canSwitch ? ImGui::CalcTextSize(other).x + 22.0f * S : 0.0f) + 16.0f * S;
		ImGui::SameLine(std::max(200.0f * S, ImGui::GetWindowContentRegionMax().x - need));
		if (canSwitch)
		{
			if (LinkButton(other)) uiMatchShowScan = showReview;
			ImGui::SameLine();
		}
		if (LinkButton("Back to reps"))
		{
			uiMatchMode = false;
			if (uiMatchRepsStale) { uiMatchRepsStale = false; RefreshRepList(); }   // a shot was saved as a rep
		}
	}
	ImGui::Spacing();
	if (showReview)
	{
		// who "you" are: the player the replay belongs to, when the scan knows; otherwise the first player
		if (uiMatchMeFor != loadedFile)
		{
			uiMatchMeFor = loadedFile;
			uiMatchMe = 0;
			uiMatchSel = -1;
			uiMatchShotFor = -1;
			uiMatchPick.clear();
			uiMatchFilter = 0;
			uiMatchZoom = 1.0f;
			uiMatchView0 = review->t0;
			for (const MatchPlayer& pl : review->players) if (!review->recordedBy.empty() && pl.name == review->recordedBy) uiMatchMe = pl.id;
		}
		const bool same = u.inReplay && !openReplay.empty() && openReplay == review->replayName;
		RenderMatchReview(*review, reviewData, same);
		if (uiMatchFiles.size() > 1)
		{
			Heading("OTHER SCANS");
			int listed = 0;
			for (const std::string& f : uiMatchFiles)
			{
				if (f == loadedFile || listed >= 6) continue;
				listed++;
				ImGui::PushID(f.c_str());
				if (LinkButton(f.substr(0, f.size() - 4).c_str())) MatchLoad(f);
				ImGui::PopID();
			}
		}
		return;
	}
	{
		FontPush f(fBig ? fBig : fHead);
		ImGui::TextColored(kText, "SCAN A REPLAY");
	}
	ImGui::PushStyleColor(ImGuiCol_Text, kSub);
	ImGui::TextWrapped("Step one of match review. Road2 plays a replay through once and writes down every car and the ball at every moment. "
		"The timeline, the grades and practising from any moment are built from that, and they're what comes next.");
	ImGui::PopStyleColor();
	ImGui::Spacing();

	// a numbered step: an orange chip, then the text
	auto step = [&](const char* n, bool done, const std::string& text) {
		const float fh = ImGui::GetFrameHeight();
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const float cw = 20.0f * S, chh = 18.0f * S;
		SlantQuad(dl, ImVec2(a.x, a.y + (fh - chh) * 0.5f), ImVec2(a.x + cw, a.y + (fh + chh) * 0.5f), U32(done ? kGood : kOrange));
		ImFont* cf = gCaps ? gCaps : ImGui::GetFont();
		const float cs = cf->FontSize * gUser;
		const ImVec2 ts = cf->CalcTextSizeA(cs, 1e9f, 0.0f, n);
		dl->AddText(cf, cs, ImVec2(a.x + (cw - ts.x) * 0.5f, a.y + (fh - ts.y) * 0.5f), U32(Hex(0x1A0E02)), n);
		ImGui::SetCursorScreenPos(ImVec2(a.x + cw + 10.0f * S, a.y));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - cw - 10.0f * S);
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, done ? kText : kSub);
		ImGui::TextWrapped("%s", text.c_str());
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
		ImGui::SetCursorScreenPos(ImVec2(a.x, std::max(ImGui::GetCursorScreenPos().y, a.y + fh) + 3.0f * S));
	};
	const bool saved = !u.savedFile.empty();
	step("1", u.inReplay, u.inReplay ? "A replay is open" + (u.replayName.empty() ? std::string() : ": " + u.replayName)
		: "Open a replay in Rocket League (main menu, Profile, Match History; any saved replay works)");
	step("2", u.scanning || saved, "Scan it. The replay restarts and plays through; leave it playing");
	step("3", saved, saved ? "Saved: matches\\" + u.savedFile : "The scan is saved to the matches folder");
	ImGui::Spacing();

	if (u.scanning)
	{
		// progress: a slanted bar, the frame count, and how fast it's going
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const float bh = 14.0f * S;
		const float frac = u.frames > 0 ? std::clamp(static_cast<float>(u.frame) / u.frames, 0.0f, 1.0f) : 0.0f;
		SlantQuad(dl, a, ImVec2(a.x + W, a.y + bh), U32(kTrack));
		if (frac > 0.01f) SlantQuad(dl, a, ImVec2(a.x + W * frac, a.y + bh), U32(kAccent));
		ImGui::Dummy(ImVec2(W, bh + 4.0f * S));
		{
			FontPush f(fLabel);
			const float left = u.rate > 1.0f && u.frames > u.frame ? (u.frames - u.frame) / u.rate : 0.0f;
			ImGui::TextColored(kMuted, "%s", std::format("FRAME {} OF {}   /   {:.0f} A SECOND{}", std::max(0, u.frame), u.frames, u.rate,
				left > 0.0f ? std::format("   /   ABOUT {:.0f} S LEFT", left) : std::string()).c_str());
		}
		ImGui::Spacing();
		if (OutlineButton("Stop and save what's read", ImVec2(220.0f * S, 34.0f * S))) onGame([this]() { MatchScanStop(true); });
		ImGui::SameLine(0, 10.0f * S);
		if (OutlineButton("Cancel", ImVec2(110.0f * S, 34.0f * S), kBad)) onGame([this]() { MatchScanStop(false); });
	}
	else
	{
		if (u.inReplay)
		{
			if (AccentButton(saved ? "Scan it again" : "Scan this replay", ImVec2(190.0f * S, 36.0f * S))) { uiMatchShowScan = false; onGame([this]() { MatchScanStart(); }); }
			ImGui::SameLine(0, 10.0f * S);
		}
		if (QuietButton("Open the matches folder", ImVec2(200.0f * S, 36.0f * S))) OpenDataSubfolder("matches");
	}
	if (!u.status.empty())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, saved ? kGood : kSub);
		ImGui::TextWrapped("%s", u.status.c_str());
		ImGui::PopStyleColor();
	}
	if (!loadError.empty()) ImGui::TextColored(kWarn, "%s", loadError.c_str());
	if (!u.scanning && !uiMatchFiles.empty())
	{
		Heading("SCANS YOU'VE SAVED");
		int listed = 0;
		for (const std::string& f : uiMatchFiles)
		{
			if (listed++ >= 8) break;
			ImGui::PushID(f.c_str());
			if (LinkButton(f.substr(0, f.size() - 4).c_str())) { MatchLoad(f); uiMatchShowScan = false; }
			ImGui::PopID();
		}
	}
	if (!u.players.empty() && u.scanning)
	{
		Heading("PLAYERS FOUND");
		for (const std::string& p : u.players)
			ImGui::TextColored(p.rfind("Blue", 0) == 0 ? kAccentText : p.rfind("Orange", 0) == 0 ? kOrange : kSub, "%s", p.c_str());
	}
}
