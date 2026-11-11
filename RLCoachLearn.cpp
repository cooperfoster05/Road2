#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"
#include "UiFonts.h"
#include "bakkesmod/wrappers/includes.h"
#include "Overlay3D.h"

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
	u.moveLesson = learnLesson.kind == LessonKind::Move;
	u.status = learnStatus;
	u.practicing = practiceState != PracticeState::Off;
	u.watching = learnWatching && viewerActive;
	u.autoRamp = learnAutoRamp;
	u.ghost = learnGhost;
	u.warmupOffered = learnWarmupOffered && !learnWarmupDone;
	u.warmup = learnWarmup;
	u.stageNames.clear(); u.stageFirst.clear(); u.stageLast.clear(); u.best.clear(); u.passed.clear(); u.recent.clear(); u.stageWhole.clear();
	u.notes = learnLesson.notes;
	for (int k = 0; k < 3; k++) u.stars[k] = learnLesson.koStars[k];
	u.starsWho = learnLesson.koStarsWho;
	u.testOut = learnTestOut;
	u.testOutScores = learnTestOutScores;
	{
		const LessonInfo* info = Lessons::Find(learnLesson.id);
		u.canTestOut = info && info->TestOut() && !learnProgress.completed && !learnTestOut;
	}
	for (size_t i = 0; i < learnLesson.stages.size(); i++)
	{
		u.stageNames.push_back(learnLesson.stages[i].name);
		u.stageFirst.push_back(learnLesson.stages[i].first);
		u.stageLast.push_back(learnLesson.stages[i].last);
		u.stageWhole.push_back(learnLesson.stages[i].whole ? 1 : 0);
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
		en.id = info.id; en.name = info.name; en.blurb = info.blurb; en.built = info.built; en.tier = info.tier;
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
				en.testedOut = pr.testOut == 2;
				en.best = pr.best.empty() ? 0 : pr.best.back();
			}
		}
		done[info.id] = en.completed;
		list.push_back(en);
	}
	std::unordered_map<std::string, bool> built;
	for (const UiLessonEntry& en : list) built[en.id] = en.built;
	for (UiLessonEntry& en : list)
	{
		const LessonInfo* info = Lessons::Find(en.id);
		en.needs = info->needs;
		// (Nov 2) A prerequisite that isn't built yet doesn't lock anything, and a lesson you've already made progress on
		// stays open when the tree gives it new prerequisites (air dribble, double tap and ceiling shot did)
		for (const std::string& need : info->needs)
			if (!done[need] && built[need]) { const LessonInfo* ni = Lessons::Find(need); en.missing.push_back(ni ? ni->name : need); }
		en.locked = !en.missing.empty() && !unlockAll && !en.completed && en.cleared == 0;
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
	if (csv.empty()) learnStatus = "Couldn't find the lesson's model rep (data/Road2/lessons/" + file + ")";
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
	// Tiers 1-2 (Nov 2): a lesson you haven't started opens on its test-out, the whole thing at full speed
	learnTestOut = false;
	learnTestOutScores.clear();
	if (learnActive && info->TestOut() && !learnProgress.completed && learnProgress.testOut == 0 && learnProgress.unlocked == 0)
	{
		learnTestOut = true;
		learnStage = static_cast<int>(learnLesson.stages.size()) - 1;
		learnSpeed = 1.0f;
	}
	if (learnActive && !gameWrapper->IsInFreeplay()) learnStatus = "Open free play to watch and practice";
	frames.clear();
	state = RepState::Idle;
	LearnPublish();
}

