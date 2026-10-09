#include "pch.h"
#include "RLCoach.h"
#include "bakkesmod/wrappers/includes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

// ============================================================
// Practice mode: drops you into a segment of a saved rep in free play,
// frozen until your first input, then grades each attempt and resets.
// ============================================================

namespace
{
	constexpr int   kLeadInTicks = 30;       // start 0.25 s before the segment so you can make the decision
	constexpr float kTeleportDist = 400.0f;
	constexpr float kFloorZ = 110.0f;
	constexpr int   kPostTicks = 120;        // keep going 1 s after a goal / floor hit (catches late goals)
	constexpr int   kRecoveryMaxTicks = 480; // when practicing recovery, wait up to 4 s to land

	bool AnyInput(const ControllerInput& in)
	{
		const float dz = 0.1f;
		return std::fabs(in.Throttle) > dz || std::fabs(in.Steer) > dz || std::fabs(in.Pitch) > dz
			|| std::fabs(in.Yaw) > dz || std::fabs(in.Roll) > dz
			|| in.Jump || in.ActivateBoost || in.HoldingBoost || in.Handbrake;
	}

	float Dist(const Vector& a, const Vector& b)
	{
		float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	bool ToScreenP(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd, const Vector& p, Vector2F& out)
	{
		Vector rel = p - camLoc;
		if (Vector::dot(rel, camFwd) < 10.0f) return false;
		Vector2 s = canvas.Project(p);
		out = Vector2F{ static_cast<float>(s.X), static_cast<float>(s.Y) };
		return true;
	}

	void GradeRgbP(Grade gr, unsigned char& r, unsigned char& g, unsigned char& b)
	{
		switch (gr)
		{
		case Grade::Clean: case Grade::Calculated: r = 115; g = 230; b = 115; break;
		case Grade::Solid: r = 153; g = 204; b = 255; break;
		case Grade::Sloppy: r = 255; g = 204; b = 77; break;
		default: r = 255; g = 115; b = 102; break;
		}
	}
}

void RLCoach::PracticeStart(int segIndex)
{
	if (viewerFrames.empty() || segIndex < 0 || segIndex >= static_cast<int>(viewerSegs.segs.size())) return;
	const Segment seg = viewerSegs.segs[segIndex];
	PracticeBegin(seg.start, seg.kind, seg.start,
		std::format("{} ({:.2f}s into the rep)", Segmenter::Name(seg.kind), seg.start / 120.0f));
}

// "Practice from here": the moment the viewer is showing, graded on whichever segment it falls in
void RLCoach::PracticeStartAt(int tick)
{
	if (viewerFrames.empty()) return;
	tick = std::max(0, std::min(tick, static_cast<int>(viewerFrames.size()) - 1));
	SegKind kind = SegKind::Approach;
	int segStart = tick;
	for (const Segment& sg : viewerSegs.segs)
		if (sg.kind != SegKind::Recovery && tick >= sg.start && tick <= sg.end) { kind = sg.kind; segStart = sg.start; }
	PracticeBegin(tick, kind, segStart, std::format("from {:.2f}s ({})", tick / 120.0f, Segmenter::Name(kind)));
}

void RLCoach::PracticeBegin(int targetTick, SegKind kind, int segStart, const std::string& label)
{
	if (!gameWrapper->IsInFreeplay())
	{
		viewerStatus = "Practice runs in free play";
		return;
	}
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;

	bool restarting = practiceState != PracticeState::Off;
	if (!restarting)
	{
		// Keep the viewer's rep and grades on screen; just stop it from driving the car
		practiceSavedGameSpeed = viewerActive ? savedGameSpeed : server.GetGameSpeed();
	}
	else
	{
		CarWrapper car = gameWrapper->GetLocalCar();
		if (!car.IsNull()) PracticeReleaseFlip(car);
	}
	viewerPlaying = false;
	uiViewerPlaying = false;

	practiceRep = viewerFrames;
	practiceItem = viewerRep;
	practiceKind = kind;
	practiceSegStart = segStart;
	practiceTargetTick = targetTick;
	practiceLabel = label;
	practiceBallStartTick = ShotGeometry::Analyze(practiceRep).startTick;
	PracticeSetLeadIn(practiceLeadIn);

	practiceAttempts = 0;
	practiceHitsByPct.clear();
	practiceHitsTotal = 0;
	practiceStreak = 0;
	practiceResultLines.clear();
	practiceLive.clear();
	practiceState = PracticeState::Frozen;
	uiPracticeActive = true;
	uiPracticeTarget = targetTick;

	// Recorder sits out while practicing
	frames.clear();
	state = RepState::Idle;

	if (!restarting || practiceRefFrames.empty()) PracticePickReference();
	statusMessage.clear();
}

void RLCoach::PracticeSetLeadIn(float sec)
{
	practiceLeadIn = std::max(0.0f, std::min(2.0f, sec));
	uiPracticeLeadIn = practiceLeadIn;
	if (practiceRep.empty()) return;
	int lead = static_cast<int>(std::lround(practiceLeadIn * 120.0f));
	practiceStartTick = std::max(practiceBallStartTick, practiceTargetTick - lead);
	practiceStartTick = std::min(practiceStartTick, static_cast<int>(practiceRep.size()) - 1);
	ViewerSeek(static_cast<float>(practiceStartTick));   // timeline shows where attempts start

	// Flip timer: if you're airborne with a flip at the start, work out when it would have run out.
	// Rocket League gives about 1.25 s after the first jump ends (release, or 0.2 s of holding).
	practiceFlipExpireTick = -1;
	const Frame& f = practiceRep[practiceStartTick];
	if (!f.onGround && !f.onWall && f.hasFlip)
	{
		for (int j = practiceStartTick; j >= 1; j--)
		{
			if (practiceRep[j].jump && !practiceRep[j - 1].jump && (practiceRep[j].onGround || practiceRep[j - 1].onGround))
			{
				int jumpEnd = j + 24;
				for (int k = j; k < j + 24 && k < static_cast<int>(practiceRep.size()); k++)
					if (!practiceRep[k].jump) { jumpEnd = k; break; }
				practiceFlipExpireTick = jumpEnd + 150;
				break;
			}
		}
	}
}

// Your best rep on this shot: highest auto grade, then your rating, then lowest aim error, then most recent
void RLCoach::PracticePickReference()
{
	practiceRefFrames.clear();
	practiceRefLabel.clear();
	if (!writer || practiceItem.shotKey.empty()) return;

	std::unordered_map<std::string, int> autos;
	gradeBook.Copy(autos);
	std::vector<RepListItem> same;
	for (const RepListItem& it : RepLibrary::ListReps(writer->Folder()))   // newest first
		if (it.shotKey == practiceItem.shotKey && it.endReason != "reset") same.push_back(it);
	if (same.empty()) return;

	auto autoOf = [&](const RepListItem& it) { auto f = autos.find(it.file); return f == autos.end() ? 0 : f->second; };
	std::stable_sort(same.begin(), same.end(), [&](const RepListItem& a, const RepListItem& b) {
		if (autoOf(a) != autoOf(b)) return autoOf(a) > autoOf(b);
		return a.rating > b.rating;
	});

	// Among the tied leaders, lowest aim error wins (stable sort keeps the most recent first on ties)
	int bestAuto = autoOf(same.front()), bestRating = same.front().rating;
	float bestAim = 1e9f;
	std::vector<Frame> bestFrames;
	RepListItem bestItem;
	for (const RepListItem& it : same)
	{
		if (autoOf(it) != bestAuto || it.rating != bestRating) break;
		std::vector<Frame> fr;
		std::string err;
		if (!RepLibrary::LoadRep(writer->Folder() / "reps" / it.file, fr, err)) continue;
		ShotAnalysis sa = ShotGeometry::Analyze(fr);
		float aim = sa.valid ? sa.aimErrDeg : 1e8f;
		if (aim < bestAim - 0.01f) { bestAim = aim; bestFrames = std::move(fr); bestItem = it; }
	}
	if (bestFrames.empty()) return;
	practiceRefFrames = std::move(bestFrames);
	std::string when = bestItem.savedAt.size() >= 19 ? bestItem.savedAt.substr(5, 11) : bestItem.savedAt;
	practiceRefLabel = std::format("Faint line: your best on this shot (auto {}{}, {})", bestAuto,
		bestRating > 0 ? std::format(", you rated {}", bestRating) : std::string(), when);
}

void RLCoach::PracticeApplyStart(CarWrapper& car, BallWrapper& ball, bool frozen)
{
	const Frame& f = practiceRep[std::min(practiceStartTick, static_cast<int>(practiceRep.size()) - 1)];
	float v = frozen ? 0.0f : 1.0f;

	car.SetLocation(Vector(f.carX, f.carY, f.carZ));
	car.SetRotation(Rotator(f.carPitch, f.carYaw, f.carRoll));
	car.SetVelocity(Vector(f.carVX * v, f.carVY * v, f.carVZ * v));
	car.SetAngularVelocity(Vector(f.carAVX * v, f.carAVY * v, f.carAVZ * v), false);
	BoostWrapper boostComp = car.GetBoostComponent();
	if (!boostComp.IsNull()) boostComp.SetCurrentBoostAmount(f.boostAmount / 100.0f);

	// Jump state: in the air means you've jumped; no flip left means you've used your second jump too
	bool airborne = !f.onGround && !f.onWall;
	car.SetbJumped(airborne ? 1 : 0);
	car.SetbDoubleJumped(airborne && !f.hasFlip ? 1 : 0);

	// The game's own flip timer doesn't know when you jumped, so hold it open and expire the flip ourselves
	if (!frozen && airborne && f.hasFlip)
	{
		if (!practiceFlipHeld) { practiceOrigMaxDodge = car.GetMaxTimeForDodge(); practiceFlipHeld = true; }
		car.SetMaxTimeForDodge(1000.0f);
	}

	ball.SetLocation(Vector(f.ballX, f.ballY, f.ballZ));
	ball.SetVelocity(Vector(f.ballVX * v, f.ballVY * v, f.ballVZ * v));
	ball.SetAngularVelocity(Vector(f.ballAVX * v, f.ballAVY * v, f.ballAVZ * v), false);
}

void RLCoach::PracticeReleaseFlip(CarWrapper& car)
{
	if (!practiceFlipHeld) return;
	car.SetMaxTimeForDodge(practiceOrigMaxDodge);
	practiceFlipHeld = false;
}

void RLCoach::PracticeTick(CarWrapper& car, const ControllerInput& in)
{
	if (practiceRep.empty()) return;
	if (!gameWrapper->IsInFreeplay()) { PracticeExit(); return; }
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	BallWrapper ball = server.GetBall();
	if (ball.IsNull()) return;

	if (std::fabs(server.GetGameSpeed() - practiceSpeed) > 0.001f) server.SetGameSpeed(practiceSpeed);

	if (practiceState == PracticeState::Frozen)
	{
		if (!AnyInput(in))
		{
			PracticeReleaseFlip(car);
			PracticeApplyStart(car, ball, true);
			return;
		}
		// First input: restore the full moment, with real speeds, and go
		PracticeApplyStart(car, ball, false);
		practiceLive.clear();
		practicePending.clear();
		practiceEventTick = -1;
		practiceGoalPending = false;
		practiceWentIn = false;
		practiceFloorFirst = false;
		practiceTouchPending = false;
		practiceRecoverRun = 0;
		practiceHavePrev = false;
		practiceState = PracticeState::Live;
		repStartTime = std::chrono::steady_clock::now();
		return;
	}

	// ---- Live attempt ----
	Vector carLoc = car.GetLocation(), ballLoc = ball.GetLocation();
	bool teleported = practiceHavePrev && (Dist(carLoc, practicePrevCar) > kTeleportDist || Dist(ballLoc, practicePrevBall) > kTeleportDist);
	float prevBallZ = practicePrevBall.Z;
	bool hadPrev = practiceHavePrev;
	practicePrevCar = carLoc;
	practicePrevBall = ballLoc;
	practiceHavePrev = true;

	if (teleported)
	{
		// Free play reset the ball (after a goal, or you hit reset)
		PracticeFinishAttempt(practicePending.empty() ? "reset" : practicePending);
		return;
	}

	int tick = practiceStartTick + static_cast<int>(practiceLive.size());
	float wallMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - repStartTime).count();
	Frame f = CaptureAt(car, ball, in, tick, wallMs);
	if (practiceTouchPending)
	{
		f.touch = true;
		f.touchX = practiceTouchLoc.X; f.touchY = practiceTouchLoc.Y; f.touchZ = practiceTouchLoc.Z;
		practiceTouchPending = false;
	}
	practiceLive.push_back(f);
	int liveTicks = static_cast<int>(practiceLive.size());

