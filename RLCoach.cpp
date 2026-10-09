#include "pch.h"
#include "RLCoach.h"
#include "bakkesmod/wrappers/includes.h"

#include <algorithm>
#include <cmath>
#include <format>


BAKKESMOD_PLUGIN(RLCoach, "RL Coach: chess.com-style review for your training reps", plugin_version, PLUGINTYPE_FREEPLAY | PLUGINTYPE_CUSTOM_TRAINING)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

namespace
{
	constexpr float kTickRate = 120.0f;
	constexpr float kTeleportDist = 400.0f;   // a jump this big in one tick means the shot or ball was reset
	constexpr float kFloorZ = 110.0f;         // ball center height that counts as hitting the floor (ball radius is about 93)
	constexpr int   kMinTicksBeforeFloor = 30;
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

	// Colors for 1-5 grades, shared by the panel and the rep list
	void OverallRgb(int o, unsigned char& r, unsigned char& g, unsigned char& b)
	{
		if (o >= 4) { r = 115; g = 230; b = 115; }
		else if (o == 3) { r = 255; g = 204; b = 77; }
		else if (o == 2) { r = 255; g = 160; b = 80; }
		else if (o == 1) { r = 255; g = 115; b = 102; }
		else { r = 160; g = 160; b = 160; }
	}

	const char* RatingName(int r)
	{
		switch (r)
		{
		case 1: return "Missed";
		case 2: return "Close";
		case 3: return "Bad shot, went in";
		case 4: return "Good shot, went in";
		case 5: return "Great shot, went in";
		default: return "";
		}
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

	CVarWrapper numpad = cvarManager->registerCvar("rlcoach_rate_numpad", "0", "Rate reps with numpad 1-5 instead of the number row", true, true, 0, true, 1);
	numpad.bindTo(useNumpad);
	numpad.addOnValueChanged([this](std::string, CVarWrapper) { LoadRatingKeys(); });

	cvarManager->registerNotifier("rlcoach_toggle_recording", [this](std::vector<std::string>) {
		cvarManager->getCvar("rlcoach_recording").setValue(*recordingEnabled ? 0 : 1);
	}, "Turn rep recording on or off", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_rate", [this](std::vector<std::string> args) {
		if (args.size() < 2) return;
		try { HandleRatingPress(std::stoi(args[1])); } catch (...) {}
	}, "Rate the last rep 1-5", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_rearm", [this](std::vector<std::string>) {
		if (*recordingEnabled && state != RepState::Recording) EnterArmed();
	}, "Arm the recorder without resetting the ball", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_viewer", [this](std::vector<std::string>) {
		cvarManager->executeCommand("togglemenu " + GetMenuName());
	}, "Open or close the rep viewer", PERMISSION_ALL);

	cvarManager->registerNotifier("rlcoach_test_setstate", [this](std::vector<std::string>) {
		TestSetState();
	}, "Test: move the car and ball to a fixed spot", PERMISSION_ALL);

	menuTitle_ = "RL Coach - Rep Viewer";

	writer = std::make_unique<RepWriter>(std::filesystem::path(gameWrapper->GetDataFolderW()) / "RLCoach");

	// Every physics tick, before the game reads the car's input (the viewer replaces it)
	gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper car, void* params, std::string) { OverrideViewerInput(car, params); });

	// Every physics tick, after the car's input for that tick is set
	gameWrapper->HookEventWithCallerPost<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper car, void*, std::string) { OnVehicleInput(car); });

	gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.OnHitBall",
		[this](CarWrapper car, void* params, std::string) { OnBallTouch(car, params); });

	gameWrapper->HookEvent("Function TAGame.Ball_TA.OnHitGoal",
		[this](std::string) { OnGoal(); });

	gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { RenderHud(canvas); });

	LoadRatingKeys();
	predictor.mirrorX = cvarManager->getCvar("rlcoach_pred_mirror").getBoolValue();
	StartPredictor();

	// Build the shot book in the background so the first rep you open doesn't stall
	{
		auto root = writer->Folder();
		shotBookThread = std::thread([this, root]() { shotBook.Update(root); });
	}
	LOG("RL Coach loaded. Reps save to {}", writer->Folder().string());
}

