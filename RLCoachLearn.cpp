#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"
#include "UiFonts.h"
#include "bakkesmod/wrappers/includes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>

// ============================================================
// Learn mode (Oct 26): a lesson teaches one mechanic from a fixed model rep, stage by stage (step 1, step 2,
// steps 1-2 together, ... the whole thing). Watching plays the model in the viewer; trying runs practice mode from the
// model's own car and ball at the stage's start. Each attempt gets a 1-5 score (Lesson.cpp); 3 of your last 5 at
// full speed scoring 4-5 unlocks the next stage, and auto ramp moves 50 / 75 / 90 / 100% after 2 in a row at 4-5.
// ============================================================

namespace
{
	std::string Lower(std::string s)
	{
		for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return s;
	}

	std::string Upper(std::string s)
	{
		for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		return s;
	}

	// A rep's CSV, same columns as the recorder writes (RepLibrary::ParseRep reads it back)
	std::string FramesCsv(const std::vector<Frame>& fr)
	{
		std::string s = "tick,t,wall_ms,throttle,steer,pitch,yaw,roll,jump,boost,handbrake,car_x,car_y,car_z,car_pitch,car_yaw,car_roll,"
			"car_vx,car_vy,car_vz,car_avx,car_avy,car_avz,boost_amount,on_ground,on_wall,has_flip,ball_x,ball_y,ball_z,ball_vx,ball_vy,ball_vz,"
			"ball_avx,ball_avy,ball_avz,touch,touch_x,touch_y,touch_z\n";
		s.reserve(fr.size() * 260);
		for (size_t i = 0; i < fr.size(); i++)
		{
			const Frame& f = fr[i];
			s += std::format("{},{:.4f},{:.1f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{:.2f},{:.2f},{:.2f},{},{},{},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},{:.1f},{},{},{},"
				"{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},{},{:.2f},{:.2f},{:.2f}\n",
				i, i / 120.0f, f.wallMs, f.throttle, f.steer, f.pitch, f.yaw, f.roll, f.jump ? 1 : 0, f.boost ? 1 : 0, f.handbrake ? 1 : 0,
				f.carX, f.carY, f.carZ, f.carPitch, f.carYaw, f.carRoll, f.carVX, f.carVY, f.carVZ, f.carAVX, f.carAVY, f.carAVZ, f.boostAmount,
				f.onGround ? 1 : 0, f.onWall ? 1 : 0, f.hasFlip ? 1 : 0, f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ,
				f.ballAVX, f.ballAVY, f.ballAVZ, f.touch ? 1 : 0, f.touchX, f.touchY, f.touchZ);
		}
		return s;
	}

	std::string ReadAll(const std::filesystem::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f) return {};
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}
}

void RLCoach::LearnPublish()
{
	std::lock_guard<std::mutex> lock(uiLearnMutex);
	UiLearn& u = uiLearn;
	u.loaded = learnLesson.valid();
	u.name = learnLesson.name;
	u.status = learnStatus;
	u.practicing = practiceState != PracticeState::Off;
	u.watching = learnWatching && viewerActive;
	u.autoRamp = learnAutoRamp;
	u.ghost = learnGhost;
	u.warmupOffered = learnWarmupOffered && !learnWarmupDone;
	u.warmup = learnWarmup;
	u.stageNames.clear(); u.stageFirst.clear(); u.stageLast.clear(); u.best.clear(); u.passed.clear(); u.recent.clear();
	for (size_t i = 0; i < learnLesson.stages.size(); i++)
	{
		u.stageNames.push_back(learnLesson.stages[i].name);
		u.stageFirst.push_back(learnLesson.stages[i].first);
		u.stageLast.push_back(learnLesson.stages[i].last);
		u.best.push_back(i < learnProgress.best.size() ? learnProgress.best[i] : 0);
		u.passed.push_back(learnProgress.PassedRecent(static_cast<int>(i)));
		std::vector<int> r;
		if (i < learnProgress.recentFull.size()) r.assign(learnProgress.recentFull[i].begin(), learnProgress.recentFull[i].end());
		u.recent.push_back(r);
	}
	u.steps = learnLesson.steps;
	u.unlocked = learnProgress.unlocked;
	u.completed = learnProgress.completed;
	u.stage = learnStage;
	u.speed = learnSpeed;
	u.last = learnLast;
	u.lastStage = learnLastStage;
	u.lastSpeed = learnLastSpeed;
	u.events = learnEvents;
	u.inList = learnInList;
	u.unlockAll = learnUnlockAll && *learnUnlockAll;
	u.lessons = learnList;
}

