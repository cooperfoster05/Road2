#include "pch.h"
#include "RLCoach.h"
#include "Overlay3D.h"
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
	// After a reset, stay frozen this long before an input can start the attempt. Free play's own reset (car to a
	// kickoff spot, ball to the middle) can land a tick or two after the button; held here, the start is re-applied
	// over it instead of the attempt starting from wherever the game put you (Oct 22: reset pressed quickly put the
	// car on a kickoff spot with the ball flying off at the shot's speed).
	constexpr int kFrozenGraceTicks = 18;
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

	// Input match (Nov 1): how closely an attempt copied the line it was following, 0-100 (-1 when there's nothing to
	// compare). Each tick of the attempt is held against the line's inputs at the same moment, forgiving timing within
	// 6 ticks (0.05 s) either way: the best match in that window counts. Per tick: the stick (steer or yaw, and pitch)
	// half, jump a fifth, boost and air roll the rest, a button counting only when you or the line press it. The half
	// second before the line's first touch counts triple (that's where the shot is decided); after the touch nothing
	// counts. Tested: an exact copy 100%, 0.05 s late 100%, 0.1 s late 72%, 0.2 s late 31%, doing nothing 6%. It
	// measures copying, not skill: the engine plays with perfect inputs, so 100% isn't the bar.
	float InputMatch(const std::vector<Frame>& live, const std::vector<Frame>& ref, int startTick)
	{
		const int n = static_cast<int>(ref.size());
		if (live.size() < 10 || startTick < 0 || startTick >= n) return -1.0f;
		int contact = -1;
		for (int t = startTick; t < n; t++) if (ref[t].touch) { contact = t; break; }
		const int end = contact >= 0 ? std::min(n, contact + 1) : n;
		constexpr int kSlack = 6;
		auto stick = [](const Frame& f, float& x, float& y) {
			x = std::fabs(f.steer) >= std::fabs(f.yaw) ? f.steer : f.yaw; y = f.pitch;
			const float m = std::sqrt(x * x + y * y);
			if (m > 1.0f) { x /= m; y /= m; }
		};
		double sum = 0.0, wsum = 0.0;
		for (int i = 0; i < static_cast<int>(live.size()); i++)
		{
			const int t = startTick + i;
			if (t >= end) break;
			const Frame& me = live[i];
			float mx, my;
			stick(me, mx, my);
			float bestStick = 0.0f;
			bool jumpOk = false, boostOk = false, rollOk = false;
			for (int k = std::max(startTick, t - kSlack); k <= std::min(end - 1, t + kSlack); k++)
			{
				const Frame& r = ref[k];
				float rx, ry;
				stick(r, rx, ry);
				bestStick = std::max(bestStick, 1.0f - std::min(1.0f, std::hypot(mx - rx, my - ry) / 0.75f));
				jumpOk = jumpOk || me.jump == r.jump;
				boostOk = boostOk || me.boost == r.boost;
				rollOk = rollOk || (me.handbrake == r.handbrake && std::fabs(me.roll - r.roll) < 0.5f);
			}
			// A button only counts on ticks where you or the line press it (both off isn't a match worth points)
			const Frame& r0 = ref[t];
			const bool jumpOn = me.jump || r0.jump, boostOn = me.boost || r0.boost;
			const bool rollOn = me.handbrake || r0.handbrake || std::fabs(me.roll) > 0.3f || std::fabs(r0.roll) > 0.3f;
			const double parts = 0.5 + 0.2 * jumpOn + 0.15 * boostOn + 0.15 * rollOn;
			const double score = (0.5 * bestStick + 0.2 * (jumpOn && jumpOk) + 0.15 * (boostOn && boostOk) + 0.15 * (rollOn && rollOk)) / parts;
			const double w = (contact >= 0 && t >= contact - 60) ? 3.0 : 1.0;
			sum += w * score; wsum += w;
		}
		return wsum > 0 ? static_cast<float>(100.0 * sum / wsum) : -1.0f;
	}

	void InputMatchRgb(float pct, unsigned char& r, unsigned char& g, unsigned char& b)
	{
		if (pct >= 80) { r = 115; g = 230; b = 115; }
		else if (pct >= 60) { r = 255; g = 204; b = 77; }
		else { r = 255; g = 115; b = 102; }
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
	if (viewerShowing >= 0) ViewerShowLine(-1, true);   // practice always runs from your real rep
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
		PracticeGoalsOff();
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
	practiceRideUntil = -1;
	practiceRideByInput = false;
	practiceLabel = label;
	practiceShot = ShotGeometry::Analyze(practiceRep);
	practiceBallStartTick = practiceShot.startTick;
	PracticeSetLeadIn(practiceLeadIn);

	practiceAttempts = 0;
	practiceHitsByPct.clear();
	practiceHitsTotal = 0;
	practiceStreak = 0;
	practiceResultLines.clear();
	practiceLive.clear();
	practiceState = PracticeState::Frozen;
	practiceFrozenTicks = 0;
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
			// Back on a surface without jumping off it (you drove or fell off the ceiling or a wall): the flip has no
			// timer, the game keeps it until you use it (Oct 28, ceiling shots)
			if (practiceRep[j].onGround)
			{
				bool jumpedHere = false;
				// (the wheels leave a few ticks after the jump press, so look back a little)
				for (int k = std::max(1, j - 10); k <= std::min(practiceStartTick, j + 3); k++)
					if (practiceRep[k].jump && !practiceRep[k - 1].jump) jumpedHere = true;
				if (!jumpedHere) break;
			}
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
	practiceGhostLine = false;   // your own best rep: drawn faintly, no controller ghost
	if (!writer || practiceItem.shotKey.empty()) return;

	std::unordered_map<std::string, int> autos;
	gradeBook.Copy(autos);
	std::vector<RepListItem> same;
	for (const RepListItem& it : RepLibrary::ListReps(writer->Folder()))   // newest first
		if (it.shotKey == practiceItem.shotKey && it.endReason != "reset" && it.hasRecording && !it.imported) same.push_back(it);
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
		if (!RepLibrary::LoadRep(writer->Folder(), it.file, fr, err)) continue;
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
	PracticeApplyFrame(car, ball, practiceRep[std::min(practiceStartTick, static_cast<int>(practiceRep.size()) - 1)], frozen, true);
}