void RLCoach::LearnSetUnlockAll(bool on)
{
	if (learnUnlockAll) *learnUnlockAll = on;
	cvarManager->getCvar("road2_learn_unlock_all").setValue(on ? 1 : 0);
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
	if (learnTestOut && stage != static_cast<int>(learnLesson.stages.size()) - 1) LearnEndTestOut(false);
	if (!learnActive || stage < 0 || (stage > learnProgress.unlocked && !unlockAll && !learnTestOut) || stage >= static_cast<int>(learnLesson.stages.size())) return;
	const bool wasPracticing = practiceState != PracticeState::Off;
	learnStage = stage;
	learnWarmup = false;
	learnLast = StageScore{};
	learnLastStage = -1;
	learnEvents.clear();
	learnProgress.rampStreak = 0;
	if (learnAutoRamp && !learnTestOut) learnSpeed = LessonProgress::kRamp[0];   // each stage ramps up from 50%
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
	const int ride = learnLesson.StageRideFrom(learnStage);
	practiceRideUntil = ride >= 0 ? start : -1;
	practiceRideByInput = ride >= 0 && learnLesson.StageRideByInput(learnStage);
	PracticeSetLeadIn(ride >= 0 ? (start - ride) / 120.0f : 0.0f);

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

void RLCoach::LearnStartTestOut()
{
	if (!learnActive) return;
	const LessonInfo* info = Lessons::Find(learnLesson.id);
	if (!info || !info->TestOut() || learnProgress.completed) return;
	learnTestOut = true;
	learnTestOutScores.clear();
	learnStage = static_cast<int>(learnLesson.stages.size()) - 1;
	learnSpeed = 1.0f;
	learnLast = StageScore{};
	learnLastStage = -1;
	learnEvents.clear();
	if (practiceState != PracticeState::Off) LearnTry();
	LearnPublish();
}

void RLCoach::LearnEndTestOut(bool passed)
{
	if (!learnTestOut) return;
	learnTestOut = false;
	const int last = static_cast<int>(learnLesson.stages.size()) - 1;
	if (passed)
	{
		learnProgress.completed = true;
		learnProgress.unlocked = std::max(learnProgress.unlocked, last);
		learnProgress.testOut = 2;
		learnEvents.push_back("Tested out: lesson complete! Every stage stays open if you want to sharpen a step.");
	}
	else
	{
		learnProgress.testOut = 1;
		learnStage = 0;
		learnSpeed = learnAutoRamp ? LessonProgress::kRamp[0] : 1.0f;
		learnProgress.rampStreak = 0;
		if (!learnTestOutScores.empty()) learnEvents.push_back("Not this time: the steps are open, starting with step 1. You can try the test-out again any time.");
		if (practiceState != PracticeState::Off) LearnTry();
	}
	learnTestOutScores.clear();
	LearnSaveProgress();
	LearnPublish();
}

// The Recovery lesson (Nov 3, Coop: "ball cam is useless there, and the no ball cam angle is awkward as hell"): the
// Combine's momentum camera (road2_combine_momentum_cam) until you've landed, then your own camera; and a ring at the
// back post, where you're heading
void RLCoach::DrawLearnRecoveryHud(CanvasWrapper& canvas)
{
	if (!learnActive || practiceState == PracticeState::Off || learnLesson.kind != LessonKind::Move || learnLesson.moveKind != "recovery" || learnLesson.model.empty()) return;
	CameraWrapper cam = gameWrapper->GetCamera();
	CarWrapper me = gameWrapper->GetLocalCar();
	if (cam.IsNull() || me.IsNull()) return;
	constexpr float kU2R = 3.14159265f / 32768.0f;
	bool air = practiceState == PracticeState::Frozen;
	if (!air)
	{
		int land = -1;
		for (size_t i = 0; i < practiceLive.size(); i++) if (practiceLive[i].onGround) { land = static_cast<int>(i); break; }
		air = land < 0 || static_cast<int>(practiceLive.size()) - land < 18;
	}
	if (*combineMomentumCam && air)
	{
		const Frame& m0 = learnLesson.model[std::min(std::max(0, practiceStartTick), static_cast<int>(learnLesson.model.size()) - 1)];
		const Vector v = practiceState == PracticeState::Frozen ? Vector(m0.carVX, m0.carVY, m0.carVZ) : me.GetVelocity();
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
	// The back post (Lesson.cpp's FindMove drives you back toward it)
	const Vector camLoc = cam.GetLocation();
	const Vector camFwd = RotatorToVector(cam.GetRotation());
	Vector2F prev{};
	bool prevOk = false;
	for (int i = 0; i <= 40; i++)
	{
		const float a = i * 6.2831853f / 40.0f;
		const Vector p(800.0f + 260.0f * std::cos(a), -4900.0f + 260.0f * std::sin(a), 20.0f);
		Vector2F sc;
		const bool ok = Overlay3D::ToScreen(canvas, camLoc, camFwd, p, sc);
		if (ok && prevOk) Overlay3D::OutlinedLine(canvas, prev, sc, 6.0f, 46, 139, 255, 255);
		prev = sc;
		prevOk = ok;
	}
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
	return Lessons::StageDone(learnLesson, learnStage, learnFull, LearnHandover());
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
	StageScore sc = Lessons::Score(learnLesson, learnStage, learnFull, LearnHandover(), practiceWentIn, practiceFloorFirst, ShotReference{});
	LearnLogAttempt(sc);
	learnLast = sc;
	learnLastStage = learnStage;
	learnLastSpeed = practiceSpeed;
	learnEvents.clear();
	practiceAttempts++;

	if (learnTestOut)
	{
		// The test-out: full-speed attempts at the whole thing. 3 at 4-5 completes the lesson; 3 below 4 opens the steps.
		if (practiceSpeed >= 0.999f)
		{
			learnTestOutScores.push_back(sc.score);
			const int last = static_cast<int>(learnLesson.stages.size()) - 1;
			learnProgress.best[last] = std::max(learnProgress.best[last], sc.score);
			int pass = 0, miss = 0;
			for (int v : learnTestOutScores) (v >= 4 ? pass : miss)++;
			if (pass >= LessonProgress::kUnlockNeed) LearnEndTestOut(true);
			else if (miss > LessonProgress::kUnlockOf - LessonProgress::kUnlockNeed) LearnEndTestOut(false);
			else learnEvents.push_back(std::format("Test-out: {} of 3 so far ({} attempt{} left)", pass, LessonProgress::kUnlockOf - static_cast<int>(learnTestOutScores.size()),
				LessonProgress::kUnlockOf - static_cast<int>(learnTestOutScores.size()) == 1 ? "" : "s"));
			LearnSaveProgress();
		}
	}
	else if (learnWarmup)
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
	const float S = Sc();   // the window's size right now (UiStyle.h)
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
	Wordmark(uiFonts[UiFonts::Display], "Learn");
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

	// ---- Lesson header (style A): chips, the name big, a slanted pip per stage ----
	ImGui::Spacing();
	{
		const int done = std::min(u.unlocked + (u.completed ? 1 : 0), nStages);
		if (u.completed) { Pill("Complete", kGood); ImGui::SameLine(0, 6.0f * S); }
		else if (u.testOut) { Pill("Test out", kOrange); ImGui::SameLine(0, 6.0f * S); }
		Pill(std::format("{} of {} stages cleared", done, nStages), kChip);
		{
			ImFont* fb = uiFonts[UiFonts::Big];
			FontPush f(fb ? fb : fHead);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W);
			ImGui::TextColored(kText, "%s", Upper(u.name).c_str());
			ImGui::PopTextWrapPos();
		}
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float g = 5.0f * S, h = 8.0f * S, pw = (W - g * (nStages - 1)) / std::max(1, nStages);
		for (int i = 0; i < nStages; i++)
		{
			const ImVec2 a(p.x + i * (pw + g), p.y), b(a.x + pw, a.y + h);
			const bool cleared = i < u.unlocked || (u.completed && i == nStages - 1);
			SlantQuad(dl, a, b, U32(cleared ? kGood : i == stage ? kText : kTrack));
			if (i == stage && !cleared) SlantQuad(dl, ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), U32(kAccent), false, 2.0f);
		}
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
			FontPush f(fHead);
			ImGui::TextColored(kGood, "LESSON COMPLETE");
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

	// ---- Test-out (tiers 1-2): the whole thing first ----
	if (u.testOut)
	{
		ImGui::Spacing();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float pad = 8.0f * S;
		ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
		ImGui::BeginGroup();
		{
			FontPush f(fHead);
			ImGui::TextColored(kAccentText, "TEST OUT");
		}
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + W - 2.0f * pad);
		ImGui::TextColored(kSub, "Already got it? Do the whole thing at full speed: 3 of 5 scoring 4-5 completes the lesson. Miss 3 and the steps open instead.");
		ImGui::PopTextWrapPos();
		for (int k = 0; k < LessonProgress::kUnlockOf; k++)
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImVec2 c(p.x + 7.0f * S, p.y + ImGui::GetTextLineHeight() * 0.5f);
			SlantQuad(dl, ImVec2(c.x - 7.0f * S, c.y - 5.0f * S), ImVec2(c.x + 7.0f * S, c.y + 5.0f * S), U32(k < static_cast<int>(u.testOutScores.size()) ? GradeColor(u.testOutScores[k]) : kTrack));
			ImGui::Dummy(ImVec2(16.0f * S, ImGui::GetTextLineHeight()));
			ImGui::SameLine();
		}
		int pass = 0;
		for (int v : u.testOutScores) pass += v >= 4;
		ImGui::TextColored(pass > 0 ? kGood : kMuted, "%d/3", pass);
		if (OutlineButton("Skip to the steps")) onGame([this]() { LearnEndTestOut(false); });
		ImGui::EndGroup();
		const ImVec2 p1(p0.x + W, ImGui::GetItemRectMax().y + pad);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p0, p1, U32(kAccent, 0.06f), 8.0f * S);
		dl->AddRect(p0, p1, U32(kAccent, 0.5f), 8.0f * S);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
		ImGui::Dummy(ImVec2(W, 2.0f * S));
	}

	// ---- The stages ----
	Heading("STAGES");
	for (int i = 0; i < nStages; i++)
	{
		const bool locked = i > u.unlocked && !u.unlockAll && !(u.testOut && i == stage), current = i == stage, cleared = i < u.unlocked || (u.completed && i == nStages - 1);
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
		// Style A: a slanted pip, green when cleared, outlined blue for the one you're on
		{
			const ImVec2 a(c.x - 6.0f * S, c.y - 5.0f * S), b(c.x + 6.0f * S, c.y + 5.0f * S);
			SlantQuad(dl, a, b, U32(cleared ? kGood : kTrack));
			if (current) SlantQuad(dl, ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), U32(kAccent), false, 2.0f);
		}
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
			const bool whole = stage < static_cast<int>(u.stageWhole.size()) && u.stageWhole[stage];
			if (whole && u.stars[2] > 0)
				wrapped(std::format("The whole kickoff, from go to the ball, scored on your time: one star {:.2f} s, two stars {:.2f} s, three stars {:.2f} s. "
					"{} Each step is scored too, so you can see which one to work on.", u.stars[0], u.stars[1], u.stars[2], u.starsWho), kText);
			else
				wrapped(whole || (last == static_cast<int>(u.steps.size()) - 1 && first == 0)
					? (u.moveLesson ? std::string("The whole thing, start to finish, against the model's time. Each step is scored too, so you can see which one to work on.")
						: std::format("The whole thing, start to finish. Graded like any {} rep; each step is scored too, so you can see which one to work on.", Lower(u.name)))
					: std::format("Put steps {}-{} together in one go. Each step is scored; your score is the lowest of them.", first + 1, last + 1), kText);
			ImGui::Spacing();
			label("ONE CUE PER STEP", kMuted);
			for (int s = first; s <= last; s++)
			{
				ImGui::TextColored(kMuted, "%d", whole ? s - first + 1 : s + 1);
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
	if (u.canTestOut)
	{
		ImGui::SameLine();
		if (LinkButton("Test out")) onGame([this]() { LearnStartTestOut(); });
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("The whole thing at full speed: 3 of 5 scoring 4-5 completes the lesson.");
	}

	// ---- Speed: a slider, or the auto ramp (the test-out is always full speed) ----
	if (!u.testOut)
	{
		Heading("SPEED");
		int pct = static_cast<int>(std::lround(u.speed * 100.0f));
		ImGui::PushItemWidth(std::min(W * 0.55f, 220.0f * S));
		if (UiStyle::SliderInt("##speed", &pct, 25, 100, "%d%%"))
		{
			pct = std::clamp(static_cast<int>(std::lround(pct / 5.0)) * 5, 25, 100);
			const float sp = pct / 100.0f;
			onGame([this, sp]() { LearnSetSpeed(sp); });
		}
		ImGui::PopItemWidth();
		ImGui::SameLine();
		bool ramp = u.autoRamp;
		if (UiStyle::Checkbox("Auto ramp", &ramp)) onGame([this, ramp]() { LearnSetAutoRamp(ramp); });
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
	if (stage == u.unlocked && !(stage + 1 >= nStages && u.completed) && !u.testOut)
	{
		Heading(stage + 1 < nStages ? "TO UNLOCK THE NEXT STAGE" : "TO COMPLETE THE LESSON");
		ImGui::TextColored(kSub, "3 of your last 5 at 100%% scoring 4-5:");
		ImGui::SameLine();
		const std::vector<int>& r = u.recent[stage];
		for (int k = 0; k < LessonProgress::kUnlockOf; k++)
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImVec2 c(p.x + 7.0f * S, p.y + ImGui::GetTextLineHeight() * 0.5f);
			SlantQuad(dl, ImVec2(c.x - 7.0f * S, c.y - 5.0f * S), ImVec2(c.x + 7.0f * S, c.y + 5.0f * S), U32(k < static_cast<int>(r.size()) ? GradeColor(r[k]) : kTrack));
			ImGui::Dummy(ImVec2(16.0f * S, ImGui::GetTextLineHeight()));
			ImGui::SameLine();
		}
		ImGui::TextColored(u.passed[stage] >= LessonProgress::kUnlockNeed ? kGood : kMuted, "%d/3", std::min(u.passed[stage], 3));
		if (u.speed < 0.999f) ImGui::TextColored(kMuted, "Attempts below 100%% count toward the ramp, not the unlock.");
	}

	// ---- Ghost ----
	{
		bool ghost = u.ghost;
		if (UiStyle::Checkbox("Show the model's inputs on the controller ghost", &ghost)) onGame([this, ghost]() { LearnSetGhost(ghost); });
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("How see-through it is: the \"Controller ghost\" slider in the settings, under Controller overlay.");
	}

	// ---- The last attempt ----
	if (u.last.score > 0)
	{
		Heading("LAST ATTEMPT");
		// Style A: the score on the same slanted badge as the rep viewer's grade
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float r = 24.0f * S;
		GradeBadge(dl, ImVec2(p.x + 4.0f * S, p.y + 4.0f * S), 2.0f * r, u.last.score, GradeColor(u.last.score), false);
		(void)fBig;
		ImGui::SetCursorScreenPos(ImVec2(p.x + 2.0f * r + 20.0f * S, p.y));
		ImGui::BeginGroup();
		ImGui::TextColored(kMuted, "at %d%% speed", static_cast<int>(std::lround(u.lastSpeed * 100.0f)));
		for (size_t i = 0; i < u.last.lines.size() && i < 5; i++) wrapped(u.last.lines[i], i == 0 ? kText : kSub);
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetCursorScreenPos().y, p.y + 2.0f * r + 10.0f * S)));
	}
	for (const std::string& e : u.events) ImGui::TextColored(kAccentText, "%s", e.c_str());

	if (u.practicing) ImGui::TextColored(kMuted, "Move to start each attempt. Free play's reset button counts as a retry.");
	else if (u.watching) ImGui::TextColored(kMuted, "Watching the model. Press Try it when you're ready.");

	// ---- More on it (kickoffs: what decides them, variations, teammates, the Combine) ----
	if (!u.notes.empty())
	{
		Heading("MORE ON KICKOFFS");
		for (const std::string& t : u.notes)
		{
			ImGui::TextColored(kAccentText, "-");
			ImGui::SameLine();
			wrapped(t, kSub);
		}
	}
}