// The lesson list (Oct 28): every lesson in the skill tree with its progress, what it needs first, and whether it's built
void RLCoach::LearnOpen()
{
	if (!writer) return;
	// Leave whatever was going on: a rep in the viewer, a practice set, a lesson
	if (viewerActive && !learnWatching) ViewerExit();
	LearnStopWatch();
	PracticeExit();
	if (!learnActive && !learnInListOpen)
	{
		ServerWrapper server = gameWrapper->GetGameEventAsServer();
		learnSavedSpeed = server.IsNull() ? 1.0f : server.GetGameSpeed();
		savedGameSpeed = learnSavedSpeed;
	}
	else
	{
		ServerWrapper server = gameWrapper->GetGameEventAsServer();
		if (!server.IsNull()) server.SetGameSpeed(learnSavedSpeed);
	}
	learnActive = false;
	learnInList = true;
	learnInListOpen = true;
	learnStatus.clear();

	const std::filesystem::path dir = writer->Folder() / "lessons";
	const std::string progressCsv = ReadAll(dir / "progress.csv");
	const bool unlockAll = learnUnlockAll && *learnUnlockAll;
	std::vector<UiLessonEntry> list;
	std::unordered_map<std::string, bool> done;
	for (const LessonInfo& info : Lessons::Catalog())
	{
		UiLessonEntry en;
		en.id = info.id; en.name = info.name; en.blurb = info.blurb; en.built = info.built;
		if (info.built)
		{
			std::vector<Frame> model;
			std::string err;
			std::string csv = ReadAll(dir / (info.id + "_model.csv"));
			Lesson L = csv.empty() || !RepLibrary::ParseRep(csv, model, err) ? Lesson{} : Lessons::Build(info.id, model);
			en.built = L.valid();
			if (L.valid())
			{
				LessonProgress pr;
				pr.Fit(static_cast<int>(L.stages.size()));
				pr.Load(info.id, progressCsv);
				en.stages = static_cast<int>(L.stages.size());
				en.cleared = std::min(en.stages, pr.unlocked + (pr.completed ? 1 : 0));
				en.completed = pr.completed;
			}
		}
		done[info.id] = en.completed;
		list.push_back(en);
	}
	for (UiLessonEntry& en : list)
	{
		const LessonInfo* info = Lessons::Find(en.id);
		for (const std::string& need : info->needs)
			if (!done[need]) { const LessonInfo* ni = Lessons::Find(need); en.missing.push_back(ni ? ni->name : need); }
		en.locked = !en.missing.empty() && !unlockAll;
	}
	learnList = std::move(list);

	// The recorder sits out while Learn is open
	frames.clear();
	state = RepState::Idle;
	LearnPublish();
}

void RLCoach::LearnOpenLesson(const std::string& id)
{
	if (!writer) return;
	const LessonInfo* info = Lessons::Find(id);
	if (!info) return;
	bool locked = false;
	for (const UiLessonEntry& en : learnList) if (en.id == id) locked = en.locked || !en.built;
	if (locked) return;
	LearnStopWatch();
	PracticeExit();

	const std::filesystem::path dir = writer->Folder() / "lessons";
	learnLesson = Lesson{};
	learnStatus.clear();
	const std::string file = id + "_model.csv";
	std::string csv = ReadAll(dir / file);
	std::vector<Frame> model;
	std::string err;
	if (csv.empty()) learnStatus = "Couldn't find the lesson's model rep (data/RLCoach/lessons/" + file + ")";
	else if (!RepLibrary::ParseRep(csv, model, err)) learnStatus = "Couldn't read the lesson's model rep: " + err;
	else
	{
		learnLesson = Lessons::Build(id, model);
		if (!learnLesson.valid()) learnStatus = "The lesson's model rep isn't a " + Lower(info->name);
	}

	learnProgress = LessonProgress{};
	learnProgress.Fit(static_cast<int>(learnLesson.stages.size()));
	if (learnLesson.valid()) learnProgress.Load(learnLesson.id, ReadAll(dir / "progress.csv"));

	learnActive = learnLesson.valid();
	learnInList = !learnActive;
	learnStage = std::min(learnProgress.unlocked, std::max(0, static_cast<int>(learnLesson.stages.size()) - 1));   // pick up where you left off
	learnSpeed = learnAutoRamp ? LessonProgress::kRamp[0] : 1.0f;
	learnProgress.rampStreak = 0;
	learnLast = StageScore{};
	learnLastStage = -1;
	learnEvents.clear();
	// A quick full attempt opens each session once you've made progress (how you do a day later is the real test)
	learnWarmupOffered = learnProgress.unlocked > 0 && !learnWarmupSeen.count(id);
	learnWarmupDone = false;
	learnWarmupSeen.insert(id);
	learnWarmup = false;
	if (learnActive && !gameWrapper->IsInFreeplay()) learnStatus = "Open free play to watch and practice";
	frames.clear();
	state = RepState::Idle;
	LearnPublish();
}

void RLCoach::LearnSetUnlockAll(bool on)
{
	if (learnUnlockAll) *learnUnlockAll = on;
	cvarManager->getCvar("rlcoach_learn_unlock_all").setValue(on ? 1 : 0);
	if (learnInList) LearnOpen();   // refresh the locks
	else LearnPublish();
}

void RLCoach::LearnClose()
{
	LearnStopWatch();
	PracticeExit();
	learnActive = false;
	learnInList = false;
	learnInListOpen = false;
	learnWarmup = false;
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (!server.IsNull()) server.SetGameSpeed(learnSavedSpeed);
	havePrev = false;
	state = *recordingEnabled ? RepState::WaitReset : RepState::Idle;
	LearnPublish();
}