void RLCoach::StartPredictor()
{
	if (writer) predictor.StartInit(writer->Folder() / "collision_meshes");
}

void RLCoach::onUnload()
{
	ViewerExit();
	if (shotBookThread.joinable()) shotBookThread.join();
	if (gradeThread.joinable()) gradeThread.join();
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
			recoverRun = 0;
			recovered = false;
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
			if (hadPrev && static_cast<int>(frames.size()) > kMinTicksBeforeFloor && ballLoc.Z < kFloorZ && prevBallZ >= kFloorZ)
			{
				pendingEndReason = "floor";
				eventTick = tick;
			}
			else if (elapsed >= *maxRepSeconds)
			{
				eventTick = tick;
				FinishRep("timeout");
				state = RepState::WaitReset;
				break;
			}
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
			if ((minDone && !needRecovery) || maxDone)
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
	Frame f;
	f.tick = static_cast<int>(frames.size());
	f.t = f.tick / kTickRate;
	f.wallMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - repStartTime).count();

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

	if (touchPending)
	{
		f.touch = true;
		f.touchX = touchLoc.X; f.touchY = touchLoc.Y; f.touchZ = touchLoc.Z;
		touchPending = false;
	}
	return f;
}

void RLCoach::OnBallTouch(CarWrapper car, void* params)
{
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
		shotBook.TryAdd(writer->Folder(), s.fileName, s.shotKey, s.goal, (shot.touchTick - shot.startTick) / 120.0f, jumpSec);
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
	lastRepGradeLines.push_back({ { { std::format("GRADE {}: {}  ({})", g.overall, Grader::OverallName(g.overall), g.overallWhy), orr, og, ob } } });

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

	gradeBook.TrySet(s.fileName, g.overall, s.shotKey, ref.bestFile);
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
	queuedRating = 0;

	if (ok)
	{
		lastRep = s;
		haveLastRep = true;
		lastRating = 0;
		repsThisSession++;
		statusMessage = std::format("Rep {} saved ({})", repsThisSession, reason);
		if (ratingToApply > 0) RateLastRep(ratingToApply);
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
	const char* row[5] = { "One", "Two", "Three", "Four", "Five" };
	const char* pad[5] = { "NumPadOne", "NumPadTwo", "NumPadThree", "NumPadFour", "NumPadFive" };
	for (int i = 0; i < 5; i++)
	{
		keyIndex[i] = gameWrapper->GetFNameIndexByString(numpadKeys ? pad[i] : row[i]);
		keyWasDown[i] = false;
	}
}

void RLCoach::UpdateRatingKeys()
{
	for (int i = 0; i < 5; i++)
	{
		bool down = gameWrapper->IsKeyPressed(keyIndex[i]);
		if (down && !keyWasDown[i] && *recordingEnabled && IsOfflinePractice())
			HandleRatingPress(i + 1);
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
// Drawing
// ============================================================

void RLCoach::RenderHud(CanvasWrapper canvas)
{
	UpdateRatingKeys();

	if (!*hudEnabled || !IsOfflinePractice()) return;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (car.IsNull()) return;

	float speed = car.GetVelocity().magnitude();
	int boost = 0;
	BoostWrapper boostComp = car.GetBoostComponent();
	if (!boostComp.IsNull()) boost = static_cast<int>(boostComp.GetCurrentBoostAmount() * 100.0f + 0.5f);

	// Build the lines first so the panel can size itself
	struct Line { std::vector<PanelChunk> chunks; float scale; };
	std::vector<Line> lines;
	auto add = [&](std::string text, unsigned char r = 230, unsigned char g = 230, unsigned char b = 230, float scale = 1.0f) {
		lines.push_back({ { { std::move(text), r, g, b } }, scale });
	};

	add("RL COACH", 255, 255, 255, 1.25f);

	if (viewerActive && !viewerFrames.empty())
	{
		DrawViewerPaths(canvas);

		int i = std::min(static_cast<int>(viewerPlayhead), static_cast<int>(viewerFrames.size()) - 1);
		const Frame& f = viewerFrames[i];
		std::string when = viewerRep.savedAt.size() >= 19 ? viewerRep.savedAt.substr(5, 11) : viewerRep.savedAt;
		add(std::format("VIEWER  {}  {}  {}", when, viewerRep.endReason,
			viewerRep.rating > 0 ? std::format("rated {}", viewerRep.rating) : std::string("unrated")), 120, 200, 255);
		add(std::format("{:.2f}s / {:.2f}s   {}   {}x", f.t, viewerFrames.back().t,
			viewerPlaying ? "PLAYING" : "PAUSED", viewerSpeed));
		float spd = std::sqrt(f.carVX * f.carVX + f.carVY * f.carVY + f.carVZ * f.carVZ);
		add(std::format("Speed {:.0f}   Boost {:.0f}   {}{}", spd, f.boostAmount,
			f.onGround ? "Grounded" : "Airborne", f.touch ? "   TOUCH" : ""));
		add(std::format("Thr {:+.2f}  Str {:+.2f}  P {:+.2f}  Y {:+.2f}  R {:+.2f}",
			f.throttle, f.steer, f.pitch, f.yaw, f.roll), 180, 220, 255);
		add(std::format("Jump {}   Boost {}   Slide {}", f.jump ? "X" : "-", f.boost ? "X" : "-", f.handbrake ? "X" : "-"), 180, 220, 255);
		if (viewerGrade.overall > 0)
			add(std::format("GRADE {}: {}  ({})", viewerGrade.overall, Grader::OverallName(viewerGrade.overall), viewerGrade.overallWhy),
				viewerGrade.overall >= 4 ? 120 : 255, viewerGrade.overall >= 4 ? 230 : (viewerGrade.overall == 3 ? 200 : 120), 120);
		if (viewerGrade.biggestFix >= 0)
		{
			const SegmentGrade& fix = viewerGrade.segs[viewerGrade.biggestFix];
			add(std::format("Biggest fix: {} ({}): {}", Segmenter::Name(viewerSegs.segs[fix.segIndex].kind), Grader::Name(fix.grade), fix.why), 255, 200, 0);
		}
		for (int si = 0; si < static_cast<int>(viewerSegs.segs.size()); si++)
		{
			const Segment& sg = viewerSegs.segs[si];
			if (i < sg.start || i > sg.end || sg.kind == SegKind::Recovery) continue;
			std::string gradeText;
			for (const SegmentGrade& gs : viewerGrade.segs)
				if (gs.segIndex == si && gs.grade != Grade::None) gradeText = std::format(": {} - {}", Grader::Name(gs.grade), gs.why);
			add(std::format("Now: {}{}{}", Segmenter::Name(sg.kind), sg.touch > 1 ? std::format(" (touch {})", sg.touch) : std::string(), gradeText), 255, 255, 255);
		}
		if (viewerSegs.whiff) add("Whiff: no touch on this rep", 255, 120, 120);

		if (viewerShot.valid)
		{
			const ShotAnalysis& s = viewerShot;
			if (!s.reachesGoalLine)
				add("Shot: never reached the goal line", 255, 120, 120);
			else if (s.missDist <= 0.0f)
				add("Shot: on target", 120, 230, 120);
			else
				add(std::format("Shot: missed {} by {:.0f}", s.missDir, s.missDist), 255, 120, 120);
			add(std::format("Aim: {:.1f} deg off   Contact: {:.0f} {} / {:.0f} {} of ideal",
				s.aimErrDeg,
				std::fabs(s.contactUp), s.contactUp >= 0 ? "high" : "low",
				std::fabs(s.contactLeft), s.contactLeft >= 0 ? "left" : "right"));
			if (s.earliestTick >= 0)
				add(std::format("Touch came {:.2f}s after the earliest possible intercept", s.lateBy));
			add("Yellow: ideal hit spot + direction   Green line: your shot   Ring: earliest intercept", 160, 160, 160);
		}
		add(viewerPred.empty() ? "Orange: ball   Blue: car   Green: touches"
			: "Orange: ball   Blue: car   Green: touches   White dashes: predicted ball", 160, 160, 160);
		if (!viewerStatus.empty()) add(viewerStatus, 255, 200, 0);
	}
	else
	{
		if (state == RepState::Recording && *livePrediction && !livePred.empty()) DrawPredictedPath(canvas, livePred);

	if (!*recordingEnabled)
		add("Recording off  (F2 > Plugins > RLCoach)", 150, 150, 150);
	else if (state == RepState::Recording && !pendingEndReason.empty())
		add(std::format("REC  shot over ({}), recording recovery", pendingEndReason), 255, 120, 120);
	else if (state == RepState::Recording)
		add("REC  recording rep", 255, 70, 70);
	else if (state == RepState::Armed)
		add("ARMED  rep starts on first input", 255, 200, 0);
	else
		add("WAITING  reset the shot or ball", 200, 200, 200);

	if (*recordingEnabled) add(std::format("Reps saved this session: {}", repsThisSession));

	add(std::format("Speed {:.0f}{}   Boost {}   {}", speed, speed >= 2200.0f ? " (SS)" : "", boost,
		car.IsOnGround() ? "Grounded" : "Airborne"));

	const ControllerInput& in = lastInput;
	add(std::format("Thr {:+.2f}  Str {:+.2f}  P {:+.2f}  Y {:+.2f}  R {:+.2f}",
		in.Throttle, in.Steer, in.Pitch, in.Yaw, in.Roll), 180, 220, 255);
	add(std::format("Jump {}   Boost {}   Slide {}",
		in.Jump ? "X" : "-", (in.ActivateBoost || in.HoldingBoost) ? "X" : "-", in.Handbrake ? "X" : "-"), 180, 220, 255);

	if (*recordingEnabled && state == RepState::Recording && queuedRating > 0)
		add(std::format("This rep will be rated {}: {}", queuedRating, RatingName(queuedRating)), 120, 230, 120);

	if (*recordingEnabled && haveLastRep)
	{
		add(std::format("Last rep: {}  {:.2f}s  {} touch{}", lastRep.endReason, lastRep.durationS,
			lastRep.touches, lastRep.touches == 1 ? "" : "es"));
		for (const PanelGradeLine& gl : lastRepGradeLines) lines.push_back({ gl.chunks, 1.0f });
		if (lastRating == 0)
			add("Rate it: 1 Missed  2 Close  3 Bad-in  4 Good-in  5 Great-in", 255, 200, 0);
		else
			add(std::format("Rated {}: {}", lastRating, RatingName(lastRating)), 120, 230, 120);
	}

	if (!statusMessage.empty()) add(statusMessage, 160, 160, 160);
	}

	// Panel
	const float x = 30.0f, y = 180.0f, pad = 10.0f, lineH = 18.0f;
	float height = pad * 2;
	float width = 420.0f;
	for (const Line& l : lines)
	{
		height += lineH * l.scale;
		float w = 0.0f;
		for (const PanelChunk& c : l.chunks) w += canvas.GetStringSize(c.text, l.scale, l.scale).X;
		width = std::max(width, w + pad * 2);
	}

	canvas.SetColor(0, 0, 0, 160);
	canvas.SetPosition(Vector2F{ x, y });
	canvas.FillBox(Vector2F{ width, height });

	float cy = y + pad;
	for (const Line& l : lines)
	{
		float cx = x + pad;
		for (const PanelChunk& c : l.chunks)
		{
			canvas.SetColor(c.r, c.g, c.b, 255);
			canvas.SetPosition(Vector2F{ cx, cy });
			canvas.DrawString(c.text, l.scale, l.scale, true);
			cx += canvas.GetStringSize(c.text, l.scale, l.scale).X;
		}
		cy += lineH * l.scale;
	}
}

// ============================================================
// Settings tab (F2 > Plugins > RLCoach)
// ============================================================

void RLCoach::RenderSettings()
{
	// This runs on the render thread, so cvar changes are handed to the game thread
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	bool rec = *recordingEnabled;
	if (ImGui::Checkbox("Record reps (data farming)", &rec)) setCvar("rlcoach_recording", rec ? 1.0f : 0.0f);

	bool hud = *hudEnabled;
	if (ImGui::Checkbox("Show RL Coach panel", &hud)) setCvar("rlcoach_hud", hud ? 1.0f : 0.0f);

	bool np = *useNumpad;
	if (ImGui::Checkbox("Rate with numpad 1-5 instead of the number row", &np)) setCvar("rlcoach_rate_numpad", np ? 1.0f : 0.0f);

	float maxS = *maxRepSeconds;
	if (ImGui::SliderFloat("Max rep length (seconds)", &maxS, 2.0f, 30.0f, "%.0f")) setCvar("rlcoach_max_rep_seconds", maxS);

	float post = *postRollSeconds;
	if (ImGui::SliderFloat("Keep recording after the shot, at least (seconds)", &post, 0.0f, 5.0f, "%.1f")) setCvar("rlcoach_postroll_seconds", post);
	float postMax = *postRollMaxSeconds;
	if (ImGui::SliderFloat("...and until recovered, at most (seconds)", &postMax, 1.0f, 8.0f, "%.1f")) setCvar("rlcoach_postroll_max_seconds", postMax);

	ImGui::Separator();
	ImGui::Text("Reps saved this session: %d", repsThisSession);
	ImGui::TextWrapped("Rating keys: 1 Missed, 2 Close, 3 Bad shot but went in, 4 Good shot went in, 5 Great shot went in. Pressing during a rep rates that rep; pressing after rates the last saved rep.");
	ImGui::TextWrapped("Controller: bind rlcoach_toggle_recording to a button in BakkesMod's Bindings tab to toggle recording.");
	if (writer) ImGui::TextWrapped("Reps are saved in: %s", writer->Folder().string().c_str());

	ImGui::Separator();
	ImGui::TextUnformatted("Ball prediction (RocketSim)");
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
		if (ImGui::Button("Run accuracy test on saved reps"))
		{
			auto root = writer->Folder();
			predictor.StartAccuracyTest(root);
		}
		std::string ts = predictor.TestStatus();
		if (!ts.empty()) ImGui::TextWrapped("%s", ts.c_str());
		bool lp = *livePrediction;
		if (ImGui::Checkbox("Show predicted ball path while playing", &lp)) setCvar("rlcoach_live_prediction", lp ? 1.0f : 0.0f);
		bool mir = predictor.mirrorX;
		if (ImGui::Checkbox("Mirror X (only if the accuracy test says mirrored is better)", &mir)) setCvar("rlcoach_pred_mirror", mir ? 1.0f : 0.0f);
	}

	ImGui::Separator();
	if (ImGui::Button("Open rep viewer"))
		gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
	ImGui::SameLine();
	ImGui::TextUnformatted("(or bind rlcoach_viewer to a key)");

	if (ImGui::Button("Test: move car and ball"))
		gameWrapper->Execute([this](GameWrapper*) { TestSetState(); });
	ImGui::SameLine();
	ImGui::TextUnformatted("(try it in free play AND in a training pack)");
}