// The lesson list: the skill tree as a list for now (a drawn tree comes with the design pass)
void RLCoach::RenderLearnList()
{
	using namespace UiStyle;
	const float S = Sc();   // the window's size right now (UiStyle.h)
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

	Wordmark(uiFonts[UiFonts::Display], "Learn");
	ImGui::SameLine(std::max(200.0f * S, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Back to reps").x - 16.0f * S));
	if (LinkButton("Back to reps"))
	{
		uiLearnMode = false;
		onGame([this]() { LearnClose(); });
	}
	ImGui::Spacing();
	if (u.lessons.empty()) { ImGui::TextColored(kMuted, "Loading..."); return; }
	// The drawn tree (Nov 3, from the design canvas mock); a narrow window keeps the list
	if (W >= 470.0f * S) { RenderSkillTree(u); return; }
	{
		ImFont* fb = uiFonts[UiFonts::Big];
		FontPush f(fb ? fb : fHead);
		ImGui::TextColored(kText, "LESSONS");
	}
	ImGui::PushStyleColor(ImGuiCol_Text, kSub);
	ImGui::TextWrapped("Each lesson teaches one mechanic step by step. Some build on others: finish what they need first.");
	ImGui::PopStyleColor();

	ImGui::Spacing();
	for (size_t i = 0; i < u.lessons.size(); i++)
	{
		const UiLessonEntry& en = u.lessons[i];
		// The tree's tiers as groups (Nov 2), kickoffs their own group
		if (i == 0 || u.lessons[i - 1].tier != en.tier)
		{
			static const char* kTier[] = { "KICKOFFS", "TIER 1  FOUNDATIONS", "TIER 2  CORE", "TIER 3  ADVANCED", "TIER 4  EXPERT", "TIER 5  MASTER", "TIER 6  LEGEND" };
			Heading(kTier[std::clamp(en.tier, 0, 6)]);
			if (en.tier == 1)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
				ImGui::TextWrapped("Tiers 1 and 2 open on a test-out: do the whole thing at full speed, 3 of 5 at 4-5, and the lesson's done.");
				ImGui::PopStyleColor();
			}
		}
		const bool open = en.built && !en.locked;
		const float rowH = 46.0f * S;
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::PushID(static_cast<int>(i));
		const bool clicked = ImGui::InvisibleButton("lesson", ImVec2(W, rowH));
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		if (clicked && open) onGame([this, id = en.id]() { LearnOpenLesson(id); });
		dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rowH - 4.0f * S), U32(hov && open ? kPanel : kPanel2), 3.0f * S);
		if (en.completed) dl->AddRectFilled(p, ImVec2(p.x + 3.0f * S, p.y + rowH - 4.0f * S), U32(kGood));        // style A: a green edge when done
		else if (hov && open) dl->AddRectFilled(p, ImVec2(p.x + 3.0f * S, p.y + rowH - 4.0f * S), U32(kAccent));

		// Status mark: done, in progress, locked, coming soon
		ImVec2 c(p.x + 16.0f * S, p.y + (rowH - 4.0f * S) * 0.5f);
		{
			const ImVec2 a(c.x - 7.0f * S, c.y - 6.0f * S), b(c.x + 7.0f * S, c.y + 6.0f * S);
			if (en.completed) SlantQuad(dl, a, b, U32(kGood));
			else if (!en.built) SlantQuad(dl, a, b, U32(kLineSoft), false, 1.5f);
			else if (en.locked) SlantQuad(dl, a, b, U32(kMuted), false, 1.5f);
			else SlantQuad(dl, a, b, U32(kAccent), false, 2.0f);
		}

		const float tx = p.x + 32.0f * S;
		const ImVec4 nameCol = !en.built ? kMuted : en.locked ? kSub : kText;
		dl->AddText(fBold ? fBold : ImGui::GetFont(), fBold ? fBold->FontSize * gUser : ImGui::GetFontSize(), ImVec2(tx, p.y + 6.0f * S), U32(nameCol), en.name.c_str());
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
		if (hov && !en.built) ImGui::SetTooltip("Coming soon.");
	}

	RenderLearnLocks(u.unlockAll);
}

