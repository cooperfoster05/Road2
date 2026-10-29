// The Combine (Oct 28): short scored events, each run as a set built from a seed. First event: saves.
// A set is 10 shots at your net. Every shot was checked in the simulator before you get it: it's on target, and a
// perfect car could reach it in time (Combine.cpp). How much spare time that perfect car has sets the level.
// Scoring per shot: a goal 0, a save 2, a save that clears it out of danger 3.
#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"
#include "UiFonts.h"
#include <fstream>
#include <random>
#include <sstream>

namespace
{
	constexpr int kCountdownTicks = 96;     // 0.8 s held in place before each shot
	constexpr int kResultTicks = 72;        // 0.6 s showing how the last shot went
	constexpr int kSettleTicks = 30;        // after your last touch, the ball has to stay out of danger this long
	constexpr int kShotTimeoutTicks = 720;  // 6 s
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
	u.event = combineEvent == "RC" ? "RECOVERY" : "SAVES";
	u.item = combineEvent == "RC" ? "Throw" : "Shot";
	for (int p : combinePoints) if (p >= 0) { u.points += p; u.maxPoints += 3; }
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
	combinePoints.clear();
	combineOutcomes.clear();
	combineIndex = 0;
	combinePhase = CombinePhase::Building;
	combineStatus = "Building your set (every one is checked in the simulator)...";
	CombinePublish();
	combineGenBusy = true;
	const uint32_t sd = combineSeed;
	const int lv = combineLevel;
	const bool rec = event == "RC";
	combineGenThread = std::thread([this, sd, lv, rec]() {
		std::vector<Combine::SaveShot> shots;
		std::vector<Combine::RecoveryThrow> throws;
		try
		{
			if (rec) throws = Combine::MakeRecoverySet(sd, lv, 10);
			else shots = Combine::MakeSaveSet(sd, lv, 10);
		}
		catch (...) { shots.clear(); throws.clear(); }
		combineGenBusy = false;
		gameWrapper->Execute([this, shots = std::move(shots), throws = std::move(throws)](GameWrapper*) mutable { CombineBegin(std::move(shots), std::move(throws)); });
	});
}

void RLCoach::CombineBegin(std::vector<Combine::SaveShot> shots, std::vector<Combine::RecoveryThrow> throws)
{
	if (combinePhase != CombinePhase::Building) return;   // stopped while it was building
	if (shots.empty() && throws.empty())
	{
		combinePhase = CombinePhase::Off;
		combineStatus = "Couldn't build a set from that code; try a new one";
		CombinePublish();
		return;
	}
	combineShots = std::move(shots);
	combineThrows = std::move(throws);
	combinePoints.assign(CombineCount(), 0);
	combineOutcomes.assign(CombineCount(), "");
	combineIndex = 0;
	combinePhase = CombinePhase::Countdown;
	combinePhaseTicks = 0;
	combineStatus.clear();
	frames.clear();
	state = RepState::Idle;
	PracticeGoalsOff();
	CombinePublish();
}