	if (practiceFlipHeld)
	{
		if (f.onGround || f.onWall) PracticeReleaseFlip(car);            // landed: the game takes over again
		else if (practiceFlipExpireTick >= 0 && tick >= practiceFlipExpireTick)
		{
			car.SetbDoubleJumped(1);                                      // your flip would have run out here
			PracticeReleaseFlip(car);
		}
	}

	// Free play may not report goals (e.g. with goal explosions off), so also check the goal line ourselves:
	// the whole ball past the line, inside the goal mouth
	if (!practiceWentIn && std::fabs(ballLoc.Y) > 5120.0f + 93.0f && std::fabs(ballLoc.X) < 893.0f && ballLoc.Z < 643.0f)
		practiceGoalPending = true;

	if (practiceGoalPending)
	{
		practiceGoalPending = false;
		if (practicePending == "floor") practiceFloorFirst = true;
		if (practicePending != "goal") { practicePending = "goal"; practiceEventTick = tick; }
		practiceWentIn = true;
	}
	else if (practicePending.empty())
	{
		if (hadPrev && liveTicks > 30 && ballLoc.Z < kFloorZ && prevBallZ >= kFloorZ)
		{
			practicePending = "floor";
			practiceEventTick = tick;
		}
		int maxTicks = static_cast<int>(practiceRep.size()) - practiceStartTick + 240;
		if (liveTicks >= maxTicks) { practiceEventTick = tick; PracticeFinishAttempt("timeout"); return; }
	}

