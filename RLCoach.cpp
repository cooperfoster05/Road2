#include "pch.h"
#include "RLCoach.h"
#include "Ratings.h"
#include "UiStyle.h"
#include "UiFonts.h"
#include "bakkesmod/wrappers/includes.h"
#include "bakkesmod/wrappers/GuiManagerWrapper.h"

#include <algorithm>
#include <cmath>
#include <format>


BAKKESMOD_PLUGIN(RLCoach, "RL Coach: chess.com-style review for your training reps", plugin_version, PLUGINTYPE_FREEPLAY | PLUGINTYPE_CUSTOM_TRAINING)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

namespace
{
	constexpr float kTickRate = 120.0f;
	constexpr float kTeleportDist = 400.0f;   // a jump this big in one tick means the shot or ball was reset
	constexpr int   kTouchDebounceTicks = 12; // contact on back-to-back ticks counts as one touch

	float Dist(const Vector& a, const Vector& b)
	{
		float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	bool InputActive(const ControllerInput& in)
	{
		const float dz = 0.1f;
		return std::fabs(in.Throttle) > dz || std::fabs(in.Steer) > dz || std::fabs(in.Pitch) > dz
			|| std::fabs(in.Yaw) > dz || std::fabs(in.Roll) > dz
			|| in.Jump || in.ActivateBoost || in.HoldingBoost || in.Handbrake;
	}

}

// ============================================================
// Lifecycle
// ============================================================

void RLCoach::onLoad()
{
	_globalCvarManager = cvarManager;

	hudEnabled = std::make_shared<bool>(true);
	recordingEnabled = std::make_shared<bool>(false);
	maxRepSeconds = std::make_shared<float>(10.0f);
	useNumpad = std::make_shared<bool>(false);
	manualGrading = std::make_shared<bool>(true);
	postRollSeconds = std::make_shared<float>(1.5f);
	livePrediction = std::make_shared<bool>(false);
	postRollMaxSeconds = std::make_shared<float>(4.0f);

	cvarManager->registerCvar("rlcoach_hud", "1", "Show the RL Coach panel", true, true, 0, true, 1)
		.bindTo(hudEnabled);

	// Recording is never saved to config, so it starts OFF every session
	CVarWrapper rec = cvarManager->registerCvar("rlcoach_recording", "0", "Record reps (data farming)", true, true, 0, true, 1, false);
	rec.bindTo(recordingEnabled);
	rec.addOnValueChanged([this](std::string, CVarWrapper cvar) { SetRecording(cvar.getBoolValue()); });

	cvarManager->registerCvar("rlcoach_max_rep_seconds", "10", "Longest a rep runs before it ends as a timeout", true, true, 2, true, 60)
		.bindTo(maxRepSeconds);

	cvarManager->registerCvar("rlcoach_postroll_seconds", "1.5", "Keep recording this long after the shot ends, to capture recovery and late goals", true, true, 0, true, 5)
		.bindTo(postRollSeconds);

	cvarManager->registerCvar("rlcoach_postroll_max_seconds", "4", "After the shot, keep recording until you're back in control, up to this long", true, true, 1, true, 8)
		.bindTo(postRollMaxSeconds);

	cvarManager->registerCvar("rlcoach_live_prediction", "0", "Draw the predicted ball path while you play", true, true, 0, true, 1)
		.bindTo(livePrediction);

	CVarWrapper mirror = cvarManager->registerCvar("rlcoach_pred_mirror", "0", "Mirror X when predicting (set from the accuracy test)", true, true, 0, true, 1);
	mirror.addOnValueChanged([this](std::string, CVarWrapper c) { predictor.mirrorX = c.getBoolValue(); });

	RegisterLookCvars();
	showTiming = std::make_shared<bool>(false);
	cvarManager->registerCvar("rlcoach_show_timing", "0", "Show how long RL Coach's own code takes each tick and frame", true, true, 0, true, 1)
		.bindTo(showTiming);

	keepDays = std::make_shared<float>(2.0f);
	keepRatedDays = std::make_shared<float>(180.0f);
	storageCapMb = std::make_shared<float>(1024.0f);
	cvarManager->registerCvar("rlcoach_keep_days", "2", "Days to keep full recordings of reps you didn't rate or star", true, true, 1, true, 60)
		.bindTo(keepDays);
	cvarManager->registerCvar("rlcoach_keep_rated_days", "180", "Days to keep full recordings of reps you rated", true, true, 7, true, 730)
		.bindTo(keepRatedDays);
	cvarManager->registerCvar("rlcoach_storage_cap_mb", "1024", "Most disk space recordings can use (MB)", true, true, 100, true, 10240)
		.bindTo(storageCapMb);

	CVarWrapper numpad = cvarManager->registerCvar("rlcoach_rate_numpad", "0", "Rate reps with numpad 1-5 instead of the number row", true, true, 0, true, 1);
	numpad.bindTo(useNumpad);
	numpad.addOnValueChanged([this](std::string, CVarWrapper) { LoadRatingKeys(); });

	cvarManager->registerCvar("rlcoach_manual_grading", "1", "Rate reps with the number keys (1-5 shot, 6-8 recovery)", true, true, 0, true, 1)
		.bindTo(manualGrading);

	cvarManager->registerNotifier("rlcoach_toggle_recording", [this](std::vector<std::string>) {
		cvarManager->getCvar("rlcoach_recording").setValue(*recordingEnabled ? 0 : 1);
	}, "Turn rep recording on or off", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_rate", [this](std::vector<std::string> args) {
		if (args.size() < 2) return;
		try { HandleRatingPress(std::stoi(args[1])); } catch (...) {}
	}, "Rate the last rep 1-5", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_rate_recovery", [this](std::vector<std::string> args) {
		if (args.size() < 2) return;
		try { HandleRecoveryPress(std::stoi(args[1])); } catch (...) {}
	}, "Rate the last rep's recovery: 1 poor, 2 eh, 3 good", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_rearm", [this](std::vector<std::string>) {
		if (*recordingEnabled && state != RepState::Recording) EnterArmed();
	}, "Arm the recorder without resetting the ball", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_viewer", [this](std::vector<std::string>) {
		cvarManager->executeCommand("togglemenu " + GetMenuName());
	}, "Open or close the rep viewer", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_practice_retry", [this](std::vector<std::string>) { PracticeRetry(); },
		"Practice: reset to the start of the segment", PERMISSION_ALL);
	cvarManager->registerNotifier("rlcoach_practice_exit", [this](std::vector<std::string>) { PracticeExit(); },
		"Practice: stop practicing", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_test_setstate", [this](std::vector<std::string>) {
		TestSetState();
	}, "Test: move the car and ball to a fixed spot", PERMISSION_ALL);

	menuTitle_ = "RL Coach - Rep Viewer";

	writer = std::make_unique<RepWriter>(std::filesystem::path(gameWrapper->GetDataFolderW()) / "RLCoach");

	// Every physics tick, before the game reads the car's input (the viewer replaces it)
	gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper car, void* params, std::string) { PerfTimer t(perfInput); OverrideViewerInput(car, params); });

	// Every physics tick, after the car's input for that tick is set
	gameWrapper->HookEventWithCallerPost<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper car, void*, std::string) { PerfTimer t(perfTick); OnVehicleInput(car); });

	gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.OnHitBall",
		[this](CarWrapper car, void* params, std::string) { OnBallTouch(car, params); });

	gameWrapper->HookEvent("Function TAGame.Ball_TA.OnHitGoal",
		[this](std::string) { OnGoal(); });

	gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { PerfTimer t(perfDraw); RenderHud(canvas); });

	LoadRatingKeys();
	LoadUiFonts();
	predictor.mirrorX = cvarManager->getCvar("rlcoach_pred_mirror").getBoolValue();
	StartPredictor();

	// In the background: tidy up storage, build the shot book and auto grades
	StartHousekeeping();
	LOG("RL Coach loaded. Reps save to {}", writer->Folder().string());
}