void RLCoach::RenderLearnLocks(bool unlockAll)
{
	using namespace UiStyle;
	Heading("LOCKS");
	bool all = unlockAll;
	if (UiStyle::Checkbox("Unlock every lesson and stage", &all)) gameWrapper->Execute([this, all](GameWrapper*) { LearnSetUnlockAll(all); });
	ImGui::PushStyleColor(ImGuiCol_Text, all ? kWarn : kMuted);
	ImGui::TextWrapped("Locks keep you learning things in an order that builds on itself. Turning them off is for practicing, not learning: "
		"you can open anything, but skipping ahead is the slow way to actually get good.");
	ImGui::PopStyleColor();
}

// ============================================================
// The skill tree (Nov 3, Coop: "build that skill tree"). The design canvas mock, drawn: branches as columns (air, air
// roll, ball control, movement, walls), tiers as bands, kickoffs underneath. A line from each skill to what it opens,
// lit blue once it's cleared. Tiles: cleared (a grade-coloured edge and your best grade), tested out (teal), in
// progress (blue edge, stage pips), open (blue outline), the one to play next (filled blue, an orange tag), locked
// (dark, what it needs), coming soon (dashed).
// ============================================================

namespace
{
	struct TreeSpot { const char* id; int band, col, row; };
	// band 0-4 = tiers 1-5, 5 = kickoffs
	const TreeSpot kTreeSpots[] = {
		{ "aerial", 0, 0, 0 }, { "air_roll", 0, 1, 0 }, { "power_shot", 0, 2, 0 }, { "dribble", 0, 2, 1 }, { "half_flip", 0, 3, 0 }, { "recovery", 0, 3, 1 },
		{ "powerslide_turn", 0, 3, 2 }, { "wall_hit", 0, 4, 0 },
		{ "fast_aerial", 1, 0, 0 }, { "backboard_read", 1, 0, 1 }, { "tornado_spin", 1, 1, 0 }, { "front_flick", 1, 2, 0 }, { "ground_catch", 1, 2, 1 },
		{ "speedflip", 1, 3, 0 }, { "wavedash", 1, 3, 1 }, { "off_the_wall", 1, 4, 0 },
		{ "double_tap", 2, 0, 0 }, { "air_dribble", 2, 2, 0 }, { "ceiling_shot", 2, 4, 0 },
		{ "air_dribble_double_tap", 3, 1, 0 }, { "flip_reset", 3, 2, 0 }, { "ceiling_double_tap", 3, 3, 0 },
		{ "flip_reset_double_tap", 4, 1, 0 }, { "ceiling_flip_reset", 4, 3, 0 },
		{ "kickoff_flipin", 5, 0, 0 }, { "kickoff_diagonal", 5, 1, 0 }, { "kickoff_offcentre", 5, 2, 0 }, { "kickoff_back", 5, 2, 1 },
	};
	// Short names for the tiles (the list keeps the full ones)
	std::string TreeName(const std::string& id, const std::string& name)
	{
		if (id == "air_roll") return "Air roll";
		if (id == "powerslide_turn") return "Powerslide";
		if (id == "air_dribble_double_tap") return "Air dribble DT";
		if (id == "ceiling_double_tap") return "Ceiling DT";
		if (id == "flip_reset_double_tap") return "Flip reset DT";
		if (id == "kickoff_flipin") return "Flip-in";
		if (id == "kickoff_diagonal") return "Diagonal";
		if (id == "kickoff_offcentre") return "Off-centre";
		if (id == "kickoff_back") return "Back centre";
		return name;
	}
	void DashedSlant(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float thick)
	{
		const float o = (b.y - a.y) * UiStyle::kSlant * 0.5f;
		const ImVec2 p[4] = { ImVec2(a.x + o, a.y), ImVec2(b.x + o, a.y), ImVec2(b.x - o, b.y), ImVec2(a.x - o, b.y) };
		for (int e = 0; e < 4; e++)
		{
			const ImVec2 s = p[e], t = p[(e + 1) % 4];
			const float len = std::sqrt((t.x - s.x) * (t.x - s.x) + (t.y - s.y) * (t.y - s.y));
			for (float d = 0; d < len; d += dash * 2.0f)
			{
				const float d2 = std::min(len, d + dash);
				dl->AddLine(ImVec2(s.x + (t.x - s.x) * d / len, s.y + (t.y - s.y) * d / len), ImVec2(s.x + (t.x - s.x) * d2 / len, s.y + (t.y - s.y) * d2 / len), col, thick);
			}
		}
	}
}