void RLCoach::LearnSelectStage(int stage)
{
	const bool unlockAll = learnUnlockAll && *learnUnlockAll;
	if (!learnActive || stage < 0 || (stage > learnProgress.unlocked && !unlockAll) || stage >= static_cast<int>(learnLesson.stages.size())) return;
	const bool wasPracticing = practiceState != PracticeState::Off;
	learnStage = stage;
	learnWarmup = false;
	learnLast = StageScore{};
	learnLastStage = -1;
	learnEvents.clear();
	learnProgress.rampStreak = 0;
	if (learnAutoRamp) learnSpeed = LessonProgress::kRamp[0];   // each stage ramps up from 50%
	if (wasPracticing) LearnTry();
	LearnPublish();
}

void RLCoach::LearnStopWatch()
{
	if (!learnWatching) return;
	learnWatching = false;
	learnWatchEnd = -1;
	viewerSpeed = uiViewerSpeed.load();   // the clip played at the lesson's speed; the rep viewer goes back to its own
	viewerActive = false;
	viewerPlaying = false;
	viewerFrames.clear();
	viewerSegs = RepSegments{};
	viewerGrade = RepGrade{};
}

void RLCoach::LearnWatch(bool whole)
{
	if (!learnActive) return;
	if (!gameWrapper->IsInFreeplay()) { learnStatus = "Open free play to watch the lesson"; LearnPublish(); return; }
	if (practiceState != PracticeState::Off) PracticeExit();
	const int stage = whole ? static_cast<int>(learnLesson.stages.size()) - 1 : learnStage;

	viewerFrames = learnLesson.model;
	viewerShot = learnLesson.modelShot;
	viewerSegs = Segmenter::Split(viewerFrames, -1);
	viewerGrade = RepGrade{};
	viewerRep = RepListItem{};
	viewerRep.savedAt = "Model";
	viewerRep.endReason = learnLesson.stages[stage].name;
	viewerPred.clear();
	viewerShowing = -1;
	viewerSpeed = std::max(0.25f, learnSpeed);
	viewerPlayhead = static_cast<float>(whole ? 0 : learnLesson.StageStart(stage));
	learnWatchEnd = whole ? static_cast<int>(viewerFrames.size()) - 1 : learnLesson.StageWatchEnd(stage);
	viewerPlaying = true;
	viewerActive = true;
	learnWatching = true;
	frames.clear();
	state = RepState::Idle;
	learnStatus.clear();
	LearnPublish();
}

void RLCoach::LearnTry()
{
	if (!learnActive) return;
	if (!gameWrapper->IsInFreeplay()) { learnStatus = "Open free play to practice"; LearnPublish(); return; }
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	LearnStopWatch();

	const bool restarting = practiceState != PracticeState::Off;
	if (!restarting) { practiceSavedGameSpeed = learnSavedSpeed; PracticeGoalsOff(); }
	else
	{
		CarWrapper car = gameWrapper->GetLocalCar();
		if (!car.IsNull()) PracticeReleaseFlip(car);
	}
	viewerPlaying = false;
	uiViewerPlaying = false;

	const int start = learnLesson.StageStart(learnStage);
	practiceRep = learnLesson.model;
	practiceItem = RepListItem{};
	practiceKind = SegKind::Approach;
	practiceSegStart = start;
	practiceTargetTick = start;
	practiceLabel = std::format("Learn: {}{}", learnLesson.stages[learnStage].name, learnWarmup ? " (warm-up)" : "");
	practiceShot = learnLesson.modelShot;
	practiceBallStartTick = learnLesson.StageStart(0);
	PracticeSetLeadIn(0.0f);

	practiceAttempts = 0;
	practiceHitsByPct.clear();
	practiceHitsTotal = 0;
	practiceStreak = 0;
	practiceResultLines.clear();
	practiceLive.clear();
	practiceState = PracticeState::Frozen;
	practiceFrozenTicks = 0;
	practiceAutoRamp = false;   // the lesson runs its own ramp
	PracticeSetSpeed(learnSpeed);
	uiPracticeActive = true;
	uiPracticeTarget = start;

	// The model's path drawn faintly, and its inputs on the controller ghost
	practiceRefFrames = learnLesson.model;
	practiceRefLabel = "Faint line: the model " + Lower(learnLesson.name);
	practiceGhostLine = learnGhost;
	practiceFineSpans.clear();

	frames.clear();
	state = RepState::Idle;
	learnStatus.clear();
	LearnPublish();
}

void RLCoach::LearnSetSpeed(float speed)
{
	learnAutoRamp = false;
	learnSpeed = std::clamp(speed, 0.1f, 1.0f);
	learnProgress.rampStreak = 0;
	if (practiceState != PracticeState::Off) PracticeSetSpeed(learnSpeed);
	if (learnWatching) viewerSpeed = learnSpeed;
	LearnPublish();
}

void RLCoach::LearnSetAutoRamp(bool on)
{
	learnAutoRamp = on;
	learnProgress.rampStreak = 0;
	if (on)
	{
		learnSpeed = LessonProgress::kRamp[0];
		if (practiceState != PracticeState::Off) PracticeSetSpeed(learnSpeed);
	}
	LearnPublish();
}