// The viewer's fonts (UiFonts.h): copied into BakkesMod's font folder the first time, then queued for loading.
// They show up a frame or two later (FetchUiFonts); until then, and if this fails, the default font is used.
void RLCoach::LoadUiFonts()
{
	const std::filesystem::path data(gameWrapper->GetDataFolderW());
	if (!UiFonts::Install(data)) LOG("RL Coach: the viewer's fonts aren't in {} (using the default font)", (data / "RLCoach" / "fonts").string());
	auto gui = gameWrapper->GetGUIManager();
	for (int i = 0; i < UiFonts::kCount; i++)
	{
		auto [status, font] = gui.LoadFont(UiFonts::kSpecs[i].name, UiFonts::kSpecs[i].file, UiFonts::kSpecs[i].size, nullptr, UiFonts::Ranges());
		if (status == 2 && font) uiFonts[i] = font;
	}
}

void RLCoach::StartPredictor()
{
	if (writer) predictor.StartInit(writer->Folder() / "collision_meshes");
}

void RLCoach::onUnload()
{
	whatIfCancel = true;
	if (whatIfThread.joinable()) whatIfThread.join();
	PracticeExit();
	ViewerExit();
	housekeepStop = true;
	if (housekeepThread.joinable()) housekeepThread.join();
	if (gradeThread.joinable()) gradeThread.join();
	if (shareThread.joinable()) shareThread.join();
	if (writer)
	{
		writer->Shutdown();   // finish any saves still queued
		gradeBook.Save(writer->Folder());
	}
	gameWrapper->UnregisterDrawables();
	frames.clear();
	state = RepState::Idle;
}

// ============================================================
// Recorder
// ============================================================

bool RLCoach::IsOfflinePractice()
{
	return gameWrapper->IsInFreeplay() || gameWrapper->IsInCustomTraining();
}