	if (!practicePending.empty())
	{
		int since = tick - practiceEventTick;
		if (practiceKind == SegKind::Recovery)
		{
			practiceRecoverRun = Segmenter::InControl(f) ? practiceRecoverRun + 1 : 0;
			if (practiceRecoverRun >= 30 || since >= kRecoveryMaxTicks) PracticeFinishAttempt(practicePending);
		}
		else if (since >= kPostTicks)
		{
			PracticeFinishAttempt(practicePending);
		}
	}
}

void RLCoach::PracticeFinishAttempt(const std::string& reason)
{
	practiceResultLines.clear();
	bool counted = reason != "reset" && practiceLive.size() > 10;

	if (counted)
	{
		// Grade the attempt as a full rep: the original rep up to the start point, then what you just did
		std::vector<Frame> full(practiceRep.begin(), practiceRep.begin() + std::min(practiceStartTick, static_cast<int>(practiceRep.size())));
		full.insert(full.end(), practiceLive.begin(), practiceLive.end());
		int eventTick = practiceEventTick >= 0 ? practiceEventTick : static_cast<int>(full.size()) - 1;

		ShotAnalysis shot = ShotGeometry::Analyze(full);
		RepSegments segs = Segmenter::Split(full, eventTick);
		RepGrade g = Grader::GradeRep(full, segs, shot, practiceWentIn, practiceFloorFirst, shotBook.Get(practiceItem.shotKey));

		// The grade for the segment you're practicing (the same kind, closest in time)
		const SegmentGrade* target = nullptr;
		int bestGap = 1 << 30;
		for (const SegmentGrade& sg : g.segs)
		{
			const Segment& s = segs.segs[sg.segIndex];
			if (s.kind != practiceKind) continue;
			int gap = std::abs(s.start - practiceSegStart);
			if (gap < bestGap) { bestGap = gap; target = &sg; }
		}

		bool segOk = target && (target->grade == Grade::Clean || target->grade == Grade::Solid || target->grade == Grade::Calculated);
		bool hit = segOk && (practiceWentIn || practiceKind == SegKind::Recovery);

		practiceAttempts++;
		int pct = static_cast<int>(std::lround(practiceSpeed * 100.0f));
		if (hit) { practiceHitsByPct[pct]++; practiceHitsTotal++; practiceStreak++; }
		else practiceStreak = 0;

		// Result lines
		PanelGradeLine head;
		head.chunks.push_back({ hit ? "HIT  " : "MISS  ", static_cast<unsigned char>(hit ? 115 : 255), static_cast<unsigned char>(hit ? 230 : 115), static_cast<unsigned char>(hit ? 115 : 102) });
		std::string segName = Segmenter::Name(practiceKind);
		if (target)
		{
			unsigned char r, gg, b;
			GradeRgbP(target->grade, r, gg, b);
			head.chunks.push_back({ segName + " ", 190, 190, 190 });
			head.chunks.push_back({ Grader::Name(target->grade), r, gg, b });
		}
		else
		{
			head.chunks.push_back({ std::format("No {} this time", segName), 255, 115, 102 });
		}
		head.chunks.push_back({ practiceWentIn ? "   Goal" : "   No goal", static_cast<unsigned char>(practiceWentIn ? 115 : 255), static_cast<unsigned char>(practiceWentIn ? 230 : 115), static_cast<unsigned char>(practiceWentIn ? 115 : 102) });
		practiceResultLines.push_back(head);
		if (target && !target->why.empty()) practiceResultLines.push_back({ { { target->why, 200, 200, 200 } } });

		// Auto-ramp: 3 hits in a row moves you up 10%
		if (practiceAutoRamp && practiceStreak >= 3 && practiceSpeed < 0.99f)
		{
			PracticeSetSpeed(std::min(1.0f, std::round((practiceSpeed + 0.10f) * 20.0f) / 20.0f));
			practiceResultLines.push_back({ { { std::format("3 in a row: speeding up to {}%", static_cast<int>(practiceSpeed * 100 + 0.5f)), 120, 200, 255 } } });
		}
	}
	else
	{
		practiceResultLines.push_back({ { { "Reset: not counted", 160, 160, 160 } } });
	}

	practiceLive.clear();
	practiceState = PracticeState::Frozen;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (!car.IsNull()) PracticeReleaseFlip(car);
}