void RLCoach::PracticeApplyFrame(CarWrapper& car, BallWrapper& ball, const Frame& f, bool frozen, bool holdFlip)
{
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
	if (holdFlip && !frozen && airborne && f.hasFlip)
	{
		if (!practiceFlipHeld) { practiceOrigMaxDodge = car.GetMaxTimeForDodge(); practiceFlipHeld = true; }
		car.SetMaxTimeForDodge(1000.0f);
	}

	ball.SetLocation(Vector(f.ballX, f.ballY, f.ballZ));
	ball.SetVelocity(Vector(f.ballVX * v, f.ballVY * v, f.ballVZ * v));
	ball.SetAngularVelocity(Vector(f.ballAVX * v, f.ballAVY * v, f.ballAVZ * v), false);
}

// A lesson's running start played as inputs (Lesson::StageRideByInput): the model's controller on your car until the
// step begins, so the game itself jumps and flips (a flip put back frame by frame can't be cancelled)
void RLCoach::OverrideRideInput(CarWrapper& car, void* params)
{
	if (!practiceRideByInput || practiceRideUntil < 0 || !learnActive || practiceState != PracticeState::Live || !params || car.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	if (local.IsNull() || local.memory_address != car.memory_address) return;
	const int rt = practiceStartTick + static_cast<int>(practiceLive.size());
	if (rt >= practiceRideUntil || rt >= static_cast<int>(practiceRep.size())) return;
	const Frame& f = practiceRep[rt];
	ControllerInput in{};
	in.Throttle = f.throttle; in.Steer = f.steer; in.Pitch = f.pitch; in.Yaw = f.yaw; in.Roll = f.roll;
	// the flip's direction (the game works these out from the stick before this point, so set them too)
	in.DodgeForward = -f.pitch;
	in.DodgeStrafe = std::fabs(f.yaw) > 0.01f ? f.yaw : f.steer;
	in.Handbrake = f.handbrake ? 1 : 0;
	in.Jump = f.jump ? 1 : 0;
	in.Jumped = in.Jump;
	in.ActivateBoost = f.boost ? 1 : 0;
	in.HoldingBoost = in.ActivateBoost;
	*reinterpret_cast<ControllerInput*>(params) = in;
}

// No unlimited boost (kickoff lessons: the model starts on 33 and picks up pads). Free play's setting goes back when
// practice ends.
void RLCoach::PracticeLimitBoost(CarWrapper& car)
{
	BoostWrapper bc = car.GetBoostComponent();
	if (bc.IsNull()) return;
	const int now = bc.GetUnlimitedBoostRefCount();
	if (now == 0) return;
	if (practiceSavedUnlimited < 0) practiceSavedUnlimited = now;
	bc.SetUnlimitedBoostRefCount(0);
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
		practiceFrozenTicks++;
		if (!AnyInput(in) || practiceFrozenTicks < kFrozenGraceTicks)
		{
			PracticeReleaseFlip(car);
			PracticeApplyStart(car, ball, true);
			return;
		}
		// First input: restore the full moment, with real speeds, and go
		PracticeApplyStart(car, ball, false);
		// Kickoff lessons: every boost pad back, as at a real kickoff (the model picks up the small pads on the way)
		if (learnActive && learnLesson.kind == LessonKind::Kickoff) server.ResetPickups();
		if (learnActive && (learnLesson.kind == LessonKind::Kickoff || learnLesson.kind == LessonKind::Move)) PracticeLimitBoost(car);
		practiceLive.clear();
		practicePending.clear();
		practiceEventTick = -1;
		practiceGoalPending = false;
		practiceWentIn = false;
		practiceFloorFirst = false;
		practiceTouchPending = false;
		practiceRecoverRun = 0;
		// Catch the end tracker up on the original rep before this moment (touches, jumps)
		practiceEndTracker.Reset();
		// (Nov 2) Only from the rep's last teleport: a lesson model can hold several runs back to back (a kickoff from
		// each side), and the earlier run's touch made the right-side kickoff end as a "miss" a second in.
		int from = 0;
		for (int i = 1; i < practiceStartTick && i < static_cast<int>(practiceRep.size()); i++)
		{
			const Frame& a = practiceRep[i - 1]; const Frame& b = practiceRep[i];
			if (std::hypot(b.carX - a.carX, b.carY - a.carY) > 500.0f) from = i;
		}
		for (int i = from; i < practiceStartTick && i < static_cast<int>(practiceRep.size()); i++)
		{
			int ignored = 0;
			practiceEndTracker.Update(practiceRep[i], i > from, i > from ? practiceRep[i - 1].ballZ : practiceRep[i].ballZ, i - from + 1, ignored);
		}
		// Watch for a reset from the very first live tick: compare against where the attempt starts (before, the
		// first live tick was taken as the reference, so a reset landing on it went unnoticed)
		{
			const Frame& s0 = practiceRep[std::min(practiceStartTick, static_cast<int>(practiceRep.size()) - 1)];
			practicePrevCar = Vector(s0.carX, s0.carY, s0.carZ);
			practicePrevBall = Vector(s0.ballX, s0.ballY, s0.ballZ);
			practiceHavePrev = true;
		}
		practiceState = PracticeState::Live;
		repStartTime = std::chrono::steady_clock::now();
		if (learnActive) LearnBeginLive();
		return;
	}

	// ---- Live attempt ----
	// Kickoff lessons: free play turns unlimited boost back on after its own resets (a goal, the reset button), so
	// keep it off for the whole attempt (Coop, Nov 2: it came back on partway through his reps)
	// (and the movement lessons: a wavedash is free speed, a speedflip starts on the model's boost)
	if (learnActive && (learnLesson.kind == LessonKind::Kickoff || learnLesson.kind == LessonKind::Move)) PracticeLimitBoost(car);
	if (learnActive && practiceRideUntil >= 0 && !practiceRideByInput)
	{
		// The running start: the model drives (its frame put back every tick) until the step begins
		// (played as inputs instead, OverrideRideInput, the car is recorded like any live tick below)
		const int rt = practiceStartTick + static_cast<int>(practiceLive.size());
		if (rt < practiceRideUntil && rt < static_cast<int>(practiceRep.size()))
		{
			const Frame& m = practiceRep[rt];
			PracticeApplyFrame(car, ball, m, false, false);
			practiceLive.push_back(m);
			learnFull.push_back(m);
			practicePrevCar = Vector(m.carX, m.carY, m.carZ);
			practicePrevBall = Vector(m.ballX, m.ballY, m.ballZ);
			practiceHavePrev = true;
			return;
		}
	}
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
	// A lesson stage that ends before the finish (the pop, the first touch, the carry) is over when it's over
	if (learnActive && LearnStageDone(f)) { practiceEventTick = tick; PracticeFinishAttempt("step"); return; }

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
		// Kickoff lessons end on their own (a second after the hit); the shot tracker would call the ball sitting at
		// centre, or knocked sideways, a missed shot
		const bool ownEnd = learnActive && learnLesson.kind == LessonKind::Kickoff;
		std::string ended = ownEnd ? std::string() : practiceEndTracker.Update(f, hadPrev, prevBallZ, liveTicks, practiceEventTick);
		if (!ended.empty()) practicePending = ended;
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
	// Following a line (an engine line, or a lesson's model): how closely you copied its inputs, shown on the panel
	practiceInputMatch = -1.0f;
	if (practiceGhostLine && reason != "reset")
	{
		// (the running start is the model's own inputs: only what you did after it counts)
		const int skip = (learnActive && practiceRideUntil > practiceStartTick) ? std::min(static_cast<int>(practiceLive.size()), practiceRideUntil - practiceStartTick) : 0;
		if (skip == 0) practiceInputMatch = InputMatch(practiceLive, practiceRefFrames, practiceStartTick);
		else if (static_cast<int>(practiceLive.size()) > skip)
			practiceInputMatch = InputMatch(std::vector<Frame>(practiceLive.begin() + skip, practiceLive.end()), practiceRefFrames, practiceStartTick + skip);
	}
	auto addMatchLine = [this]() {
		if (practiceInputMatch < 0) return;
		PanelGradeLine line;
		unsigned char r, g, b;
		InputMatchRgb(practiceInputMatch, r, g, b);
		line.chunks.push_back({ "Input match  ", 190, 190, 190 });
		line.chunks.push_back({ std::format("{}%", static_cast<int>(std::lround(practiceInputMatch))), r, g, b });
		practiceResultLines.push_back(line);
	};
	if (learnActive)
	{
		// A lesson scores its own stages (and may switch stage, e.g. after the warm-up)
		const bool wasWarmup = learnWarmup;
		LearnOnAttempt(reason);
		if (!practiceResultLines.empty()) addMatchLine();
		if (wasWarmup) return;   // LearnTry already set up the next attempt
		practiceLive.clear();
		practiceState = PracticeState::Frozen;
		practiceFrozenTicks = 0;
		CarWrapper car = gameWrapper->GetLocalCar();
		if (!car.IsNull()) PracticeReleaseFlip(car);
		return;
	}
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
		addMatchLine();

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
	practiceFrozenTicks = 0;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (!car.IsNull()) PracticeReleaseFlip(car);
}