void RLCoach::SetRecording(bool on)
{
	frames.clear();
	havePrev = false;
	goalPending = false;
	touchPending = false;
	pendingEndReason.clear();
	queuedRating = 0;
	queuedRecovery = 0;
	if (on)
	{
		state = RepState::WaitReset;
		statusMessage = "Recording on: reset the shot or ball to start";
	}
	else
	{
		state = RepState::Idle;
		statusMessage = "Recording off";
	}
}

void RLCoach::EnterArmed()
{
	state = RepState::Armed;
	armedTicks = 0;
	statusMessage = "Armed: rep starts on your first input";
}

void RLCoach::OnVehicleInput(CarWrapper car)
{
	if (car.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	if (local.IsNull() || local.memory_address != car.memory_address) return;

	ControllerInput in = car.GetInput();
	lastInput = in;

	// Practice mode runs the car and ball itself; the recorder sits out
	if (practiceState != PracticeState::Off)
	{
		PracticeTick(car, in);
		return;
	}

	// While watching a rep, the viewer drives the car and ball and the recorder sits out
	if (viewerActive)
	{
		ViewerTick(car);
		return;
	}

	if (!*recordingEnabled || state == RepState::Idle) return;

	if (!IsOfflinePractice())
	{
		havePrev = false;
		if (state == RepState::Recording) { frames.clear(); state = RepState::WaitReset; }
		return;
	}

	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	BallWrapper ball = server.GetBall();
	if (ball.IsNull()) return;

	Vector carLoc = car.GetLocation();
	Vector ballLoc = ball.GetLocation();
	bool hadPrev = havePrev;
	float prevBallZ = prevBallLoc.Z;
	bool teleported = hadPrev && (Dist(carLoc, prevCarLoc) > kTeleportDist || Dist(ballLoc, prevBallLoc) > kTeleportDist);
	prevCarLoc = carLoc;
	prevBallLoc = ballLoc;
	havePrev = true;

	switch (state)
	{
	case RepState::WaitReset:
		if (teleported) EnterArmed();
		break;

	case RepState::Armed:
	{
		// For the first few ticks after arming, remember where things sit. In a training pack the car and
		// ball are frozen until the shot starts, so any movement after that means the attempt began,
		// even if the stick was only nudged below our input deadzone.
		if (teleported || armedTicks < 6)
		{
			if (teleported) armedTicks = 0;
			armedCarLoc = carLoc;
			armedBallLoc = ballLoc;
			armedTicks++;
			if (!InputActive(in)) break;
		}
		bool movedInPack = gameWrapper->IsInCustomTraining()
			&& (Dist(carLoc, armedCarLoc) > 5.0f || Dist(ballLoc, armedBallLoc) > 5.0f);

		if (InputActive(in) || movedInPack)
		{
			frames.clear();
			frames.reserve(static_cast<size_t>(kTickRate * 14));
			touchCount = 0;
			lastTouchTick = -1000;
			goalPending = false;
			touchPending = false;
			pendingEndReason.clear();
			eventTick = -1;
			floorBeforeGoal = false;
			queuedRating = 0;
			queuedRecovery = 0;
			recoverRun = 0;
			recovered = false;
			endTracker.Reset();
			predictedTouch = -1;
			repStartTime = std::chrono::steady_clock::now();
			state = RepState::Recording;
			frames.push_back(Capture(car, ball, in));
			statusMessage = "Recording rep";
		}
		break;
	}

	case RepState::Recording:
	{
		if (teleported)
		{
			// Shot or ball was reset. If the shot had already ended, keep that result; otherwise it's a bail.
			FinishRep(pendingEndReason.empty() ? "reset" : pendingEndReason);
			EnterArmed();
			break;
		}

		frames.push_back(Capture(car, ball, in));
		int tick = frames.back().tick;
		float elapsed = frames.back().t;

		// Once the ball is moving, predict where it's going (only drawn if live prediction is on)
		if (livePred.empty() && *livePrediction && touchCount == 0 && predictor.Ready())
		{
			const Frame& f = frames.back();
			if (std::fabs(f.ballVX) + std::fabs(f.ballVY) + std::fabs(f.ballVZ) > 1.0f)
				predictor.Predict(BallSample{ f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ },
					f.ballAVX, f.ballAVY, f.ballAVZ, static_cast<int>(kTickRate * 6), livePred);
		}

		if (goalPending)
		{
			goalPending = false;
			if (pendingEndReason != "goal")
			{
				// A goal right after a floor bounce still counts as a goal
				if (pendingEndReason == "floor") floorBeforeGoal = true;
				pendingEndReason = "goal";
				eventTick = tick;
			}
		}
		else if (pendingEndReason.empty())
		{
			// Aerials end when the ball hits the floor; ground shots when the ball stops heading for the goal
			std::string ended = endTracker.Update(frames.back(), hadPrev, prevBallZ, static_cast<int>(frames.size()), eventTick);
			// Ground shot just left your car: if the simulator says it can't go in, call it a miss now,
			// so resetting right away still keeps the rep (a goal during the post-roll still counts)
			int lt = endTracker.LastTouchTick();
			if (ended.empty() && endTracker.GroundPlay() && !endTracker.Carrying() && lt >= 0 && lt != predictedTouch && tick >= lt + 3 && predictor.Ready())
			{
				predictedTouch = lt;
				const Frame& f = frames.back();
				std::vector<BallSample> path;
				if (predictor.Predict(BallSample{ f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ },
					f.ballAVX, f.ballAVY, f.ballAVZ, static_cast<int>(kTickRate * 3), path) && !path.empty())
				{
					const BallSample& last = path.back();
					bool goes_in = std::fabs(last.y) > 5215.0f && std::fabs(last.x) < 893.0f && last.z < 643.0f;
					// Heading for the backboard, ceiling or a wall top = a double tap setup, not a miss
					bool toBackboard = false;
					for (const BallSample& b : path)
					{
						float ax = std::fabs(b.x), ay = std::fabs(b.y);
						bool backboard = ay > 4900.0f && b.z > 200.0f && !(ax < 893.0f && b.z < 643.0f);
						bool ceiling = b.z > 1800.0f;
						bool wallTop = b.z > 1000.0f && (ax > 3800.0f || ax + ay > 7700.0f);
						if (backboard || ceiling || wallTop) { toBackboard = true; break; }
					}
					if (!goes_in && !toBackboard) { ended = "miss"; eventTick = tick; }
				}
			}
			if (!ended.empty())
			{
				pendingEndReason = ended;
			}
			// Mid-dribble: give it up to 20 more seconds so the flick still makes the rep
			else if (elapsed >= *maxRepSeconds && !(endTracker.Carrying() && elapsed < *maxRepSeconds + 20.0f))
			{
				eventTick = tick;
				FinishRep("timeout");
				state = RepState::WaitReset;
				break;
			}
		}
		else
		{
			int unused = 0;
			endTracker.Update(frames.back(), hadPrev, prevBallZ, static_cast<int>(frames.size()), unused);
		}

		// After the shot ends, keep recording so late goals and your recovery are captured:
		// at least the minimum window, then until you're back in control (or the max runs out)
		if (!pendingEndReason.empty())
		{
			recoverRun = Segmenter::InControl(frames.back()) ? recoverRun + 1 : 0;
			if (recoverRun >= 30) recovered = true;
			int since = tick - eventTick;
			bool minDone = since >= static_cast<int>(*postRollSeconds * kTickRate);
			bool maxDone = since >= static_cast<int>(*postRollMaxSeconds * kTickRate);
			bool needRecovery = touchCount > 0 && !recovered;
			// A slow shot that's still rolling or bouncing toward the net: keep going until it's in or stops
			// (up to 6 s more), so late goals aren't saved as misses
			const Frame& lf = frames.back();
			float dirY = lf.ballVY >= 0.0f ? 1.0f : -1.0f;
			float toLine = 5120.0f - lf.ballY * dirY;
			float tLine = std::fabs(lf.ballVY) > 1.0f ? toLine / std::fabs(lf.ballVY) : 1e9f;
			bool rollingIn = std::fabs(lf.ballVY) > 300.0f && toLine > 0.0f && tLine < 6.0f
				&& std::fabs(lf.ballX + lf.ballVX * tLine) < 950.0f;
			bool hardMax = since >= static_cast<int>((*postRollMaxSeconds + 6.0f) * kTickRate);
			if (((minDone && !needRecovery) || maxDone) && (!rollingIn || hardMax))
			{
				FinishRep(pendingEndReason);
				state = RepState::WaitReset;
			}
		}
		break;
	}

	default:
		break;
	}
}

Frame RLCoach::Capture(CarWrapper& car, BallWrapper& ball, const ControllerInput& in)
{
	float wallMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - repStartTime).count();
	Frame f = CaptureAt(car, ball, in, static_cast<int>(frames.size()), wallMs);
	if (touchPending)
	{
		f.touch = true;
		f.touchX = touchLoc.X; f.touchY = touchLoc.Y; f.touchZ = touchLoc.Z;
		touchPending = false;
	}
	return f;
}