void RLCoach::PracticeRetry()
{
	if (practiceState == PracticeState::Off) return;
	practiceLive.clear();
	practiceResultLines.clear();
	practiceState = PracticeState::Frozen;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (!car.IsNull()) PracticeReleaseFlip(car);
}

void RLCoach::PracticeSetSpeed(float speed)
{
	practiceSpeed = speed;
	practiceStreak = 0;
	uiPracticeSpeed = speed;
}

void RLCoach::PracticeExit()
{
	if (practiceState == PracticeState::Off) return;
	practiceState = PracticeState::Off;
	uiPracticeActive = false;
	uiPracticeTarget = -1;
	practiceLive.clear();
	practiceRefFrames.clear();

	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (!server.IsNull()) server.SetGameSpeed(practiceSavedGameSpeed);
	CarWrapper car = gameWrapper->GetLocalCar();
	if (!car.IsNull())
	{
		PracticeReleaseFlip(car);
		car.SetVelocity(Vector(0.0f, 0.0f, 0.0f));
		car.SetAngularVelocity(Vector(0.0f, 0.0f, 0.0f), false);
	}
	havePrev = false;
	// If a rep is open in the viewer it picks back up (paused); otherwise the recorder re-arms
	state = (*recordingEnabled && !viewerActive) ? RepState::WaitReset : RepState::Idle;
}