void RLCoach::CombineExit()
{
	if (combinePhase == CombinePhase::Off) return;
	combinePhase = CombinePhase::Off;
	combineShots.clear();
	combineThrows.clear();
	PracticeGoalsRestore();
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
		if (!bc.IsNull()) bc.SetCurrentBoostAmount(1.0f);
		ball.SetLocation(Vector(t.pos.x > 0 ? -3800.0f : 3800.0f, t.pos.y > 0 ? -4700.0f : 4700.0f, 93.0f));
		ball.SetVelocity(Vector(0, 0, 0));
		ball.SetAngularVelocity(Vector(0, 0, 0), false);
		return;
	}
	const Combine::SaveShot& s = combineShots[combineIndex];
	car.SetLocation(Vector(s.car.pos.x, s.car.pos.y, s.car.pos.z));
	car.SetRotation(Rotator(0, static_cast<int>(s.carYaw / kU2R), 0));
	car.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(s.car.vel.x, s.car.vel.y, s.car.vel.z));
	car.SetAngularVelocity(Vector(0, 0, 0), false);
	BoostWrapper bc = car.GetBoostComponent();
	if (!bc.IsNull()) bc.SetCurrentBoostAmount(1.0f);
	ball.SetLocation(Vector(s.ball.pos.x, s.ball.pos.y, s.ball.pos.z));
	ball.SetVelocity(frozen ? Vector(0, 0, 0) : Vector(s.ball.vel.x, s.ball.vel.y, s.ball.vel.z));
	ball.SetAngularVelocity(frozen ? Vector(0, 0, 0) : Vector(s.ball.angVel.x, s.ball.angVel.y, s.ball.angVel.z), false);
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
		if (combinePhaseTicks < kCountdownTicks) { CombinePlace(car, ball, true); return; }
		CombinePlace(car, ball, false);
		combinePhase = CombinePhase::Live;
		combineLiveTicks = 0;
		combineTouched = false;
		combineLastTouch = -1;
		combineRecContact = combineRecSettle = -1;
		combineRecGround = 0;
		combineRecBodyHit = false;
		combineRecPrevVel = car.GetVelocity();
		return;
	}
	if (combinePhase == CombinePhase::Result)
	{
		if (combinePhaseTicks < kResultTicks) return;
		combineIndex++;
		if (combineIndex >= CombineCount())
		{
			// The set is done: score, medal, bests
			combinePhase = CombinePhase::Done;
			PracticeGoalsRestore();
			int pts = 0;
			for (int p : combinePoints) if (p >= 0) pts += p;
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
		CombinePublish();
		return;
	}

	// ---- Live ----
	combineLiveTicks++;
	if (combineEvent == "RC")
	{
		// Recovery. 1 point each: your wheels touched first (not your roof or side), you were settled on them
		// quickly (within 0.3 s of when you could first have landed), and you kept your speed (1100+ just after).
		const Combine::RecoveryThrow& t = combineThrows[combineIndex];
		{
			const Vector bp = ball.GetLocation();
			const Vector park(t.pos.x > 0 ? -3800.0f : 3800.0f, t.pos.y > 0 ? -4700.0f : 4700.0f, 93.0f);
			if (std::fabs(bp.X - park.X) + std::fabs(bp.Y - park.Y) > 50.0f) { ball.SetLocation(park); ball.SetVelocity(Vector(0, 0, 0)); }
		}
		const Vector cv = car.GetVelocity();
		const bool wheels = car.IsOnGround();
		const Vector dv(cv.X - combineRecPrevVel.X, cv.Y - combineRecPrevVel.Y, cv.Z - combineRecPrevVel.Z);
		combineRecPrevVel = cv;
		if (combineRecContact < 0)
		{
			if (wheels) combineRecContact = combineLiveTicks;
			else if (combineLiveTicks > 3 && std::sqrt(dv.X * dv.X + dv.Y * dv.Y + dv.Z * dv.Z) > 300.0f) { combineRecContact = combineLiveTicks; combineRecBodyHit = true; }
		}
		combineRecGround = wheels ? combineRecGround + 1 : 0;
		if (combineRecGround >= 6 && combineRecSettle < 0) combineRecSettle = combineLiveTicks - 5;
		const int par = static_cast<int>(t.tHit * 120.0f) + 36;
		if (combineRecSettle >= 0 && combineLiveTicks >= combineRecSettle + 48)
		{
			const float speed = std::sqrt(cv.X * cv.X + cv.Y * cv.Y + cv.Z * cv.Z);
			const bool clean = !combineRecBodyHit, quick = combineRecSettle <= par, kept = speed >= 1100.0f;
			std::string what = clean ? "Wheels first" : "Body hit first";
			what += quick ? ", quick" : std::format(", slow (+{:.2f} s)", (combineRecSettle - par) / 120.0f + 0.30f);
			what += kept ? std::format(", kept speed ({:.0f})", speed) : std::format(", lost speed ({:.0f})", speed);
			CombineEndShot((clean ? 1 : 0) + (quick ? 1 : 0) + (kept ? 1 : 0), what);
			return;
		}
		if (combineLiveTicks >= 540) { CombineEndShot(0, "Never got settled on your wheels"); return; }
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
	int pts = 0, maxPts = 0;
	for (int i = 0; i < static_cast<int>(combinePoints.size()) && i <= combineIndex; i++)
		if (combinePoints[i] >= 0 && (i < combineIndex || combinePhase == CombinePhase::Result || combinePhase == CombinePhase::Done)) { pts += combinePoints[i]; maxPts += 3; }
	std::string top;
	if (combinePhase == CombinePhase::Building) top = "COMBINE  ·  building your set...";
	else if (combinePhase == CombinePhase::Done) top = std::format("COMBINE  ·  {}  ·  {}  ·  done: {} of {} points", combineEvent == "RC" ? "RECOVERY" : "SAVES", Combine::SaveLevelName(combineLevel), pts, maxPts);
	else top = std::format("COMBINE  ·  {}  ·  {}  ·  {} {} of {}  ·  {} points", combineEvent == "RC" ? "RECOVERY" : "SAVES", Combine::SaveLevelName(combineLevel), combineEvent == "RC" ? "throw" : "shot", combineIndex + 1, CombineCount(), pts);
	canvas.SetColor(255, 255, 255, 235);
	const Vector2F ts = canvas.GetStringSize(top, 1.6f, 1.6f);
	canvas.SetPosition(Vector2F{ (sz.X - ts.X) * 0.5f, sz.Y * 0.08f });
	canvas.DrawString(top, 1.6f, 1.6f, true);
	std::string mid;
	if (combinePhase == CombinePhase::Countdown) mid = std::format("{}", (kCountdownTicks - combinePhaseTicks) / 40 + 1);
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
	const float S = kScale;
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
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Share this code: it gives anyone the exact same 10 shots.");
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
		ImGui::SameLine();
		const std::string ev = u.event == "RECOVERY" ? "RC" : "SV";
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
	float slo, shi, rlo, rhi;
	Combine::SaveLevelBand(pick, slo, shi);
	Combine::RecoveryLevelBand(pick, rlo, rhi);
	eventCard("SV", "SAVES", "10 shots at your net. Every one is on target and was checked in the simulator: a perfect car could reach it "
		"in time. The level sets how much time that is. A save is 2 points, a save that clears it out of danger is 3.",
		std::format("{}: a perfect car has {:.2f}-{:.2f} s to spare", Combine::SaveLevelName(pick), slo, shi));
	eventCard("RC", "RECOVERY", "10 throws: you're launched into the air, tilted and spinning, toward the floor, a wall or the ceiling. "
		"1 point each for landing wheels first, being settled quickly, and keeping your speed (1100+). Every throw was checked: "
		"a perfect car can get its wheels round in time.",
		std::format("{}: {:.2f}-{:.2f} s to spare after the turn", Combine::SaveLevelName(pick), rlo, std::min(rhi, 2.5f)));

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
		if (Combine::ParseSetCode(uiCombineCode, ev, l, sd) && (ev == "SV" || ev == "RC")) onGame([this, ev, l, sd]() { CombineStart(ev, l, sd); });
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextColored(kMuted, "Coming next: Rings, Speed course, Boost pathing, Wall and backboard reads, First touches.");
	ImGui::TextColored(kMuted, "With a bot (testing now): 50s, Shadow defense.");
}