// One tick of car, ball and input state (no touch info)
Frame RLCoach::CaptureAt(CarWrapper& car, BallWrapper& ball, const ControllerInput& in, int tick, float wallMs)
{
	Frame f;
	f.tick = tick;
	f.t = f.tick / kTickRate;
	f.wallMs = wallMs;

	f.throttle = in.Throttle;
	f.steer = in.Steer;
	f.pitch = in.Pitch;
	f.yaw = in.Yaw;
	f.roll = in.Roll;
	f.jump = in.Jump != 0;
	f.boost = in.ActivateBoost != 0 || in.HoldingBoost != 0;
	f.handbrake = in.Handbrake != 0;

	Vector loc = car.GetLocation();
	Rotator rot = car.GetRotation();
	Vector vel = car.GetVelocity();
	Vector ang = car.GetAngularVelocity();
	f.carX = loc.X; f.carY = loc.Y; f.carZ = loc.Z;
	f.carPitch = rot.Pitch; f.carYaw = rot.Yaw; f.carRoll = rot.Roll;
	f.carVX = vel.X; f.carVY = vel.Y; f.carVZ = vel.Z;
	f.carAVX = ang.X; f.carAVY = ang.Y; f.carAVZ = ang.Z;

	BoostWrapper boostComp = car.GetBoostComponent();
	f.boostAmount = boostComp.IsNull() ? 0.0f : boostComp.GetCurrentBoostAmount() * 100.0f;
	f.onGround = car.IsOnGround();
	f.onWall = car.IsOnWall();
	f.hasFlip = car.HasFlip() != 0;

	Vector bLoc = ball.GetLocation();
	Vector bVel = ball.GetVelocity();
	Vector bAng = ball.GetAngularVelocity();
	f.ballX = bLoc.X; f.ballY = bLoc.Y; f.ballZ = bLoc.Z;
	f.ballVX = bVel.X; f.ballVY = bVel.Y; f.ballVZ = bVel.Z;
	f.ballAVX = bAng.X; f.ballAVY = bAng.Y; f.ballAVZ = bAng.Z;
	return f;
}