void RLCoach::PracticeRetry()
{
	if (practiceState == PracticeState::Off) return;
	practiceLive.clear();
	practiceResultLines.clear();
	practiceState = PracticeState::Frozen;
	practiceFrozenTicks = 0;
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
	practiceGhostLine = false;

	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (!server.IsNull()) server.SetGameSpeed(practiceSavedGameSpeed);
	PracticeGoalsRestore();
	CarWrapper car = gameWrapper->GetLocalCar();
	if (!car.IsNull())
	{
		PracticeReleaseFlip(car);
		car.SetVelocity(Vector(0.0f, 0.0f, 0.0f));
		car.SetAngularVelocity(Vector(0.0f, 0.0f, 0.0f), false);
		if (practiceSavedUnlimited >= 0)
		{
			BoostWrapper bc = car.GetBoostComponent();
			if (!bc.IsNull()) bc.SetUnlimitedBoostRefCount(practiceSavedUnlimited);
		}
	}
	practiceSavedUnlimited = -1;
	practiceRideUntil = -1;
	practiceRideByInput = false;
	havePrev = false;
	// If a rep is open in the viewer it picks back up (paused); otherwise the recorder re-arms
	state = (*recordingEnabled && !viewerActive && !learnActive) ? RepState::WaitReset : RepState::Idle;
}