void RLCoach::LearnSetGhost(bool on)
{
	learnGhost = on;
	if (practiceState != PracticeState::Off) practiceGhostLine = on;
	LearnPublish();
}

void RLCoach::LearnStartWarmup()
{
	if (!learnActive) return;
	// One full-speed attempt at the furthest "together" stage you've reached (or step 1)
	int target = 0;
	for (int i = 0; i <= learnProgress.unlocked && i < static_cast<int>(learnLesson.stages.size()); i++)
		if (learnLesson.stages[i].first == 0) target = i;
	learnWarmupReturn = learnStage;
	learnStage = target;
	learnWarmup = true;
	learnWarmupDone = true;
	learnSpeed = 1.0f;
	LearnTry();
}

void RLCoach::LearnSkipWarmup()
{
	learnWarmupDone = true;
	LearnPublish();
}

void RLCoach::LearnBeginLive()
{
	const int start = std::min(practiceStartTick, static_cast<int>(learnLesson.model.size()));
	learnFull.assign(learnLesson.model.begin(), learnLesson.model.begin() + start);
	learnFull.reserve(learnLesson.model.size() + 480);
}

bool RLCoach::LearnStageDone(const Frame& f)
{
	learnFull.push_back(f);
	return Lessons::StageDone(learnLesson, learnStage, learnFull, practiceStartTick);
}

void RLCoach::LearnOnAttempt(const std::string& reason)
{
	practiceResultLines.clear();
	const bool counted = reason != "reset" && practiceLive.size() > 10 && !learnFull.empty();
	if (!counted)
	{
		practiceResultLines.push_back({ { { "Reset: not counted", 160, 160, 160 } } });
		LearnPublish();
		return;
	}
	StageScore sc = Lessons::Score(learnLesson, learnStage, learnFull, practiceStartTick, practiceWentIn, practiceFloorFirst, ShotReference{});
	LearnLogAttempt(sc);
	learnLast = sc;
	learnLastStage = learnStage;
	learnLastSpeed = practiceSpeed;
	learnEvents.clear();
	practiceAttempts++;

	if (learnWarmup)
	{
		// The warm-up only tells you where you are; then it's back to the stage you were on
		learnEvents.push_back(std::format("Warm-up: {} on {}", sc.score, learnLesson.stages[learnStage].name));
		learnWarmup = false;
		learnStage = learnWarmupReturn;
		learnSpeed = learnAutoRamp ? LessonProgress::kRamp[0] : 1.0f;
		LearnTry();   // sets up the stage you came from (and publishes)
		learnLast = sc;
		learnLastStage = -1;
	}
	else
	{
		LessonProgress::Change ch = learnProgress.Record(learnStage, sc.score, practiceSpeed, learnAutoRamp);
		if (ch.rampedUp)
		{
			learnSpeed = ch.speed;
			PracticeSetSpeed(learnSpeed);
			learnEvents.push_back(std::format("2 in a row: speeding up to {}%", static_cast<int>(std::lround(learnSpeed * 100.0f))));
		}
		if (ch.unlockedNext)
			learnEvents.push_back("Unlocked: " + learnLesson.stages[learnProgress.unlocked].name);
		if (ch.completed)
			learnEvents.push_back("Lesson complete: you passed the whole " + Lower(learnLesson.name) + "!");
		LearnSaveProgress();
	}

	// The in-game panel's lines
	PanelGradeLine head;
	head.grade = sc.score;
	head.chunks.push_back({ std::format("{}  ", sc.score), 240, 244, 250 });
	head.chunks.push_back({ learnLesson.stages[learnLastStage >= 0 ? learnLastStage : learnStage].name, 190, 190, 190 });
	practiceResultLines.push_back(head);
	for (size_t i = 0; i < sc.lines.size() && i < 3; i++) practiceResultLines.push_back({ { { sc.lines[i], 200, 200, 200 } } });
	for (const std::string& e : learnEvents) practiceResultLines.push_back({ { { e, 120, 200, 255 } } });
	LearnPublish();
}

// Every scored attempt is kept (model time, the model's lead-in included) so the scoring can be tuned on real
// lesson runs: lessons/attempts/<lesson>_s<stage>_<when>_<score>_<speed>.csv, the newest 400
void RLCoach::LearnLogAttempt(const StageScore& sc)
{
	if (!writer || learnFull.empty()) return;
	std::error_code ec;
	const std::filesystem::path dir = writer->Folder() / "lessons" / "attempts";
	std::filesystem::create_directories(dir, ec);
	const auto now = std::chrono::system_clock::now();
	const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	const std::string name = std::format("{}_s{}_{}_{}_{}{}{}.csv", learnLesson.id, learnStage, ms, sc.score,
		static_cast<int>(std::lround(practiceSpeed * 100.0f)), practiceWentIn ? "_goal" : "", learnWarmup ? "_warmup" : "");
	std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
	out << FramesCsv(learnFull);
	out.close();
	// Keep the newest 400
	std::vector<std::filesystem::path> files;
	for (const auto& de : std::filesystem::directory_iterator(dir, ec)) if (de.path().extension() == ".csv") files.push_back(de.path());
	if (files.size() > 400)
	{
		std::sort(files.begin(), files.end());   // names sort by lesson then stage then time, so sort by time below
		std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
			std::error_code e1, e2;
			return std::filesystem::last_write_time(a, e1) < std::filesystem::last_write_time(b, e2);
		});
		for (size_t i = 0; i + 400 < files.size(); i++) std::filesystem::remove(files[i], ec);
	}
}