void RLCoach::OnBallTouch(CarWrapper car, void* params)
{
	if (practiceState == PracticeState::Live && !car.IsNull())
	{
		CarWrapper me = gameWrapper->GetLocalCar();
		if (me.IsNull() || me.memory_address != car.memory_address) return;
		struct HitP { uintptr_t ball; float hx, hy, hz; float nx, ny, nz; };
		if (params)
		{
			const HitP* p = reinterpret_cast<const HitP*>(params);
			practiceTouchLoc = Vector(p->hx, p->hy, p->hz);
		}
		practiceTouchPending = true;
		return;
	}
	if (state != RepState::Recording || car.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	if (local.IsNull() || local.memory_address != car.memory_address) return;

	touchPending = true;

	// Game passes: ball pointer, hit location, hit normal
	struct HitParams { uintptr_t ball; float hx, hy, hz; float nx, ny, nz; };
	if (params)
	{
		const HitParams* p = reinterpret_cast<const HitParams*>(params);
		touchLoc = Vector(p->hx, p->hy, p->hz);
	}

	int tickNow = static_cast<int>(frames.size());
	if (tickNow - lastTouchTick > kTouchDebounceTicks) touchCount++;
	lastTouchTick = tickNow;
}

void RLCoach::OnGoal()
{
	if (practiceState == PracticeState::Live) practiceGoalPending = true;
	if (state == RepState::Recording) goalPending = true;
}

void RLCoach::GradeFinishedRep(const RepSummary& s)
{
	lastRepGradeLines.clear();
	if (!writer || frames.size() < 10) return;
	if (s.endReason == "reset")
	{
		lastRepGradeLines.push_back({ { { "Reset before the shot finished: not graded", 160, 160, 160 } } });
		return;
	}

	ShotAnalysis shot = ShotGeometry::Analyze(frames);
	RepSegments segs = Segmenter::Split(frames, s.eventTick);

	// Compare against your fastest goal on this shot so far (skipped if the shot book is still loading)
	ShotReference ref;
	shotBook.TryGet(s.shotKey, ref);
	RepGrade g = Grader::GradeRep(frames, segs, shot, s.goal, s.floorBeforeGoal, ref);

	if (shot.valid)
	{
		float jumpSec = -1.0f;
		for (const Segment& sg : segs.segs)
			if (sg.kind == SegKind::Takeoff && sg.start < shot.touchTick) jumpSec = (sg.start - shot.startTick) / 120.0f;
		shotBook.TryAdd(writer->Folder(), s.fileName, s.shotKey, s.goal, (shot.touchTick - shot.startTick) / 120.0f, jumpSec, shot.doubleTap);
	}

	auto gradeRgb = [](Grade gr, unsigned char& r, unsigned char& gg, unsigned char& b) {
		switch (gr)
		{
		case Grade::Clean: case Grade::Calculated: r = 115; gg = 230; b = 115; break;
		case Grade::Solid: r = 153; gg = 204; b = 255; break;
		case Grade::Sloppy: r = 255; gg = 204; b = 77; break;
		default: r = 255; gg = 115; b = 102; break;
		}
	};

	unsigned char orr, og, ob;
	OverallRgb(g.overall, orr, og, ob);
	lastRepGradeLines.push_back({ { { std::format("GRADE {}: {}  ({}){}", g.overall, Grader::OverallName(g.overall), g.overallWhy,
		!shot.valid || shot.type == ShotType::Aerial ? "" : shot.type == ShotType::Ground ? "  [ground shot]"
		: shot.type == ShotType::Dribble ? "  [dribble]" : shot.type == ShotType::AirDribble ? "  [air dribble]"
		: shot.type == ShotType::FlipReset ? "  [flip reset]" : "  [double tap]"), orr, og, ob } } });

	// One line of segment grades, each grade in its own color (same colors as the rep viewer)
	PanelGradeLine chips;
	for (const SegmentGrade& sgd : g.segs)
	{
		if (sgd.grade == Grade::None) continue;
		unsigned char r, gg, b;
		gradeRgb(sgd.grade, r, gg, b);
		chips.chunks.push_back({ std::format("{}{} ", chips.chunks.empty() ? "" : "   ", Segmenter::Name(segs.segs[sgd.segIndex].kind)), 190, 190, 190 });
		chips.chunks.push_back({ Grader::Name(sgd.grade), r, gg, b });
	}
	if (!chips.chunks.empty()) lastRepGradeLines.push_back(chips);

	if (g.biggestFix >= 0)
	{
		const SegmentGrade& fix = g.segs[g.biggestFix];
		unsigned char r, gg, b;
		gradeRgb(fix.grade, r, gg, b);
		lastRepGradeLines.push_back({ { { std::format("Fix: {} - {}", Segmenter::Name(segs.segs[fix.segIndex].kind), fix.why), r, gg, b } } });
	}
	else if (g.overall >= 4)
	{
		lastRepGradeLines.push_back({ { { "Nothing to fix on this one", 115, 230, 115 } } });
	}

	// v2 coaching: the root cause first, then one more (numbers are in the rep viewer, on hover)
	for (const CoachNote& n : coachBook.Notes(frames, shot, g.overall, habitBook))
		lastRepGradeLines.push_back({ { { n.label + " " + n.cue, 255, 205, 130 } } });

	gradeBook.TrySet(s.fileName, GradeBook::Make(g, shot, segs, s.shotKey, ref.bestFile, false));
}

void RLCoach::FinishRep(const std::string& reason)
{
	// A reset right after starting is just noise, don't save it
	if (reason == "reset" && frames.size() < 24)
	{
		frames.clear();
		pendingEndReason.clear();
		eventTick = -1;
		queuedRating = 0;
		queuedRecovery = 0;
		return;
	}

	RepSummary s;
	s.mode = gameWrapper->IsInCustomTraining() ? "training" : "freeplay";
	s.map = gameWrapper->GetCurrentMap();
	s.endReason = reason;
	s.ticks = static_cast<int>(frames.size());
	s.durationS = frames.empty() ? 0.0f : frames.back().t;
	s.touches = touchCount;
	s.goal = (reason == "goal");
	s.eventTick = eventTick >= 0 ? eventTick : (s.ticks > 0 ? s.ticks - 1 : 0);
	s.floorBeforeGoal = floorBeforeGoal;
	if (!frames.empty())
	{
		// Starting ball and car spots, rounded to 50 units, so attempts at the same shot group together
		auto r50 = [](float v) { return static_cast<int>(std::lround(v / 50.0f)) * 50; };
		const Frame& f0 = frames.front();
		s.shotKey = std::format("{}_{}_{}_{}_{}", r50(f0.ballX), r50(f0.ballY), r50(f0.ballZ), r50(f0.carX), r50(f0.carY));
	}

	bool ok = writer && writer->SaveRep(frames, s);
	if (ok) GradeFinishedRep(s);
	frames.clear();
	livePred.clear();
	pendingEndReason.clear();
	eventTick = -1;
	floorBeforeGoal = false;
	int ratingToApply = queuedRating;
	int recoveryToApply = queuedRecovery;
	queuedRating = 0;
	queuedRecovery = 0;

	if (ok)
	{
		lastRep = s;
		haveLastRep = true;
		lastRating = 0;
		lastRecovery = 0;
		repsThisSession++;
		statusMessage = writer->FailedSaves() > 0
			? std::format("Rep {} saved ({}), but {} save(s) failed this session", repsThisSession, reason, writer->FailedSaves())
			: std::format("Rep {} saved ({})", repsThisSession, reason);
		if (ratingToApply > 0) RateLastRep(ratingToApply);
		if (recoveryToApply > 0) RateLastRecovery(recoveryToApply);
	}
	else
	{
		statusMessage = "Couldn't save the rep file";
		LOG("RL Coach: failed to save rep to {}", writer ? writer->Folder().string() : "(no folder)");
	}
}

// ============================================================
// Rating
// ============================================================

void RLCoach::LoadRatingKeys()
{
	bool numpadKeys = cvarManager->getCvar("rlcoach_rate_numpad").getBoolValue();
	const char* row[8] = { "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight" };
	const char* pad[8] = { "NumPadOne", "NumPadTwo", "NumPadThree", "NumPadFour", "NumPadFive", "NumPadSix", "NumPadSeven", "NumPadEight" };
	for (int i = 0; i < 8; i++)
	{
		keyIndex[i] = gameWrapper->GetFNameIndexByString(numpadKeys ? pad[i] : row[i]);
		keyWasDown[i] = false;
	}
}

void RLCoach::UpdateRatingKeys()
{
	for (int i = 0; i < 8; i++)
	{
		bool down = gameWrapper->IsKeyPressed(keyIndex[i]);
		if (down && !keyWasDown[i] && *recordingEnabled && *manualGrading && IsOfflinePractice())
		{
			if (i < 5) HandleRatingPress(i + 1);
			else HandleRecoveryPress(i - 4);   // 6 poor, 7 eh, 8 good
		}
		keyWasDown[i] = down;
	}
}

void RLCoach::HandleRatingPress(int rating)
{
	if (rating < 1 || rating > 5) return;
	if (state == RepState::Recording)
	{
		// Pressed during a rep: it belongs to this rep, not the previous one
		queuedRating = rating;
		statusMessage = std::format("Rating {} ({}) saved when this rep ends", rating, RatingName(rating));
	}
	else
	{
		RateLastRep(rating);
	}
}

void RLCoach::HandleRecoveryPress(int rating)
{
	if (rating < 1 || rating > 3) return;
	if (state == RepState::Recording)
	{
		queuedRecovery = rating;
		statusMessage = std::format("Recovery {} saved when this rep ends", RecoveryName(rating));
	}
	else
	{
		RateLastRecovery(rating);
	}
}

void RLCoach::RateLastRecovery(int rating)
{
	if (!haveLastRep || rating < 1 || rating > 3 || !writer) return;
	writer->SaveRecoveryRating(lastRep.fileName, rating);
	lastRecovery = rating;
	statusMessage = std::format("Rated rep {}'s recovery: {}", repsThisSession, RecoveryName(rating));
	repListStale = true;
}

void RLCoach::SetRecoveryRating(const std::string& file, int rating)
{
	if (!writer || rating < 1 || rating > 3) return;
	for (RepListItem& it : repList)
	{
		if (it.file != file) continue;
		it.recoveryRating = rating;
		writer->SaveRecoveryRating(file, rating);
		return;
	}
}

void RLCoach::RateLastRep(int rating)
{
	if (!haveLastRep || rating < 1 || rating > 5 || !writer) return;
	if (writer->SaveRating(lastRep.fileName, rating))
	{
		lastRating = rating;
		statusMessage = std::format("Rated rep {}: {} ({})", repsThisSession, rating, RatingName(rating));
	}
	else
	{
		statusMessage = "Couldn't save the rating";
	}
}

// ============================================================
// Test tools
// ============================================================

void RLCoach::TestSetState()
{
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) { LOG("RL Coach test: not in an offline game"); return; }
	CarWrapper car = gameWrapper->GetLocalCar();
	BallWrapper ball = server.GetBall();
	if (car.IsNull() || ball.IsNull()) { LOG("RL Coach test: no car or ball"); return; }

	// Car near the blue goal facing the ball, ball floating mid-field drifting toward it
	car.SetLocation(Vector(0.0f, -2500.0f, 20.0f));
	car.SetRotation(Rotator(0, 16384, 0));
	car.SetVelocity(Vector(0.0f, 0.0f, 0.0f));
	car.SetAngularVelocity(Vector(0.0f, 0.0f, 0.0f), false);
	BoostWrapper boostComp = car.GetBoostComponent();
	if (!boostComp.IsNull()) boostComp.SetCurrentBoostAmount(0.33f);

	ball.SetLocation(Vector(0.0f, 0.0f, 900.0f));
	ball.SetVelocity(Vector(0.0f, -400.0f, 200.0f));
	ball.SetAngularVelocity(Vector(0.0f, 0.0f, 0.0f), false);

	statusMessage = "Test: car and ball moved (boost set to 33)";
	LOG("RL Coach test: set state applied");
}