// Faint reference: your best rep on this shot, from the practice start on
void RLCoach::DrawPracticeOverlay(CanvasWrapper& canvas)
{
	CameraWrapper cam = gameWrapper->GetCamera();
	if (cam.IsNull()) return;
	Vector camLoc = cam.GetLocation();
	Vector camFwd = RotatorToVector(cam.GetRotation());

	// Yellow ring on the ball: where to hit it. Rides with the ball until you touch it this attempt.
	if (practiceShot.valid && practiceTargetTick <= practiceShot.touchTick && (!learnActive || learnLesson.StageHasFinish(learnStage)))
	{
		bool touched = false;
		for (const Frame& f : practiceLive) if (f.touch) { touched = true; break; }
		ServerWrapper server = gameWrapper->GetGameEventAsServer();
		BallWrapper ball = server.IsNull() ? BallWrapper(0) : server.GetBall();
		if (!touched && !ball.IsNull())
		{
			const ShotAnalysis& s = practiceShot;
			Vector n(s.idealContact.x - s.ballCenter.x, s.idealContact.y - s.ballCenter.y, s.idealContact.z - s.ballCenter.z);
			Overlay3D::BallRing(canvas, camLoc, camFwd, ball.GetLocation(), n, 255, 220, 0);
			int tick = practiceStartTick + static_cast<int>(practiceLive.size());
			DrawTargetHitbox(canvas, camLoc, camFwd, s, practiceRep, tick, ball.GetLocation(), false);
		}
	}

	if (practiceRefFrames.size() < 2) return;
	int from = std::min(practiceStartTick, static_cast<int>(practiceRefFrames.size()) - 1);
	auto draw = [&](auto getPos, unsigned char r, unsigned char g, unsigned char b) {
		Vector2F prev{};
		bool havePrevPt = false;
		for (int i = from; i < static_cast<int>(practiceRefFrames.size()); i += 3)
		{
			// a jump back to a spawn ends this line (kickoff lessons hold the left and the right kickoff back to back)
			if (i >= from + 3)
			{
				const Frame& a = practiceRefFrames[i - 3], & b = practiceRefFrames[i];
				if (std::fabs(a.carX - b.carX) + std::fabs(a.carY - b.carY) > 800.0f) break;
			}
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

void RLCoach::PracticeGoalsOff()
{
	if (practiceGoalCvarSaved >= 0) return;
	CVarWrapper cv = cvarManager->getCvar("sv_soccar_enablegoal");
	if (cv.IsNull()) return;
	practiceGoalCvarSaved = cv.getBoolValue() ? 1 : 0;
	cv.setValue(0);
}

void RLCoach::PracticeGoalsRestore()
{
	if (practiceGoalCvarSaved < 0) return;
	CVarWrapper cv = cvarManager->getCvar("sv_soccar_enablegoal");
	if (!cv.IsNull()) cv.setValue(practiceGoalCvarSaved);
	practiceGoalCvarSaved = -1;
}