void RLCoach::LearnSaveProgress()
{
	if (!writer || !learnLesson.valid()) return;
	std::error_code ec;
	const std::filesystem::path dir = writer->Folder() / "lessons";
	std::filesystem::create_directories(dir, ec);
	// Keep other lessons' lines, replace this one's
	std::string keep;
	{
		std::istringstream in(ReadAll(dir / "progress.csv"));
		std::string line;
		while (std::getline(in, line))
			if (!line.empty() && line.rfind(learnLesson.id + ",", 0) != 0) keep += line + "\n";
	}
	std::ofstream out(dir / "progress.csv.tmp", std::ios::binary | std::ios::trunc);
	out << keep << learnProgress.Save(learnLesson.id);
	out.close();
	if (out) std::filesystem::rename(dir / "progress.csv.tmp", dir / "progress.csv", ec);
}

// ============================================================
// The lesson screen (render thread)
// ============================================================

namespace
{
	struct FontPush
	{
		explicit FontPush(ImFont* f) { ImGui::PushFont(f); }
		~FontPush() { ImGui::PopFont(); }
	};
}

void RLCoach::RenderLearn()
{
	using namespace UiStyle;
	const float S = kScale;
	ImFont* fLabel = uiFonts[UiFonts::Label];
	ImFont* fBold = uiFonts[UiFonts::Bold];
	ImFont* fHead = uiFonts[UiFonts::Head];
	ImFont* fBig = uiFonts[UiFonts::Big];
	auto onGame = [this](std::function<void()> fn) { gameWrapper->Execute([fn](GameWrapper*) { fn(); }); };
	auto label = [&](const char* text, ImVec4 color) {
		FontPush f(fLabel);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	};
	auto wrapped = [&](const std::string& text, ImVec4 color) {
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextWrapped("%s", text.c_str());
		ImGui::PopStyleColor();
	};

	UiLearn u;
	{
		std::lock_guard<std::mutex> lock(uiLearnMutex);
		u = uiLearn;
	}
	if (u.inList)
	{
		RenderLearnList();
		return;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float W = ImGui::GetContentRegionAvail().x;

	// ---- Title row ----
	{
		FontPush f(fLabel);
		ImGui::TextColored(kAccent, "ROAD2");
		ImGui::SameLine();
		ImGui::TextColored(kMuted, "LEARN");
	}
	ImGui::SameLine(std::max(200.0f * S, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("All lessons").x - ImGui::CalcTextSize("Back to reps").x - 34.0f * S));
	if (LinkButton("All lessons")) onGame([this]() { LearnOpen(); });
	ImGui::SameLine();
	if (LinkButton("Back to reps"))
	{
		uiLearnMode = false;
		onGame([this]() { LearnClose(); });
	}

	if (!u.loaded)
	{
		ImGui::Spacing();
		wrapped(u.status.empty() ? "Loading the lesson..." : u.status, kSub);
		return;
	}
	const int nStages = static_cast<int>(u.stageNames.size());
	const int stage = std::clamp(u.stage, 0, nStages - 1);

	// ---- Lesson header: name and how far along you are ----
	ImGui::Spacing();
	{
		FontPush f(fHead);
		ImGui::TextColored(kText, "%s", Upper(u.name).c_str());
	}
	{
		const int done = std::min(u.unlocked + (u.completed ? 1 : 0), nStages);
		ImGui::SameLine();
		if (u.completed) { ImGui::TextColored(kGood, "  COMPLETE"); ImGui::SameLine(); }
		ImGui::TextColored(kMuted, "  %d of %d stages cleared", done, nStages);
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float h = 4.0f * S;
		dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), U32(kTrack), h);
		dl->AddRectFilled(p, ImVec2(p.x + W * done / std::max(1, nStages), p.y + h), U32(kAccent), h);
		ImGui::Dummy(ImVec2(W, h + 4.0f * S));
	}
	if (!u.status.empty()) wrapped(u.status, kWarn);

	// ---- Lesson complete ----
	if (u.completed)
	{
		ImGui::Spacing();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float pad = 8.0f * S;
		ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
		ImGui::BeginGroup();
		{
			FontPush f(fBold);
			ImGui::TextColored(kGood, "Lesson complete");
		}
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + W - 2.0f * pad);
		ImGui::TextColored(kSub, "You passed every stage, the whole %s included. Every stage stays open to keep it sharp, and the warm-up check still opens each session.", Lower(u.name).c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p0, p1, U32(kGood, 0.08f), 8.0f * S);
		dl->AddRect(p0, p1, U32(kGood, 0.6f), 8.0f * S);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
		ImGui::Dummy(ImVec2(W, 2.0f * S));
	}

	// ---- Warm-up check ----
	if (u.warmupOffered)
	{
		// A card that fits its contents: draw the contents first, then the background behind them
		ImGui::Spacing();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float pad = 8.0f * S;
		ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + W - 2.0f * pad);
		ImGui::TextColored(kSub, "Warm-up check: one full-speed run of the furthest part you've reached, before picking up where you left off.");
		ImGui::PopTextWrapPos();
		if (AccentButton("Start warm-up")) onGame([this]() { LearnStartWarmup(); });
		ImGui::SameLine();
		if (OutlineButton("Skip")) onGame([this]() { LearnSkipWarmup(); });
		ImGui::EndGroup();
		const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p0, p1, U32(kPanel), 8.0f * S);
		dl->AddRect(p0, p1, U32(kLine), 8.0f * S);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
		ImGui::Dummy(ImVec2(W, 2.0f * S));
	}

	// ---- The stages ----
	Heading("STAGES");
	for (int i = 0; i < nStages; i++)
	{
		const bool locked = i > u.unlocked && !u.unlockAll, current = i == stage, cleared = i < u.unlocked || (u.completed && i == nStages - 1);
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float rowH = 22.0f * S;
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("stage", ImVec2(W, rowH)) && !locked) onGame([this, i]() { LearnSelectStage(i); });
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		if (current) dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH), U32(kPanel), 6.0f * S);
		else if (hov && !locked) dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH), U32(kPanel2), 6.0f * S);
		// Status dot: cleared (green check-dot), current (accent ring), locked (dim)
		ImVec2 c(p.x + 11.0f * S, p.y + rowH * 0.5f);
		if (cleared && current) { dl->AddCircleFilled(c, 5.0f * S, U32(kGood)); dl->AddCircle(c, 7.5f * S, U32(kAccent), 16, 1.5f * S); }
		else if (cleared) dl->AddCircleFilled(c, 5.0f * S, U32(kGood));
		else if (current) dl->AddCircle(c, 5.0f * S, U32(kAccent), 16, 2.0f * S);
		else dl->AddCircle(c, 5.0f * S, U32(kLine), 16, 1.5f * S);
		const bool together = u.stageFirst[i] != u.stageLast[i];
		ImVec4 tc = locked ? kMuted : current ? kText : kSub;
		dl->AddText(ImVec2(p.x + 24.0f * S + (together ? 10.0f * S : 0.0f), p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), U32(tc),
			u.stageNames[i].c_str());
		// Right side: best score at full speed, or the lock
		std::string right = locked ? "locked" : u.best[i] > 0 ? std::format("best {}", u.best[i]) : "";
		if (!right.empty())
		{
			ImVec2 ts = ImGui::CalcTextSize(right.c_str());
			dl->AddText(ImVec2(p.x + W - ts.x - 8.0f * S, p.y + (rowH - ts.y) * 0.5f), U32(locked ? kMuted : GradeColor(u.best[i])), right.c_str());
		}
		if (hov && locked) ImGui::SetTooltip("Clear the stage before it first (3 of your last 5 at full speed scoring 4-5)");
	}

	// ---- The current stage: what, why, how ----
	Heading(Upper(u.stageNames[stage]).c_str());
	{
		const int first = u.stageFirst[stage], last = u.stageLast[stage];
		if (first == last)
		{
			const LessonStep& st = u.steps[first];
			wrapped(st.what, kText);
			ImGui::Spacing();
			label("WHY IT MATTERS", kMuted);
			wrapped(st.why, kSub);
			ImGui::Spacing();
			label("BEST PRACTICES", kMuted);
			for (const std::string& t : st.tips)
			{
				ImGui::TextColored(kAccentText, "-");
				ImGui::SameLine();
				wrapped(t, kSub);
			}
			ImGui::Spacing();
			label("FOCUS ON", kMuted);
			{
				FontPush f(fBold);
				wrapped(st.cue, kAccent);
			}
		}
		else
		{
			wrapped(last == static_cast<int>(u.steps.size()) - 1 && first == 0
				? std::format("The whole thing, start to finish. Graded like any {} rep; each step is scored too, so you can see which one to work on.", Lower(u.name))
				: std::format("Put steps {}-{} together in one go. Each step is scored; your score is the lowest of them.", first + 1, last + 1), kText);
			ImGui::Spacing();
			label("ONE CUE PER STEP", kMuted);
			for (int s = first; s <= last; s++)
			{
				ImGui::TextColored(kMuted, "%d", s + 1);
				ImGui::SameLine();
				wrapped(u.steps[s].name + ": " + u.steps[s].cue, kSub);
			}
		}
	}

	// ---- Controls ----
	ImGui::Spacing();
	if (AccentButton(u.practicing ? "Retry" : "Try it", ImVec2(96.0f * S, 0)))
		onGame([this, p = u.practicing]() { if (p) PracticeRetry(); else LearnTry(); });
	ImGui::SameLine();
	if (OutlineButton("Watch")) onGame([this]() { LearnWatch(false); });
	ImGui::SameLine();
	if (OutlineButton("Watch the whole thing")) onGame([this]() { LearnWatch(true); });
	if (u.practicing)
	{
		ImGui::SameLine();
		if (OutlineButton("Stop", ImVec2(0, 0), kMuted)) onGame([this]() { PracticeExit(); LearnPublish(); });
	}
	if (stage < u.unlocked)
	{
		ImGui::SameLine();
		if (LinkButton("Next stage")) onGame([this, stage]() { LearnSelectStage(stage + 1); });
	}

	// ---- Speed: a slider, or the auto ramp ----
	Heading("SPEED");
	{
		int pct = static_cast<int>(std::lround(u.speed * 100.0f));
		ImGui::PushItemWidth(std::min(W * 0.55f, 220.0f * S));
		if (ImGui::SliderInt("##speed", &pct, 25, 100, "%d%%"))
		{
			pct = std::clamp(static_cast<int>(std::lround(pct / 5.0)) * 5, 25, 100);
			const float sp = pct / 100.0f;
			onGame([this, sp]() { LearnSetSpeed(sp); });
		}
		ImGui::PopItemWidth();
		ImGui::SameLine();
		bool ramp = u.autoRamp;
		if (ImGui::Checkbox("Auto ramp", &ramp)) onGame([this, ramp]() { LearnSetAutoRamp(ramp); });
		if (u.autoRamp)
		{
			// 50 / 75 / 90 / 100, the current one lit
			for (int k = 0; k < 4; k++)
			{
				const float r = LessonProgress::kRamp[k];
				const bool here = std::fabs(r - u.speed) < 0.01f, past = r < u.speed - 0.01f;
				if (k) ImGui::SameLine();
				Pill(std::format("{}%", static_cast<int>(r * 100.0f + 0.5f)), here ? kAccent : past ? kGood : kMuted, 44.0f * S);
			}
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "  2 in a row at 4-5 moves up");
		}
	}

	// ---- Unlocking the next stage ----
	if (stage == u.unlocked && !(stage + 1 >= nStages && u.completed))
	{
		Heading(stage + 1 < nStages ? "TO UNLOCK THE NEXT STAGE" : "TO COMPLETE THE LESSON");
		ImGui::TextColored(kSub, "3 of your last 5 at 100%% scoring 4-5:");
		ImGui::SameLine();
		const std::vector<int>& r = u.recent[stage];
		for (int k = 0; k < LessonProgress::kUnlockOf; k++)
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImVec2 c(p.x + 7.0f * S, p.y + ImGui::GetTextLineHeight() * 0.5f);
			if (k < static_cast<int>(r.size())) dl->AddCircleFilled(c, 5.0f * S, U32(GradeColor(r[k])));
			else dl->AddCircle(c, 5.0f * S, U32(kLine), 16, 1.5f * S);
			ImGui::Dummy(ImVec2(16.0f * S, ImGui::GetTextLineHeight()));
			ImGui::SameLine();
		}
		ImGui::TextColored(u.passed[stage] >= LessonProgress::kUnlockNeed ? kGood : kMuted, "%d/3", std::min(u.passed[stage], 3));
		if (u.speed < 0.999f) ImGui::TextColored(kMuted, "Attempts below 100%% count toward the ramp, not the unlock.");
	}

	// ---- Ghost ----
	{
		bool ghost = u.ghost;
		if (ImGui::Checkbox("Show the model's inputs on the controller ghost", &ghost)) onGame([this, ghost]() { LearnSetGhost(ghost); });
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its opacity is the \"Controller ghost\" slider in the plugin settings.");
	}

	// ---- The last attempt ----
	if (u.last.score > 0)
	{
		Heading("LAST ATTEMPT");
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float r = 22.0f * S;
		ImVec2 c(p.x + r + 4.0f * S, p.y + r + 4.0f * S);
		Gauge(dl, c, r, 5.0f * S, u.last.score, GradeColor(u.last.score));
		{
			// The score in the middle of the gauge
			std::string n = std::to_string(u.last.score);
			ImFont* font = fBig ? fBig : ImGui::GetFont();
			const float size = fBig ? fBig->FontSize * 0.62f : ImGui::GetFontSize() * 1.4f;
			ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.0f, n.c_str());
			dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), U32(GradeColor(u.last.score)), n.c_str());
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x + 2.0f * r + 16.0f * S, p.y));
		ImGui::BeginGroup();
		ImGui::TextColored(kMuted, "at %d%% speed", static_cast<int>(std::lround(u.lastSpeed * 100.0f)));
		for (size_t i = 0; i < u.last.lines.size() && i < 5; i++) wrapped(u.last.lines[i], i == 0 ? kText : kSub);
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetCursorScreenPos().y, p.y + 2.0f * r + 10.0f * S)));
	}
	for (const std::string& e : u.events) ImGui::TextColored(kAccentText, "%s", e.c_str());

	if (u.practicing) ImGui::TextColored(kMuted, "Move to start each attempt. Free play's reset button counts as a retry.");
	else if (u.watching) ImGui::TextColored(kMuted, "Watching the model. Press Try it when you're ready.");
}