// ============================================================
// Settings tab (F2 > Plugins > RLCoach)
// ============================================================

void RLCoach::RenderSettings()
{
	UiStyle::Scope style(false);   // RL Coach's look for the widgets in this tab

	// This runs on the render thread, so cvar changes are handed to the game thread
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	ImGui::TextColored(UiStyle::kAccent, "RL COACH");
	ImGui::SameLine();
	ImGui::TextDisabled("v%s   %d reps saved this session", plugin_version, repsThisSession);

	bool rec = *recordingEnabled;
	if (ImGui::Checkbox("Record reps", &rec)) setCvar("rlcoach_recording", rec ? 1.0f : 0.0f);
	ImGui::SameLine();
	bool manual = *manualGrading;
	if (ImGui::Checkbox("Manual grading", &manual)) setCvar("rlcoach_manual_grading", manual ? 1.0f : 0.0f);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rate reps with the number keys: 1-5 the shot, 6-8 the recovery.\nTurn off to free up those keys.");
	ImGui::SameLine();
	bool hud = *hudEnabled;
	if (ImGui::Checkbox("Show panel", &hud)) setCvar("rlcoach_hud", hud ? 1.0f : 0.0f);
	ImGui::SameLine();
	if (UiStyle::AccentButton("Open rep viewer"))
		gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });

	if (ImGui::CollapsingHeader("Recording and rating"))
	{
		bool np = *useNumpad;
		if (ImGui::Checkbox("Rate with the numpad instead of the number row", &np)) setCvar("rlcoach_rate_numpad", np ? 1.0f : 0.0f);
		float maxS = *maxRepSeconds;
		if (ImGui::SliderFloat("Max rep length (seconds)", &maxS, 2.0f, 30.0f, "%.0f")) setCvar("rlcoach_max_rep_seconds", maxS);
		float post = *postRollSeconds;
		if (ImGui::SliderFloat("Keep recording after the shot, at least (s)", &post, 0.0f, 5.0f, "%.1f")) setCvar("rlcoach_postroll_seconds", post);
		float postMax = *postRollMaxSeconds;
		if (ImGui::SliderFloat("...and until recovered, at most (s)", &postMax, 1.0f, 8.0f, "%.1f")) setCvar("rlcoach_postroll_max_seconds", postMax);
		ImGui::TextDisabled("Rating keys: 1 Missed, 2 Close, 3 Bad shot went in, 4 Good shot went in, 5 Great shot went in.");
		ImGui::TextDisabled("Recovery keys: 6 Poor, 7 Eh, 8 Good (rate how you got back in control after the shot).");
		ImGui::TextDisabled("Pressing during a rep rates that rep; after, it rates the last saved rep.");
		ImGui::TextDisabled("Controller: bind rlcoach_toggle_recording / rlcoach_viewer / rlcoach_rate 1-5 /");
		ImGui::TextDisabled("rlcoach_rate_recovery 1-3 in BakkesMod's Bindings tab.");
	}

	if (ImGui::CollapsingHeader("Panel and units")) RenderLookSettings();
	if (ImGui::CollapsingHeader("Controller overlay")) RenderPadSettings();
	if (ImGui::CollapsingHeader("Storage and sharing")) RenderStorageSettings();

	if (ImGui::CollapsingHeader("Coaching checks"))
	{
		ImGui::TextWrapped("Known mistakes for each mechanic, from how top players coach it. A check only shows up in your "
			"notes once your rated reps back it: it has to happen clearly more on your misses than on your goals.");
		std::vector<std::string> flicks = habitBook.FlickSummary();
		if (!flicks.empty())
		{
			ImGui::TextDisabled("Your flicks off a dribble:");
			for (const std::string& f : flicks) ImGui::BulletText("%s", f.c_str());
		}
		std::vector<std::string> lines = coachBook.Summary();
		if (lines.empty()) ImGui::TextDisabled("Still measuring your reps...");
		for (const std::string& l : lines)
		{
			bool on = l.rfind("[on]", 0) == 0;
			ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
			ImGui::TextWrapped("%s", l.c_str());
			ImGui::PopStyleColor();
		}
	}

	if (ImGui::CollapsingHeader("Ball prediction (RocketSim)"))
	{
		ImGui::Text("Status: %s", predictor.Status().c_str());
		if (!predictor.Ready())
		{
			ImGui::TextWrapped("Needs arena files: download RLArenaCollisionDumper.exe, put it in the folder below, "
				"launch Rocket League without EAC, go into free play on a standard arena (like DFH Stadium), then run the exe. "
				"It creates a collision_meshes folder next to itself. Then click Load.");
			if (writer) ImGui::TextWrapped("Folder: %s", writer->Folder().string().c_str());
			if (ImGui::Button("Load arena files")) gameWrapper->Execute([this](GameWrapper*) { StartPredictor(); });
		}
		else
		{
			bool lp = *livePrediction;
			if (ImGui::Checkbox("Show predicted ball path while playing", &lp)) setCvar("rlcoach_live_prediction", lp ? 1.0f : 0.0f);
			if (ImGui::Button("Run accuracy test on saved reps"))
			{
				auto root = writer->Folder();
				predictor.StartAccuracyTest(root);
			}
			std::string ts = predictor.TestStatus();
			if (!ts.empty()) ImGui::TextWrapped("%s", ts.c_str());
			bool mir = predictor.mirrorX;
			if (ImGui::Checkbox("Mirror X (only if the accuracy test says mirrored is better)", &mir)) setCvar("rlcoach_pred_mirror", mir ? 1.0f : 0.0f);
		}
	}

	if (ImGui::CollapsingHeader("Tools"))
	{
		bool st = *showTiming;
		if (ImGui::Checkbox("Show plugin timing on the panel", &st)) setCvar("rlcoach_show_timing", st ? 1.0f : 0.0f);
		ImGui::SameLine();
		ImGui::TextDisabled("(how many milliseconds RL Coach adds; a 240 fps frame is 4.2 ms)");
		if (writer) ImGui::TextWrapped("Data folder: %s", writer->Folder().string().c_str());
		if (ImGui::Button("Test: move car and ball"))
			gameWrapper->Execute([this](GameWrapper*) { TestSetState(); });
		ImGui::SameLine();
		ImGui::TextDisabled("(try it in free play and in a training pack)");
	}
}