// Faint reference: your best rep on this shot, from the practice start on
void RLCoach::DrawPracticeOverlay(CanvasWrapper& canvas)
{
	if (practiceRefFrames.size() < 2) return;
	CameraWrapper cam = gameWrapper->GetCamera();
	if (cam.IsNull()) return;
	Vector camLoc = cam.GetLocation();
	Vector camFwd = RotatorToVector(cam.GetRotation());

	int from = std::min(practiceStartTick, static_cast<int>(practiceRefFrames.size()) - 1);
	auto draw = [&](auto getPos, unsigned char r, unsigned char g, unsigned char b) {
		Vector2F prev{};
		bool havePrevPt = false;
		for (int i = from; i < static_cast<int>(practiceRefFrames.size()); i += 3)
		{
			Vector2F s;
			bool ok = ToScreenP(canvas, camLoc, camFwd, getPos(practiceRefFrames[i]), s);
			if (ok && havePrevPt)
			{
				canvas.SetColor(r, g, b, 90);
				canvas.DrawLine(prev, s, 2.0f);
			}
			prev = s;
			havePrevPt = ok;
		}
	};
	draw([](const Frame& f) { return Vector(f.carX, f.carY, f.carZ); }, 120, 230, 255);
	draw([](const Frame& f) { return Vector(f.ballX, f.ballY, f.ballZ); }, 255, 190, 90);
}