// The lesson list: the skill tree as a list for now (a drawn tree comes with the design pass)
void RLCoach::RenderLearnList()
{
	using namespace UiStyle;
	const float S = kScale;
	ImFont* fLabel = uiFonts[UiFonts::Label];
	ImFont* fHead = uiFonts[UiFonts::Head];
	ImFont* fBold = uiFonts[UiFonts::Bold];
	auto onGame = [this](std::function<void()> fn) { gameWrapper->Execute([fn](GameWrapper*) { fn(); }); };
	UiLearn u;
	{
		std::lock_guard<std::mutex> lock(uiLearnMutex);
		u = uiLearn;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float W = ImGui::GetContentRegionAvail().x;

	{
		FontPush f(fLabel);
		ImGui::TextColored(kAccent, "ROAD2");
		ImGui::SameLine();
		ImGui::TextColored(kMuted, "LEARN");
	}
	ImGui::SameLine(std::max(200.0f * S, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Back to reps").x - 16.0f * S));
	if (LinkButton("Back to reps"))
	{
		uiLearnMode = false;
		onGame([this]() { LearnClose(); });
	}
	ImGui::Spacing();
	{
		FontPush f(fHead);
		ImGui::TextColored(kText, "LESSONS");
	}
	ImGui::PushStyleColor(ImGuiCol_Text, kSub);
	ImGui::TextWrapped("Each lesson teaches one mechanic step by step. Some build on others: finish what they need first.");
	ImGui::PopStyleColor();
	if (u.lessons.empty()) { ImGui::TextColored(kMuted, "Loading..."); return; }

	ImGui::Spacing();
	for (size_t i = 0; i < u.lessons.size(); i++)
	{
		const UiLessonEntry& en = u.lessons[i];
		const bool open = en.built && !en.locked;
		const float rowH = 46.0f * S;
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::PushID(static_cast<int>(i));
		const bool clicked = ImGui::InvisibleButton("lesson", ImVec2(W, rowH));
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		if (clicked && open) onGame([this, id = en.id]() { LearnOpenLesson(id); });
		dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH - 4.0f * S), U32(hov && open ? kPanel : kPanel2), 8.0f * S);
		if (en.completed) dl->AddRect(p, ImVec2(p.x + W, p.y + rowH - 4.0f * S), U32(kGood, 0.5f), 8.0f * S);

		// Status mark: done, in progress, locked, coming soon
		ImVec2 c(p.x + 16.0f * S, p.y + (rowH - 4.0f * S) * 0.5f);
		if (en.completed) dl->AddCircleFilled(c, 6.0f * S, U32(kGood));
		else if (!en.built) dl->AddCircle(c, 6.0f * S, U32(kLineSoft), 16, 1.5f * S);
		else if (en.locked) dl->AddCircle(c, 6.0f * S, U32(kMuted), 16, 1.5f * S);
		else dl->AddCircle(c, 6.0f * S, U32(kAccent), 16, 2.0f * S);

		const float tx = p.x + 32.0f * S;
		const ImVec4 nameCol = !en.built ? kMuted : en.locked ? kSub : kText;
		dl->AddText(fBold ? fBold : ImGui::GetFont(), fBold ? fBold->FontSize : ImGui::GetFontSize(), ImVec2(tx, p.y + 6.0f * S), U32(nameCol), en.name.c_str());
		std::string sub = en.blurb;
		if (en.locked && !en.missing.empty())
		{
			sub = "Needs ";
			for (size_t k = 0; k < en.missing.size(); k++) sub += (k ? (k + 1 == en.missing.size() ? " and " : ", ") : "") + en.missing[k];
		}
		dl->AddText(ImVec2(tx, p.y + 24.0f * S), U32(en.locked ? kWarn : kMuted, en.locked ? 0.85f : 1.0f), sub.c_str());

		std::string right = !en.built ? "coming soon" : en.completed ? "complete" : en.locked ? "locked"
			: en.cleared > 0 ? std::format("{} of {} stages", en.cleared, en.stages) : "start";
		ImVec4 rc = !en.built ? kMuted : en.completed ? kGood : en.locked ? kMuted : kAccentText;
		ImVec2 ts = ImGui::CalcTextSize(right.c_str());
		dl->AddText(ImVec2(p.x + W - ts.x - 12.0f * S, p.y + 6.0f * S), U32(rc), right.c_str());
		if (hov && en.locked) ImGui::SetTooltip("Complete what this lesson needs first. (Or turn the locks off below: for practicing, not learning.)");
		if (hov && !en.built) ImGui::SetTooltip("Not built yet: it needs a model rep and its scoring.");
	}

	Heading("LOCKS");
	bool all = u.unlockAll;
	if (ImGui::Checkbox("Unlock every lesson and stage", &all)) onGame([this, all]() { LearnSetUnlockAll(all); });
	ImGui::PushStyleColor(ImGuiCol_Text, all ? kWarn : kMuted);
	ImGui::TextWrapped("Locks keep you learning things in an order that builds on itself. Turning them off is for practicing, not learning: "
		"you can open anything, but skipping ahead is the slow way to actually get good.");
	ImGui::PopStyleColor();
}