void RLCoach::RenderSkillTree(const UiLearn& u)
{
	using namespace UiStyle;
	const float S = Sc();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float W = ImGui::GetContentRegionAvail().x;
	ImFont* fName = uiFonts[UiFonts::Head] ? uiFonts[UiFonts::Head] : ImGui::GetFont();
	ImFont* fCaps = uiFonts[UiFonts::Caps] ? uiFonts[UiFonts::Caps] : ImGui::GetFont();
	ImFont* fBig = uiFonts[UiFonts::Big] ? uiFonts[UiFonts::Big] : ImGui::GetFont();
	const ImVec4 kTeal = Hex(0x2DE2E6);
	auto open = [](const UiLessonEntry& e) { return e.built && !e.locked; };

	// What to play next: the first lesson you've started and not finished, else the first open one you haven't
	int next = -1;
	for (int pass = 0; pass < 2 && next < 0; pass++)
		for (size_t i = 0; i < u.lessons.size(); i++)
		{
			const UiLessonEntry& e = u.lessons[i];
			if (!open(e) || e.completed) continue;
			if (pass == 0 ? e.cleared > 0 : e.cleared == 0) { next = static_cast<int>(i); break; }
		}
	int nDone = 0, nGoing = 0;
	for (const UiLessonEntry& e : u.lessons) { nDone += e.completed; nGoing += !e.completed && e.cleared > 0; }

	// ---- Header: the title, how far along, what's next ----
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const float rightW = 170.0f * S;
	{
		FontPush f(fBig);
		ImGui::TextColored(kText, "SKILL TREE");
	}
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - rightW - 10.0f * S);
	ImGui::TextColored(kSub, "Clear a skill, or test out of it, to open what it leads to.");
	ImGui::PopTextWrapPos();
	{
		const int n = static_cast<int>(u.lessons.size());
		const float bw = W - rightW - 10.0f * S, gap = 3.0f * S, segW = (bw - gap * (n - 1)) / std::max(1, n), h = 8.0f * S;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		for (int i = 0; i < n; i++)
		{
			const ImVec4 c = i < nDone ? kGood : i < nDone + nGoing ? kAccent : kTrack;
			SlantQuad(dl, ImVec2(p.x + i * (segW + gap), p.y + 3.0f * S), ImVec2(p.x + i * (segW + gap) + segW, p.y + 3.0f * S + h), U32(c));
		}
		ImGui::Dummy(ImVec2(bw, h + 6.0f * S));
		CapsFont cf(fCaps);
		ImGui::TextColored(kMuted, "%d OF %d CLEARED  /  %d IN PROGRESS", nDone, n, nGoing);
	}
	const float headerBottom = ImGui::GetCursorScreenPos().y;
	if (next >= 0)
	{
		const UiLessonEntry& e = u.lessons[next];
		const float x = top.x + W - rightW;
		ImGui::SetCursorScreenPos(ImVec2(x, top.y + 4.0f * S));
		{
			CapsFont cf(fCaps);
			ImGui::TextColored(kMuted, "PLAY NEXT");
		}
		ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
		{
			FontPush f(fName);
			ImGui::TextColored(kText, "%s", Caps(TreeName(e.id, e.name).c_str()).c_str());
		}
		ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y + 2.0f * S));
		if (SlantButton(e.cleared > 0 ? "Continue" : "Start", ImVec2(rightW - 6.0f * S, 0), kOrange, true, Hex(0x1A0E02)))
			gameWrapper->Execute([this, id = e.id](GameWrapper*) { LearnOpenLesson(id); });
	}
	ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(headerBottom, ImGui::GetCursorScreenPos().y) + 8.0f * S));

	// ---- The layout ----
	const float fsName = fName->FontSize * gUser * 0.78f, fsStat = fCaps->FontSize * gUser * 0.78f;
	const float colW = W / 5.0f, nw = std::min(160.0f * S, colW - 12.0f * S), rowGap = 16.0f * S;
	const float nh = std::max(50.0f * S, 4.0f * S + fsName * 1.95f + fsStat + 6.0f * S);   // room for a name on two lines
	const float bandHead = 30.0f * S, bandGap = 10.0f * S;
	struct Node { int idx; int band, col, row; ImVec2 a, b; };
	std::vector<Node> nodes;
	std::unordered_map<std::string, int> at;   // id -> nodes index
	int rows[6] = { 0, 0, 0, 0, 0, 0 };
	for (size_t i = 0; i < u.lessons.size(); i++)
	{
		const UiLessonEntry& e = u.lessons[i];
		Node nd{ static_cast<int>(i), -1, 0, 0, {}, {} };
		for (const TreeSpot& sp : kTreeSpots) if (e.id == sp.id) { nd.band = sp.band; nd.col = sp.col; nd.row = sp.row; }
		if (nd.band < 0)
		{
			// a lesson the map doesn't know yet: its tier's band, in a new row at the end
			nd.band = e.tier == 0 ? 5 : std::clamp(e.tier - 1, 0, 4);
			nd.col = 0; nd.row = rows[nd.band];
		}
		rows[nd.band] = std::max(rows[nd.band], nd.row + 1);
		at[e.id] = static_cast<int>(nodes.size());
		nodes.push_back(nd);
	}
	// Column names over tier 1
	ImVec2 o = ImGui::GetCursorScreenPos();
	{
		static const char* kCols[5] = { "AIR", "AIR ROLL", "BALL CONTROL", "MOVEMENT", "WALLS" };
		for (int c = 0; c < 5; c++)
			dl->AddText(fCaps, fCaps->FontSize * gUser * 0.85f, ImVec2(o.x + colW * (c + 0.5f) - nw * 0.5f, o.y), U32(Hex(0x5F6B86)), kCols[c]);
		o.y += fCaps->FontSize * gUser + 6.0f * S;
	}
	float bandTop[6], y = o.y;
	for (int b = 0; b < 6; b++)
	{
		bandTop[b] = y;
		y += bandHead + std::max(1, rows[b]) * (nh + rowGap) - rowGap + bandGap + (b == 4 ? 14.0f * S : 0.0f);
	}
	const float totalH = y - o.y;
	for (Node& nd : nodes)
	{
		const float cx = o.x + colW * (nd.col + 0.5f), ny = bandTop[nd.band] + bandHead + nd.row * (nh + rowGap);
		nd.a = ImVec2(cx - nw * 0.5f, ny);
		nd.b = ImVec2(cx + nw * 0.5f, ny + nh);
	}

	// the band rules under the lines
	for (int b = 0; b < 6; b++)
	{
		const float hy = bandTop[b] + 4.0f * S, fs = fCaps->FontSize * gUser;
		dl->AddLine(ImVec2(o.x, hy + fs * 0.55f), ImVec2(o.x + W, hy + fs * 0.55f), U32(kLineSoft));
	}

	// ---- The lines ----
	auto blocked = [&](float x, float y1, float y2, int skipA, int skipB) {
		for (size_t k = 0; k < nodes.size(); k++)
		{
			if (static_cast<int>(k) == skipA || static_cast<int>(k) == skipB) continue;
			const Node& n = nodes[k];
			if (x >= n.a.x - 2 && x <= n.b.x + 2 && n.a.y < y2 && n.b.y > y1) return true;
		}
		return false;
	};
	for (int pass = 0; pass < 2; pass++)   // the unlit lines first, so the lit ones draw over them
		for (size_t k = 0; k < nodes.size(); k++)
		{
			const UiLessonEntry& e = u.lessons[nodes[k].idx];
			for (const std::string& need : e.needs)
			{
				auto it = at.find(need);
				if (it == at.end()) continue;
				const Node& p = nodes[it->second];
				const Node& c = nodes[k];
				const bool lit = u.lessons[p.idx].completed;
				if (lit != (pass == 1)) continue;
				const ImU32 col = U32(lit ? kAccent : kLine);
				const float th = (lit ? 2.5f : 2.0f) * S;
				const float pcx = (p.a.x + p.b.x) * 0.5f, ccx = (c.a.x + c.b.x) * 0.5f, pmy = (p.a.y + p.b.y) * 0.5f, cmy = (c.a.y + c.b.y) * 0.5f;
				const float gapX = colW - nw;
				std::vector<ImVec2> pts;
				if (std::fabs(p.a.y - c.a.y) < 1.0f || (p.band == 5 && c.band == 5))
				{
					const float gx = p.b.x + gapX * 0.5f;
					pts = { ImVec2(p.b.x, pmy), ImVec2(gx, pmy), ImVec2(gx, cmy), ImVec2(c.a.x, cmy) };
				}
				else if (std::fabs(pcx - ccx) < 1.0f && !blocked(pcx, p.b.y, c.a.y, it->second, static_cast<int>(k)))
					pts = { ImVec2(pcx, p.b.y), ImVec2(ccx, c.a.y) };
				else if (std::fabs(pcx - ccx) < 1.0f)
				{
					const float gx = p.b.x + gapX * 0.5f;
					pts = { ImVec2(p.b.x, pmy), ImVec2(gx, pmy), ImVec2(gx, cmy), ImVec2(c.b.x, cmy) };
				}
				else
				{
					const float bus = c.a.y - 9.0f * S;
					if (!blocked(pcx, p.b.y, bus, it->second, static_cast<int>(k))) pts = { ImVec2(pcx, p.b.y), ImVec2(pcx, bus), ImVec2(ccx, bus), ImVec2(ccx, c.a.y) };
					else
					{
						const float gx = p.b.x + gapX * 0.5f;
						pts = { ImVec2(p.b.x, pmy), ImVec2(gx, pmy), ImVec2(gx, bus), ImVec2(ccx, bus), ImVec2(ccx, c.a.y) };
					}
				}
				dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, false, th);
			}
		}

	// ---- Band headers ----
	static const char* kBands[6] = { "TIER 1  /  FOUNDATIONS", "TIER 2  /  CORE", "TIER 3  /  ADVANCED", "TIER 4  /  EXPERT", "TIER 5  /  MASTER", "KICKOFFS" };
	for (int b = 0; b < 6; b++)
	{
		int done = 0, all = 0;
		for (const Node& nd : nodes) if (nd.band == b) { all++; done += u.lessons[nd.idx].completed; }
		const float hy = bandTop[b] + 4.0f * S, fs = fCaps->FontSize * gUser;
		const float lx = o.x + fCaps->CalcTextSizeA(fs, 1e9f, 0.0f, kBands[b]).x + 10.0f * S;
		const std::string cnt = std::format("{} / {}", done, all);
		const float cw = fCaps->CalcTextSizeA(fs, 1e9f, 0.0f, cnt.c_str()).x;
		// (drawn over the lines: the names cut through them)
		dl->AddRectFilled(ImVec2(o.x - 2.0f * S, hy - 1.0f * S), ImVec2(lx - 4.0f * S, hy + fs + 1.0f * S), U32(kBg));
		dl->AddRectFilled(ImVec2(o.x + W - cw - 6.0f * S, hy - 1.0f * S), ImVec2(o.x + W + 2.0f * S, hy + fs + 1.0f * S), U32(kBg));
		dl->AddText(fCaps, fs, ImVec2(o.x, hy), U32(kSub), kBands[b]);
		
		dl->AddText(fCaps, fs, ImVec2(o.x + W - cw, hy), U32(kMuted), cnt.c_str());
	}

	// ---- The tiles ----
	for (size_t k = 0; k < nodes.size(); k++)
	{
		const Node& nd = nodes[k];
		const UiLessonEntry& e = u.lessons[nd.idx];
		ImGui::SetCursorScreenPos(nd.a);
		ImGui::PushID(static_cast<int>(k) + 1000);
		const bool clicked = ImGui::InvisibleButton("skill", ImVec2(nw, nh));
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool isNext = static_cast<int>(nd.idx) == next;
		const bool going = !e.completed && e.cleared > 0;
		if (clicked && open(e)) gameWrapper->Execute([this, id = e.id](GameWrapper*) { LearnOpenLesson(id); });

		ImVec4 bg = kPanel, nameCol = kText, statCol = kSub, edge(0, 0, 0, 0), border(0, 0, 0, 0);
		std::string status;
		const ImVec4 gc = e.best > 0 ? GradeColor(e.best) : kGood;
		if (!e.built) { bg = ImVec4(0, 0, 0, 0); nameCol = Hex(0x5F6B86); statCol = Hex(0x4C5873); status = "COMING SOON"; }
		else if (e.locked)
		{
			bg = kPanel2; nameCol = Hex(0x5F6B86); statCol = Hex(0x4C5873); border = kChip;
			std::string need = e.missing.empty() ? "" : e.missing[0];
			for (const UiLessonEntry& o2 : u.lessons) if (o2.name == need) { need = TreeName(o2.id, o2.name); break; }
			status = e.missing.size() == 1 ? "NEEDS " + Caps(need.c_str()) : std::format("NEEDS {} SKILLS", e.missing.size());
		}
		else if (e.completed && e.testedOut) { edge = kTeal; statCol = kTeal; status = "TESTED OUT"; }
		else if (e.completed) { edge = gc; statCol = gc; status = "CLEARED"; }
		else if (isNext) { bg = kAccent; nameCol = kOnAccent; statCol = kOnAccent; status = going ? std::format("{} OF {} STAGES", e.cleared, e.stages) : "START"; }
		else if (going) { edge = kAccent; statCol = kAccentText; status = std::format("{} OF {} STAGES", e.cleared, e.stages); }
		else { border = kAccent; statCol = kAccentText; status = "START"; }
		if (hov && open(e) && !isNext) bg = kChip;

		if (isNext) SlantQuad(dl, ImVec2(nd.a.x + 4.0f * S, nd.a.y + 4.0f * S), ImVec2(nd.b.x + 4.0f * S, nd.b.y + 4.0f * S), U32(Hex(0x0F2C52)));
		if (bg.w > 0) SlantQuad(dl, nd.a, nd.b, U32(bg));
		if (!e.built) DashedSlant(dl, nd.a, nd.b, U32(kLine), 5.0f * S, 1.5f * S);
		else if (border.w > 0) SlantQuad(dl, ImVec2(nd.a.x + 1, nd.a.y + 1), ImVec2(nd.b.x - 1, nd.b.y - 1), U32(border), false, (border.x == kAccent.x ? 2.0f : 1.0f) * S);
		if (edge.w > 0)
		{
			const float o2 = nh * kSlant * 0.5f;
			const ImVec2 q[4] = { ImVec2(nd.a.x + o2, nd.a.y), ImVec2(nd.a.x + o2 + 5.0f * S, nd.a.y), ImVec2(nd.a.x - o2 + 5.0f * S, nd.b.y), ImVec2(nd.a.x - o2, nd.b.y) };
			dl->AddConvexPolyFilled(q, 4, U32(edge));
		}
		// the best grade, on cleared and in-progress tiles
		float badgeW = 0.0f;
		if (e.built && !e.locked && e.best > 0 && !isNext)
		{
			const float bs = std::round(nh * 0.48f);
			const ImVec2 bp(nd.b.x - bs - 8.0f * S, nd.a.y + (nh - bs) * 0.5f);
			badgeW = GradeBadge(dl, bp, bs, e.best, e.testedOut && e.completed ? kTeal : GradeColor(e.best), false) + 10.0f * S;
		}
		if (isNext)
		{
			const char* tag = "PLAY NEXT";
			const float tw = fCaps->CalcTextSizeA(fsStat, 1e9f, 0.0f, tag).x, th = fsStat + 3.0f * S;
			const ImVec2 ta(nd.a.x + 10.0f * S, nd.a.y - th * 0.6f);
			SlantQuad(dl, ta, ImVec2(ta.x + tw + 14.0f * S, ta.y + th), U32(kOrange));
			dl->AddText(fCaps, fsStat, ImVec2(ta.x + 7.0f * S, ta.y + 1.5f * S), U32(Hex(0x1A0E02)), tag);
		}
		// the name (two lines if it doesn't fit), then the status, kept inside the tile
		dl->PushClipRect(ImVec2(nd.a.x - 4.0f * S, nd.a.y - 12.0f * S), ImVec2(nd.b.x - 2.0f * S, nd.b.y), true);
		const float tx = nd.a.x + 14.0f * S, maxW = nw - 18.0f * S - badgeW;
		std::string name = Caps(TreeName(e.id, e.name).c_str());
		std::string line2;
		if (fName->CalcTextSizeA(fsName, 1e9f, 0.0f, name.c_str()).x > maxW)
		{
			const size_t sp = name.rfind(' ');
			if (sp != std::string::npos) { line2 = name.substr(sp + 1); name = name.substr(0, sp); }
		}
		float ty = nd.a.y + (line2.empty() ? 8.0f * S : 4.0f * S);
		dl->AddText(fName, fsName, ImVec2(tx, ty), U32(nameCol), name.c_str());
		if (!line2.empty()) { ty += fsName * 0.95f; dl->AddText(fName, fsName, ImVec2(tx - fsName * kSlant * 0.2f, ty), U32(nameCol), line2.c_str()); }
		ty += fsName + 2.0f * S;
		dl->AddText(fCaps, fsStat, ImVec2(tx - 2.0f * S, ty), U32(statCol), status.c_str());
		dl->PopClipRect();
		// stage pips on a lesson you're partway through
		if (going && e.stages > 0 && e.stages <= 10 && line2.empty() && !isNext)
		{
			const float pw = 8.0f * S, ph = 3.0f * S, py = nd.b.y - 7.0f * S;
			for (int s2 = 0; s2 < e.stages; s2++)
				SlantQuad(dl, ImVec2(tx + s2 * (pw + 2.0f * S), py), ImVec2(tx + s2 * (pw + 2.0f * S) + pw, py + ph), U32(s2 < e.cleared ? kGood : kTrack));
		}
		if (hov)
		{
			if (!e.built) ImGui::SetTooltip("%s\nComing soon.", e.blurb.c_str());
			else if (e.locked) ImGui::SetTooltip("%s\nOpens when you clear what it needs (or turn the locks off below).", e.blurb.c_str());
			else ImGui::SetTooltip("%s", e.blurb.c_str());
		}
	}
	ImGui::SetCursorScreenPos(o);
	ImGui::Dummy(ImVec2(W, totalH));
	ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
	ImGui::TextWrapped("Pick any skill for its lesson. Tier 1 and 2 skills you already have can be tested out of in one go.");
	ImGui::PopStyleColor();
	RenderLearnLocks(u.unlockAll);
}
