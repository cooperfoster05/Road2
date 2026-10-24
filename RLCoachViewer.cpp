#include "pch.h"
#include "RLCoach.h"
#include "Ratings.h"
#include "bakkesmod/wrappers/includes.h"
#include "bakkesmod/wrappers/GuiManagerWrapper.h"
#include "Overlay3D.h"
#include "UiStyle.h"
#include "Engine.h"
#include "UiFonts.h"
#include "Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <format>
#include <functional>
#ifndef NOMINMAX
#define NOMINMAX       // keep Windows.h from turning std::min / std::max into macros
#endif
#include <Windows.h>   // SetThreadPriority: the what-if search runs below normal priority

// ============================================================
// Rep viewer: plays a saved rep back in free play by moving the
// car and ball through their recorded positions every tick.
// ============================================================

namespace
{
	constexpr float kGoalLineY = 5130.0f;   // keep the replayed ball just short of scoring, so free play doesn't explode it
	constexpr float kGoalHalfWidth = 900.0f;
	constexpr float kGoalHeight = 650.0f;

	float Lerp(float a, float b, float t) { return a + (b - a) * t; }

	// Smooth curve between two ticks using each tick's position and velocity (cubic Hermite).
	// Straight-line blending looks kinked in slow motion; this follows the real arc.
	// v0, v1 are in units per second; frames are 1/120 s apart.
	void Hermite(float p0, float v0, float p1, float v1, float t, float& pos, float& velPerSec)
	{
		const float m0 = v0 / 120.0f, m1 = v1 / 120.0f;
		const float t2 = t * t, t3 = t2 * t;
		pos = (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
		float dPerTick = (6 * t2 - 6 * t) * p0 + (3 * t2 - 4 * t + 1) * m0 + (-6 * t2 + 6 * t) * p1 + (3 * t2 - 2 * t) * m1;
		velPerSec = dPerTick * 120.0f;
	}

	// Rotations wrap at 65536 units, so blend along the shorter way around
	int LerpAngle(int a, int b, float t)
	{
		int d = b - a;
		while (d > 32768) d -= 65536;
		while (d < -32768) d += 65536;
		return a + static_cast<int>(std::lround(d * t));
	}

	bool ToScreen(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd, const Vector& p, Vector2F& out)
	{
		Vector rel = p - camLoc;
		if (Vector::dot(rel, camFwd) < 10.0f) return false; // behind the camera
		Vector2 s = canvas.Project(p);
		out = Vector2F{ static_cast<float>(s.X), static_cast<float>(s.Y) };
		return true;
	}
}

// The review header: the mechanic's name, and three numbers about your shot
static std::string ReviewMechanic(int shotType)
{
	switch (shotType)
	{
	case 1: return "GROUND SHOT";
	case 2: return "DOUBLE TAP";
	case 3: return "DRIBBLE";
	case 4: return "AIR DRIBBLE";
	case 5: return "FLIP RESET";
	default: return "AERIAL";
	}
}

static std::vector<ReviewStat> ReviewStats(const ShotAnalysis& s, bool wentIn, const ShotReference& ref)
{
	std::vector<ReviewStat> out;
	if (!s.valid) return out;
	auto num = [](float v) {
		std::string t = std::format("{:.0f}", std::fabs(v));
		for (int i = static_cast<int>(t.size()) - 3; i > 0; i -= 3) t.insert(static_cast<size_t>(i), ",");
		return (v < 0 ? "-" : "") + t;
	};
	if (wentIn) out.push_back({ "Height", num(s.crossZ), "at the goal line" });
	else if (s.reachesGoalLine) out.push_back({ "Missed by", num(s.missDist), "outside the net" });
	else out.push_back({ "Missed", "short", "never reached the line" });
	out.push_back({ "Shot speed", num(Units::ConvertSpeed(s.shotSpeed)), Units::SpeedLabel() });
	if (s.type == ShotType::DoubleTap)
		out.push_back({ "Off the wall", num(s.reboundDist), "300+ for a 5" });
	else if (s.type != ShotType::Ground && ref.valid)
	{
		const float vs = (s.touchTick - s.startTick) / 120.0f - ref.touchSec;
		out.push_back({ "Touch", std::format("{}{:.2f} s", vs >= 0 ? "+" : "-", std::fabs(vs)), "vs your fastest goal" });
	}
	else
		out.push_back({ "Aim", std::format("{:.1f} deg", s.aimErrDeg), "off line" });
	return out;
}

// Older reps (before the index stored it) have no event tick; the segmenter then uses the last frame
static int ViewerEventTick(const RepListItem& item) { return item.eventTick; }

void RLCoach::ViewerLoad(const RepListItem& item)
{
	if (!writer) return;
	PracticeExit();   // opening a rep ends any practice set
	CancelWhatIf();   // and forgets the last rep's searches
	viewerShowing = -1;
	uiShowing = -1;
	viewerRealFrames.clear();
	std::vector<Frame> loaded;
	std::string err;
	if (!RepLibrary::LoadRep(writer->Folder(), item.file, loaded, err))
	{
		viewerStatus = "Couldn't load rep: " + err;
		return;
	}

	// Pause the recorder so the viewer's teleports aren't saved as reps
	frames.clear();
	pendingEndReason.clear();
	state = RepState::Idle;
	havePrev = false;

	// Remember the game speed so it can be put back when you stop watching
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (!viewerActive) savedGameSpeed = server.IsNull() ? 1.0f : server.GetGameSpeed();

	viewerFrames = std::move(loaded);
	viewerRep = item;

	viewerShot = ShotGeometry::Analyze(viewerFrames);
	viewerSegs = Segmenter::Split(viewerFrames, ViewerEventTick(item));
	uiWhatIfStart = WhatIf::StartTick(viewerFrames);
	{
		SegKind keySeg;
		std::string keyName;
		bool hasKey = WhatIf::KeyMoment(viewerFrames, keySeg, keyName);
		uiWhatIfKeyKind = hasKey ? static_cast<int>(keySeg) : -1;
		std::lock_guard<std::mutex> lock(uiSegMutex);
		uiWhatIfKeyWhat = keyName;
	}

	// Grades: timing compares against your fastest goal on the same shot
	ShotReference viewerRef;
	shotBook.TryGet(item.shotKey, viewerRef);   // never waits; if the shot book is still loading, timing isn't compared
	viewerGrade = Grader::GradeRep(viewerFrames, viewerSegs, viewerShot, item.goal, item.floorBeforeGoal, viewerRef);
	gradeBook.TrySet(item.file, GradeBook::Make(viewerGrade, viewerShot, viewerSegs, item.shotKey, viewerRef.bestFile, item.endReason == "reset"));
	{
		std::lock_guard<std::mutex> lock(uiSegMutex);
		uiViewerFile = item.file;
		uiSegs = viewerSegs.segs;
		uiSegTotal = static_cast<int>(viewerFrames.size());
		uiGrades.clear();
		for (int gi = 0; gi < static_cast<int>(viewerGrade.segs.size()); gi++)
		{
			const SegmentGrade& gs = viewerGrade.segs[gi];
			uiGrades.push_back(UiGrade{ gs.segIndex, Grader::Name(gs.grade), gs.why, gi == viewerGrade.biggestFix });
		}
		uiOverall = viewerGrade.overall > 0
			? std::format("{}: {}  ({})", viewerGrade.overall, Grader::OverallName(viewerGrade.overall), viewerGrade.overallWhy)
			: std::string();
		uiOverallGrade = viewerGrade.overall;
		uiOverallName = viewerGrade.overall > 0 ? Grader::OverallName(viewerGrade.overall) : "";
		uiOverallWhy = viewerGrade.overallWhy;
		uiMechanic = viewerShot.valid ? ReviewMechanic(static_cast<int>(viewerShot.type)) : "REP";
		uiStats = ReviewStats(viewerShot, item.goal, viewerRef);
	}
	// v2: the coach's root cause (and one more note) for this rep
	{
		std::vector<CoachNote> notes = coachBook.Notes(viewerFrames, viewerShot, viewerGrade.overall, habitBook);
		std::lock_guard<std::mutex> lock(uiSegMutex);
		uiNotes = std::move(notes);
	}

	// Predicted ball path from the moment the ball starts moving, for comparing against what really happened
	viewerPred.clear();
	if (predictor.Ready())
	{
		for (const Frame& f : viewerFrames)
		{
			if (std::fabs(f.ballVX) + std::fabs(f.ballVY) + std::fabs(f.ballVZ) > 1.0f)
			{
				int ticks = static_cast<int>(viewerFrames.size()) - f.tick;
				predictor.Predict(BallSample{ f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ },
					f.ballAVX, f.ballAVY, f.ballAVZ, ticks, viewerPred);
				break;
			}
		}
	}
	viewerPlayhead = 0.0f;
	viewerPlaying = true;
	viewerActive = true;
	viewerStatus.clear();

	uiViewerActive = true;
	uiViewerPlaying = true;
	uiViewerTotal = static_cast<int>(viewerFrames.size());
	uiViewerPlayhead = 0.0f;

	// The review's two searches start on their own (or come from the cache); if the simulator is still loading,
	// the viewer window starts them once it's ready
	uiReviewPending = true;
	StartReview();
}

void RLCoach::ViewerExit()
{
	PracticeExit();
	if (!viewerActive) return;
	CancelWhatIf();
	viewerShowing = -1;
	uiShowing = -1;
	uiReviewPending = false;
	viewerRealFrames.clear();
	uiWhatIfStart = -1;
	uiWhatIfKeyKind = -1;
	viewerActive = false;
	viewerPlaying = false;
	viewerFrames.clear();
	viewerPred.clear();
	viewerSegs = RepSegments{};
	viewerGrade = RepGrade{};
	{
		std::lock_guard<std::mutex> lock(uiSegMutex);
		uiSegs.clear();
		uiSegTotal = 0;
		uiGrades.clear();
		uiOverall.clear();
		uiNotes.clear();
	}
	uiViewerActive = false;
	uiViewerPlaying = false;

	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (!server.IsNull()) server.SetGameSpeed(savedGameSpeed);

	CarWrapper car = gameWrapper->GetLocalCar();
	if (!car.IsNull())
	{
		car.SetVelocity(Vector(0.0f, 0.0f, 0.0f));
		car.SetAngularVelocity(Vector(0.0f, 0.0f, 0.0f), false);
	}

	// Recorder picks back up after the next ball reset
	havePrev = false;
	state = *recordingEnabled ? RepState::WaitReset : RepState::Idle;
}

void RLCoach::ViewerSeek(float tick)
{
	if (!viewerActive || viewerFrames.empty()) return;
	float last = static_cast<float>(viewerFrames.size() - 1);
	viewerPlayhead = std::fmax(0.0f, std::fmin(tick, last));
	uiViewerPlayhead = viewerPlayhead;
}

void RLCoach::ViewerTick(CarWrapper& car)
{
	if (viewerFrames.empty()) return;
	if (!gameWrapper->IsInFreeplay())
	{
		viewerStatus = "Open free play to watch reps";
		return;
	}
	viewerStatus.clear();

	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull()) return;
	BallWrapper ball = server.GetBall();
	if (ball.IsNull()) return;

	// Slow motion uses the game's own speed setting, so the game smooths motion between ticks itself.
	// The replay always advances exactly one recorded tick per game tick.
	if (std::fabs(server.GetGameSpeed() - viewerSpeed) > 0.001f) server.SetGameSpeed(viewerSpeed);

	const float last = static_cast<float>(viewerFrames.size() - 1);
	if (viewerPlaying)
	{
		viewerPlayhead += 1.0f;
		if (viewerPlayhead > last)
		{
			if (viewerLoop) viewerPlayhead = 0.0f;
			else { viewerPlayhead = last; viewerPlaying = false; }
		}
	}

	int i0 = static_cast<int>(viewerPlayhead);
	int i1 = std::min(i0 + 1, static_cast<int>(last));
	float a = viewerPlayhead - static_cast<float>(i0);
	const Frame& f0 = viewerFrames[i0];
	const Frame& f1 = viewerFrames[i1];

	// While playing, give things their recorded velocity so the game's own motion between ticks lines up
	// with where we put them next. While paused, hold them still.
	float vScale = viewerPlaying ? 1.0f : 0.0f;

	float cx, cy, cz, cvx, cvy, cvz;
	Hermite(f0.carX, f0.carVX, f1.carX, f1.carVX, a, cx, cvx);
	Hermite(f0.carY, f0.carVY, f1.carY, f1.carVY, a, cy, cvy);
	Hermite(f0.carZ, f0.carVZ, f1.carZ, f1.carVZ, a, cz, cvz);
	car.SetLocation(Vector(cx, cy, cz));
	car.SetRotation(Rotator(LerpAngle(f0.carPitch, f1.carPitch, a), LerpAngle(f0.carYaw, f1.carYaw, a), LerpAngle(f0.carRoll, f1.carRoll, a)));
	car.SetVelocity(Vector(cvx * vScale, cvy * vScale, cvz * vScale));
	car.SetAngularVelocity(Vector(Lerp(f0.carAVX, f1.carAVX, a) * vScale, Lerp(f0.carAVY, f1.carAVY, a) * vScale,
		Lerp(f0.carAVZ, f1.carAVZ, a) * vScale), false);
	BoostWrapper boostComp = car.GetBoostComponent();
	if (!boostComp.IsNull()) boostComp.SetCurrentBoostAmount(std::fmax(f0.boostAmount, f0.boost ? 1.0f : 0.0f) / 100.0f);

	float bx, by, bz, bvx, bvy, bvz;
	Hermite(f0.ballX, f0.ballVX, f1.ballX, f1.ballVX, a, bx, bvx);
	Hermite(f0.ballY, f0.ballVY, f1.ballY, f1.ballVY, a, by, bvy);
	Hermite(f0.ballZ, f0.ballVZ, f1.ballZ, f1.ballVZ, a, bz, bvz);
	// A touch changes the ball's velocity within one tick, so the curve can overshoot there: use a straight line instead
	if (f1.touch)
	{
		bx = Lerp(f0.ballX, f1.ballX, a); by = Lerp(f0.ballY, f1.ballY, a); bz = Lerp(f0.ballZ, f1.ballZ, a);
		bvx = f1.ballVX; bvy = f1.ballVY; bvz = f1.ballVZ;
	}
	bool inGoalMouth = std::fabs(bx) < kGoalHalfWidth && bz < kGoalHeight;
	bool guarded = inGoalMouth && std::fabs(by) > kGoalLineY;
	if (guarded) by = std::copysign(kGoalLineY, by);
	ball.SetLocation(Vector(bx, by, bz));
	if (guarded)
		ball.SetVelocity(Vector(0.0f, 0.0f, 0.0f));
	else
		ball.SetVelocity(Vector(bvx * vScale, bvy * vScale, bvz * vScale));
	ball.SetAngularVelocity(Vector(Lerp(f0.ballAVX, f1.ballAVX, a) * vScale, Lerp(f0.ballAVY, f1.ballAVY, a) * vScale,
		Lerp(f0.ballAVZ, f1.ballAVZ, a) * vScale), false);

	uiViewerPlayhead = viewerPlayhead;
	uiViewerPlaying = viewerPlaying;
}

void RLCoach::OverrideViewerInput(CarWrapper& car, void* params)
{
	if (practiceState != PracticeState::Off) return;   // practice: your controller drives
	if (!viewerActive || !params || viewerFrames.empty() || car.IsNull()) return;
	CarWrapper local = gameWrapper->GetLocalCar();
	if (local.IsNull() || local.memory_address != car.memory_address) return;

	// Replace your controller input with the recorded boost only: the boost flame shows when you
	// boosted, and stray jumps or stick movement can't twitch the car while you watch.
	int i = std::min(static_cast<int>(viewerPlayhead), static_cast<int>(viewerFrames.size()) - 1);
	ControllerInput* in = reinterpret_cast<ControllerInput*>(params);
	ControllerInput replay{};
	replay.ActivateBoost = viewerFrames[i].boost ? 1 : 0;
	replay.HoldingBoost = replay.ActivateBoost;
	*in = replay;
}

void RLCoach::DrawViewerPaths(CanvasWrapper& canvas)
{
	if (viewerFrames.size() < 2) return;
	CameraWrapper cam = gameWrapper->GetCamera();
	if (cam.IsNull()) return;
	Vector camLoc = cam.GetLocation();
	Vector camFwd = RotatorToVector(cam.GetRotation());

	const int n = static_cast<int>(viewerFrames.size());
	const int head = static_cast<int>(viewerPlayhead);
	const int step = 2;

	auto drawPath = [&](auto getPos, unsigned char r, unsigned char g, unsigned char b) {
		Vector2F prev{};
		bool havePrevPt = false;
		for (int i = 0; i < n; i += step)
		{
			Vector2F s;
			bool ok = ToScreen(canvas, camLoc, camFwd, getPos(viewerFrames[i]), s);
			if (ok && havePrevPt)
			{
				// Already-played part is bright, the rest is faint
				unsigned char alpha = i <= head ? 230 : 70;
				canvas.SetColor(r, g, b, alpha);
				canvas.DrawLine(prev, s, 2.0f);
			}
			prev = s;
			havePrevPt = ok;
		}
	};

	if (!viewerPred.empty()) DrawPredictedPath(canvas, viewerPred);
	drawPath([](const Frame& f) { return Vector(f.ballX, f.ballY, f.ballZ); }, 255, 160, 40);
	drawPath([](const Frame& f) { return Vector(f.carX, f.carY, f.carZ); }, 90, 190, 255);
	DrawWhatIfPath(canvas);

	// Shot analysis overlays
	if (viewerShot.valid)
	{
		const ShotAnalysis& s = viewerShot;
		Vector c(s.ballCenter.x, s.ballCenter.y, s.ballCenter.z);
		auto line = [&](const Vector& a, const Vector& b, unsigned char r, unsigned char g, unsigned char bl) {
			Vector2F sa, sb;
			if (!ToScreen(canvas, camLoc, camFwd, a, sa) || !ToScreen(canvas, camLoc, camFwd, b, sb)) return;
			Overlay3D::OutlinedLine(canvas, sa, sb, 2.5f, r, g, bl, 220);
		};

		// Yellow: the direction the ball needed to go. Green: where your shot actually went.
		line(c, Vector(c.X + s.idealDir.x * 900, c.Y + s.idealDir.y * 900, c.Z + s.idealDir.z * 900), 255, 220, 0);
		line(c, Vector(c.X + s.shotDir.x * 900, c.Y + s.shotDir.y * 900, c.Z + s.shotDir.z * 900), 80, 255, 120);

		// Rings on the ball: yellow = where you needed to hit it, green = where you did.
		// Before the touch they ride along with the ball, so at contact they sit right where the car meets it.
		Vector nIdeal(s.idealContact.x - s.ballCenter.x, s.idealContact.y - s.ballCenter.y, s.idealContact.z - s.ballCenter.z);
		Vector nActual(s.actualContact.x - s.ballCenter.x, s.actualContact.y - s.ballCenter.y, s.actualContact.z - s.ballCenter.z);
		Vector now;
		{
			ServerWrapper server = gameWrapper->GetGameEventAsServer();
			BallWrapper ball = server.IsNull() ? BallWrapper(0) : server.GetBall();
			if (!ball.IsNull()) now = ball.GetLocation();
			else { const Frame& f = viewerFrames[std::min(head, n - 1)]; now = Vector(f.ballX, f.ballY, f.ballZ); }
		}
		DrawTargetHitbox(canvas, camLoc, camFwd, s, viewerFrames, head, now, true);
		if (head < s.touchTick)
		{
			Overlay3D::BallRing(canvas, camLoc, camFwd, now, nIdeal, 255, 220, 0);
			Overlay3D::BallRing(canvas, camLoc, camFwd, now, nActual, 80, 255, 120);
		}
		else
		{
			// After the touch: a faint ghost ball marks where the hit happened, with both rings on it
			float fade = std::max(0.55f, 1.0f - (head - s.touchTick) / 90.0f);
			Overlay3D::BallGhost(canvas, camLoc, camFwd, c, 255, 255, 255, static_cast<unsigned char>(70 * fade));
			Overlay3D::BallRing(canvas, camLoc, camFwd, c, nIdeal, 255, 220, 0, fade);
			Overlay3D::BallRing(canvas, camLoc, camFwd, c, nActual, 80, 255, 120, fade);
		}

		// Faint ghost ball: the earliest a fast, direct aerial could have met it
		if (s.earliestTick >= 0 && s.earliestTick < n)
		{
			const Frame& e = viewerFrames[s.earliestTick];
			Overlay3D::BallGhost(canvas, camLoc, camFwd, Vector(e.ballX, e.ballY, e.ballZ), 255, 220, 0, 150);
		}
	}

	// Other touches (not the shot): small green dots
	for (int i = 0; i < n; i++)
	{
		const Frame& f = viewerFrames[i];
		if (!f.touch || (viewerShot.valid && std::abs(i - viewerShot.touchTick) <= 12)) continue;
		Vector2F s;
		if (!ToScreen(canvas, camLoc, camFwd, Vector(f.touchX, f.touchY, f.touchZ), s)) continue;
		canvas.SetColor(0, 0, 0, 120);
		canvas.SetPosition(Vector2F{ s.X - 4.0f, s.Y - 4.0f });
		canvas.FillBox(Vector2F{ 8.0f, 8.0f });
		canvas.SetColor(80, 255, 120, 255);
		canvas.SetPosition(Vector2F{ s.X - 3.0f, s.Y - 3.0f });
		canvas.FillBox(Vector2F{ 6.0f, 6.0f });
	}
}

// Target car: your hitbox, placed and aimed to make the ideal hit (yellow). Rides along with the ball
// until the moment it's for, then stays put. After your touch, your real hitbox at contact shows in green.
void RLCoach::DrawTargetHitbox(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd, const ShotAnalysis& s,
	const std::vector<Frame>& fr, int tick, const Vector& ballNow, bool showActual)
{
	int mode = *targetHitbox;
	if (mode == 0 || !s.valid || fr.empty()) return;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (car.IsNull()) return;
	Vector half = car.GetLocalCollisionExtent();
	Vector off = car.GetLocalCollisionOffset();
	if (half.X > 90.0f) half = half * 0.5f;   // some builds report the full size
	if (half.X < 20.0f || half.Z < 5.0f) return;

	bool earliest = mode == 2 && s.earliestValid && s.earliestTick >= 0;
	Vector contact = earliest ? Vector(s.earliestContact.x, s.earliestContact.y, s.earliestContact.z)
		: Vector(s.idealContact.x, s.idealContact.y, s.idealContact.z);
	Vector ball = earliest ? Vector(s.earliestBall.x, s.earliestBall.y, s.earliestBall.z)
		: Vector(s.ballCenter.x, s.ballCenter.y, s.ballCenter.z);
	int anchor = earliest ? s.earliestTick : s.touchTick;

	Vector fwd = ball - contact;
	if (fwd.magnitude() < 1.0f) return;
	fwd.normalize();
	const Frame& tf = fr[std::max(0, std::min(s.touchTick, static_cast<int>(fr.size()) - 1))];
	Overlay3D::Basis actual = Overlay3D::FromRotator(tf.carPitch, tf.carYaw, tf.carRoll);
	Overlay3D::Basis aim = Overlay3D::Aim(fwd, actual.u);   // same roll as your car had, just aimed right

	// Nose (front face center) on the ideal contact point
	Vector rel = (contact - fwd * half.X) - ball;
	Vector base = tick < anchor ? ballNow : ball;
	float fade = tick < anchor ? 1.0f : std::max(0.55f, 1.0f - (tick - anchor) / 90.0f);
	Overlay3D::HitboxWire(canvas, camLoc, camFwd, base + rel, aim, half, 255, 220, 0, fade);

	if (showActual && tick >= s.touchTick)
	{
		Vector center = Vector(tf.carX, tf.carY, tf.carZ) + actual.f * off.X + actual.r * off.Y + actual.u * off.Z;
		Overlay3D::HitboxWire(canvas, camLoc, camFwd, center, actual, half, 80, 255, 120, 0.6f * fade);
	}
}

// Predicted ball path: dashed white line
void RLCoach::DrawPredictedPath(CanvasWrapper& canvas, const std::vector<BallSample>& path)
{
	if (path.size() < 2) return;
	CameraWrapper cam = gameWrapper->GetCamera();
	if (cam.IsNull()) return;
	Vector camLoc = cam.GetLocation();
	Vector camFwd = RotatorToVector(cam.GetRotation());

	Vector2F prev{};
	bool havePrevPt = false;
	int seg = 0;
	for (size_t i = 0; i < path.size(); i += 3)
	{
		Vector2F s;
		bool ok = ToScreen(canvas, camLoc, camFwd, Vector(path[i].x, path[i].y, path[i].z), s);
		if (ok && havePrevPt && (seg++ % 2 == 0))
		{
			canvas.SetColor(255, 255, 255, 200);
			canvas.DrawLine(prev, s, 2.0f);
		}
		prev = s;
		havePrevPt = ok;
	}
}

// ============================================================
// The review's searches: the smallest fix (the what-if, WhatIf.cpp)
// and the best line (the engine, Engine.cpp). Each has its own slot.
// ============================================================

void RLCoach::CancelWhatIf()
{
	whatIfCancel = true;
	std::lock_guard<std::mutex> lock(whatIfMutex);
	for (int s = 0; s < 2; s++) { lineResult[s] = WhatIfResult{}; lineFile[s].clear(); }
}

namespace
{
	// The engine's result, in the what-if's shape (so both slots work the same way: Watch it, Practice it, the path)
	WhatIfResult FromEngine(EngineResult&& e, int depth)
	{
		WhatIfResult r;
		r.point = WhatIfPoint::Engine;
		r.depth = depth;
		r.status = e.status == EngineStatus::Found ? WhatIfStatus::Found
			: e.status == EngineStatus::NoBall ? WhatIfStatus::NoShot
			: e.status == EngineStatus::Cancelled ? WhatIfStatus::Cancelled : WhatIfStatus::NothingBetter;
		r.startTick = e.startTick;
		r.realGrade = e.realGrade;
		r.cue = e.cue;
		r.detail = e.detail;
		r.grade = e.grade;
		r.wentIn = e.wentIn;
		r.gradeWhy = e.gradeWhy;
		r.held = e.held;
		r.heldOf = e.heldOf;
		r.frames = std::move(e.frames);
		r.eventTick = e.eventTick;
		r.note = e.note;
		r.tries = e.tries;
		r.ms = e.ms;
		return r;
	}

	bool Shows(const WhatIfResult& r) { return r.status == WhatIfStatus::Found || r.status == WhatIfStatus::CleanerWayIn; }
}

// Both searches for the rep being watched, one after the other: the what-if first (a few seconds), then the engine
// (up to 15). If the what-if finds no goal at the shot and the rep has a key moment (the reset, the setup touch, the
// first air dribble touch), it also tries different ways into that. A rep you scored a 5 on has nothing to beat.
void RLCoach::StartReview()
{
	if (!viewerActive || viewerFrames.empty() || viewerShowing >= 0) return;
	const std::string file = viewerRep.file;
	{
		// Already searched this session: show those results right away
		std::lock_guard<std::mutex> lock(whatIfMutex);
		for (const ReviewCacheEntry& c : reviewCache)
			if (c.file == file)
			{
				for (int s = 0; s < 2; s++) { lineResult[s] = c.line[s]; lineFile[s] = file; }
				uiReviewPending = false;
				return;
			}
	}
	if (viewerGrade.overall >= 5)
	{
		std::lock_guard<std::mutex> lock(whatIfMutex);
		for (int s = 0; s < 2; s++) { lineResult[s] = WhatIfResult{}; lineResult[s].status = WhatIfStatus::NotNeeded; lineFile[s] = file; }
		uiReviewPending = false;
		return;
	}
	if (whatIfBusy || !predictor.Ready()) return;   // the viewer window asks again once it can
	if (whatIfThread.joinable()) whatIfThread.join();

	std::vector<Frame> fr = viewerFrames;
	RepListItem item = viewerRep;
	ShotReference ref;
	shotBook.TryGet(item.shotKey, ref);
	std::vector<int> keyFix;   // the coach's checks on the way to the key moment, for "a cleaner way in"
	SegKind keySeg;
	std::string keyName;
	const bool hasKey = WhatIf::KeyMoment(viewerFrames, keySeg, keyName);
	if (hasKey)
		for (int c : coachBook.Firing(viewerShot, Coach::Measure(viewerFrames, viewerShot)))
			if (Coach::SegOf(c) == keySeg) keyFix.push_back(c);
	{
		std::lock_guard<std::mutex> lock(whatIfMutex);
		for (int s = 0; s < 2; s++) { lineResult[s] = WhatIfResult{}; lineFile[s] = file; }
	}
	uiReviewPending = false;
	whatIfCancel = false;
	whatIfBusy = true;
	whatIfDepth = 1;
	whatIfEngineStart = -1;
	whatIfRunningSlot = kFixLine;
	whatIfRunningPoint = static_cast<int>(WhatIfPoint::Shot);
	whatIfThread = std::thread([this, fr = std::move(fr), item, ref, hasKey, keyFix]() {
		// Below normal priority and capped, so the game never waits on it
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		auto publish = [&](int slot, WhatIfResult&& r) {
			std::lock_guard<std::mutex> lock(whatIfMutex);
			if (lineFile[slot] == item.file && !whatIfCancel) lineResult[slot] = std::move(r);
		};
		WhatIfResult fix, best;
		try
		{
			fix = WhatIf::Search(fr, item.goal, item.floorBeforeGoal, item.eventTick, ref, 0, &whatIfCancel, 4.0f, 1, WhatIfPoint::Shot, nullptr);
			if (!Shows(fix) && hasKey && !whatIfCancel)
			{
				whatIfRunningPoint = static_cast<int>(WhatIfPoint::KeyMoment);
				WhatIfResult key = WhatIf::Search(fr, item.goal, item.floorBeforeGoal, item.eventTick, ref, 0, &whatIfCancel, 4.0f, 1, WhatIfPoint::KeyMoment, &keyFix);
				if (Shows(key)) fix = std::move(key);
			}
		}
		catch (...) { fix = WhatIfResult{}; fix.status = WhatIfStatus::CantSimulate; }
		publish(kFixLine, WhatIfResult(fix));
		if (!whatIfCancel)
		{
			whatIfRunningSlot = kBestLine;
			whatIfRunningPoint = static_cast<int>(WhatIfPoint::Engine);
			try { best = FromEngine(Engine::Line(fr, item.goal, item.floorBeforeGoal, item.eventTick, ref, 0, &whatIfCancel, 15.0f, -1, 1), 1); }
			catch (...) { best = WhatIfResult{}; best.status = WhatIfStatus::CantSimulate; }
			publish(kBestLine, WhatIfResult(best));
		}
		if (!whatIfCancel && fix.status != WhatIfStatus::Cancelled && best.status != WhatIfStatus::Cancelled)
		{
			std::lock_guard<std::mutex> lock(whatIfMutex);
			std::erase_if(reviewCache, [&](const ReviewCacheEntry& c) { return c.file == item.file; });
			reviewCache.push_back(ReviewCacheEntry{ item.file, { std::move(fix), std::move(best) } });
			if (reviewCache.size() > 12) reviewCache.erase(reviewCache.begin());
		}
		whatIfRunningSlot = -1;
		whatIfBusy = false;
	});
}

// One search into its slot: "Search deeper" on the fix, "Think harder" or "Take over from here" on the best line
void RLCoach::StartWhatIf(int depth, WhatIfPoint point, int engineStart)
{
	if (!viewerActive || viewerFrames.empty() || whatIfBusy || !predictor.Ready()) return;
	if (viewerShowing >= 0) ViewerShowLine(-1, true);
	if (whatIfThread.joinable()) whatIfThread.join();   // the last search already finished (not busy)

	const int slot = point == WhatIfPoint::Engine ? kBestLine : kFixLine;
	std::vector<Frame> fr = viewerFrames;
	RepListItem item = viewerRep;
	ShotReference ref;
	shotBook.TryGet(item.shotKey, ref);   // same benchmark the breakdown used
	std::vector<int> fix;
	if (point == WhatIfPoint::KeyMoment)
	{
		SegKind keySeg;
		std::string keyName;
		if (WhatIf::KeyMoment(viewerFrames, keySeg, keyName))
			for (int c : coachBook.Firing(viewerShot, Coach::Measure(viewerFrames, viewerShot)))
				if (Coach::SegOf(c) == keySeg) fix.push_back(c);
	}
	{
		std::lock_guard<std::mutex> lock(whatIfMutex);
		lineResult[slot] = WhatIfResult{};
		lineFile[slot] = item.file;
	}
	whatIfCancel = false;
	whatIfBusy = true;
	whatIfDepth = depth;
	whatIfRunningSlot = slot;
	whatIfRunningPoint = static_cast<int>(point);
	if (point == WhatIfPoint::Engine) whatIfEngineStart = engineStart;
	whatIfThread = std::thread([this, fr = std::move(fr), item, ref, depth, point, fix, engineStart, slot]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		WhatIfResult r;
		try
		{
			if (point == WhatIfPoint::Engine)
				r = FromEngine(Engine::Line(fr, item.goal, item.floorBeforeGoal, item.eventTick, ref, 0, &whatIfCancel,
					depth >= 2 ? 60.0f : 15.0f, engineStart, depth), depth);
			else
				r = WhatIf::Search(fr, item.goal, item.floorBeforeGoal, item.eventTick, ref, 0, &whatIfCancel, depth >= 2 ? 20.0f : 4.0f, depth, point, &fix);
		}
		catch (...)
		{
			r = WhatIfResult{};
			r.status = WhatIfStatus::CantSimulate;
		}
		{
			std::lock_guard<std::mutex> lock(whatIfMutex);
			if (lineFile[slot] == item.file && !whatIfCancel)
			{
				lineResult[slot] = r;
				for (ReviewCacheEntry& c : reviewCache) if (c.file == item.file) c.line[slot] = std::move(r);
			}
		}
		whatIfRunningSlot = -1;
		whatIfBusy = false;
	});
}

// Plays a line in the viewer instead of your rep (or goes back to your rep). Same car, same overlays.
void RLCoach::ViewerShowLine(int slot, bool keepPlayhead)
{
	if (!viewerActive) return;
	if (slot == viewerShowing)
	{
		// Already showing it: "Watch it" again starts it over
		if (!keepPlayhead)
		{
			int start = 0;
			if (slot >= 0)
			{
				std::lock_guard<std::mutex> lock(whatIfMutex);
				start = std::max(0, lineResult[slot].startTick - 60);
			}
			ViewerSeek(static_cast<float>(start));
			viewerPlaying = true;
			uiViewerPlaying = true;
		}
		return;
	}
	// Back to your rep first (also on the way from one line to the other)
	if (viewerShowing >= 0)
	{
		viewerFrames = std::move(viewerRealFrames);
		viewerRealFrames.clear();
		viewerShot = viewerRealShot;
		viewerShowing = -1;
		uiViewerTotal = static_cast<int>(viewerFrames.size());
		ViewerSeek(viewerPlayhead);   // clamps to your rep's length
	}
	if (slot >= 0)
	{
		std::vector<Frame> alt;
		int start = 0;
		{
			std::lock_guard<std::mutex> lock(whatIfMutex);
			const WhatIfResult& r = lineResult[slot];
			if (!Shows(r) || lineFile[slot] != viewerRep.file || r.frames.size() < 2) { uiShowing = -1; return; }
			alt = r.frames;
			start = r.startTick;
		}
		// A line that scores ends as the ball crosses, like a real rep does. Past that the replayed ball would only
		// be held on the goal line (free play would explode it), which looks like it died there.
		for (int i = std::max(1, start); i < static_cast<int>(alt.size()); i++)
			if (std::fabs(alt[i].ballX) < kGoalHalfWidth && alt[i].ballZ < kGoalHeight && std::fabs(alt[i].ballY) > kGoalLineY)
			{
				alt.resize(i + 1);
				break;
			}
		PracticeExit();
		viewerRealFrames = std::move(viewerFrames);
		viewerRealShot = viewerShot;
		viewerFrames = std::move(alt);
		viewerShot = ShotGeometry::Analyze(viewerFrames);
		viewerShowing = slot;
		uiLastLine = slot;
		uiViewerTotal = static_cast<int>(viewerFrames.size());
		if (keepPlayhead) ViewerSeek(viewerPlayhead);
		else
		{
			ViewerSeek(static_cast<float>(std::max(0, start - 60)));   // a half second of lead-in before it changes
			viewerPlaying = true;
			uiViewerPlaying = true;
		}
	}
	else if (!keepPlayhead)
	{
		ViewerSeek(0.0f);
		viewerPlaying = true;
		uiViewerPlaying = true;
	}
	uiShowing = viewerShowing;
}

// Practice from where a line starts, with its path drawn faintly as the line to follow
void RLCoach::PracticeLine(int slot)
{
	std::vector<Frame> alt;
	int start = 0;
	std::string cue;
	{
		std::lock_guard<std::mutex> lock(whatIfMutex);
		const WhatIfResult& r = lineResult[slot];
		if (!Shows(r) || lineFile[slot] != viewerRep.file) return;
		alt = r.frames;
		start = r.startTick;
		cue = r.cue;
	}
	if (viewerShowing >= 0) ViewerShowLine(-1, true);
	PracticeStartAt(start);
	if (practiceState == PracticeState::Off) return;
	practiceRefFrames = std::move(alt);
	practiceRefLabel = std::string(slot == kBestLine ? "Faint line: the engine's line (" : "Faint line: the smallest fix (") + cue + ")";
}

// While watching your rep: the last line you looked at, faintly in cyan from where it changes. While watching a
// line: your real path in grey, for comparison.
void RLCoach::DrawWhatIfPath(CanvasWrapper& canvas)
{
	CameraWrapper cam = gameWrapper->GetCamera();
	if (cam.IsNull()) return;
	Vector camLoc = cam.GetLocation();
	Vector camFwd = RotatorToVector(cam.GetRotation());

	auto draw = [&](const std::vector<Frame>& fr, int from, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha, bool ball) {
		Vector2F prev{};
		bool havePrevPt = false;
		for (int i = std::max(0, from); i < static_cast<int>(fr.size()); i += 2)
		{
			const Frame& f = fr[i];
			Vector2F s;
			bool ok = ToScreen(canvas, camLoc, camFwd, ball ? Vector(f.ballX, f.ballY, f.ballZ) : Vector(f.carX, f.carY, f.carZ), s);
			if (ok && havePrevPt)
			{
				canvas.SetColor(r, g, b, alpha);
				canvas.DrawLine(prev, s, 2.0f);
			}
			prev = s;
			havePrevPt = ok;
		}
	};

	std::lock_guard<std::mutex> lock(whatIfMutex);
	if (viewerShowing >= 0)
	{
		const int start = lineResult[viewerShowing].startTick;
		draw(viewerRealFrames, start, 200, 200, 200, 110, false);
		draw(viewerRealFrames, start, 200, 200, 200, 70, true);
		return;
	}
	int slot = uiLastLine;
	if (!Shows(lineResult[slot]) || lineFile[slot] != viewerRep.file) slot = 1 - slot;
	const WhatIfResult& r = lineResult[slot];
	if (!Shows(r) || lineFile[slot] != viewerRep.file) return;
	draw(r.frames, r.startTick, 56, 189, 248, 200, false);
	draw(r.frames, r.startTick, 125, 211, 252, 120, true);
}

// ============================================================
// Viewer window (render thread). Anything that touches the game
// is handed to the game thread with gameWrapper->Execute.
// ============================================================

void RLCoach::RefreshRepList()
{
	repList = writer ? RepLibrary::ListReps(writer->Folder()) : std::vector<RepListItem>{};
	repListLoaded = true;
	if (selectedRep >= static_cast<int>(repList.size())) selectedRep = -1;
	StartGradeRefresh();
}

// Grades every rep in the background (only new ones, or ones whose benchmark changed)
void RLCoach::StartGradeRefresh()
{
	if (!writer || gradeThreadBusy) return;
	if (gradeThread.joinable()) gradeThread.join();
	gradeThreadBusy = true;
	auto root = writer->Folder();
	gradeThread = std::thread([this, root]() {
		shotBook.Update(root);
		gradeBook.Refresh(root, shotBook);
		habitBook.Update(root);
		coachBook.Update(root);
		gradeThreadBusy = false;
	});
}

void RLCoach::FetchUiFonts()
{
	bool missing = false;
	for (ImFont* f : uiFonts) missing = missing || !f;
	if (!missing) return;
	auto gui = gameWrapper->GetGUIManager();
	for (int i = 0; i < UiFonts::kCount; i++)
		if (!uiFonts[i]) uiFonts[i] = gui.GetFont(UiFonts::kSpecs[i].name);
}

namespace
{
	// Pushes a font for one scope (null = ImGui's default font)
	struct FontScope
	{
		explicit FontScope(ImFont* f) { ImGui::PushFont(f); }
		~FontScope() { ImGui::PopFont(); }
	};

	// What the window needs from a search's result (copied under the lock, without the frames)
	struct LineView
	{
		WhatIfStatus status = WhatIfStatus::None;
		WhatIfPoint point = WhatIfPoint::Shot;
		std::string what, cue, detail, gradeWhy, fixes, note;
		int grade = 0, held = 0, heldOf = 0, startTick = -1, depth = 1;
		bool wentIn = false, mine = false;
		bool shows() const { return mine && (status == WhatIfStatus::Found || status == WhatIfStatus::CleanerWayIn); }
	};

	std::string Upper(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return s;
	}

	// A 5-point star; filled when kept
	void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col)
	{
		ImVec2 pts[10];
		for (int k = 0; k < 10; k++)
		{
			float a = -1.5708f + k * 0.62832f;
			float rr = (k % 2 == 0) ? r : r * 0.45f;
			pts[k] = ImVec2(c.x + rr * std::cos(a), c.y + rr * std::sin(a));
		}
		if (filled)
			for (int k = 0; k < 10; k++) dl->AddTriangleFilled(c, pts[k], pts[(k + 1) % 10], col);
		else
			dl->AddPolyline(pts, 10, col, true, 1.4f);
	}

}

void RLCoach::RenderWindow()
{
	using namespace UiStyle;
	if (!repListLoaded || repListStale.exchange(false)) RefreshRepList();
	FetchUiFonts();
	FontScope bodyFont(uiFonts[UiFonts::Body]);
	ImFont* fLabel = uiFonts[UiFonts::Label];
	ImFont* fBold = uiFonts[UiFonts::Bold];
	ImFont* fHead = uiFonts[UiFonts::Head];
	ImFont* fMono = uiFonts[UiFonts::Mono];
	ImFont* fBig = uiFonts[UiFonts::Big];

	auto onGame = [this](std::function<void()> fn) {
		gameWrapper->Execute([fn](GameWrapper*) { fn(); });
	};
	// Small capitals in a color (section labels, tab names)
	auto label = [&](const char* text, ImVec4 color) {
		FontScope f(fLabel);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	};

	// ---- Title row ----
	{
		FontScope f(fLabel);
		ImGui::TextColored(kAccent, "ROAD2");
		ImGui::SameLine();
		ImGui::TextColored(kMuted, "REP VIEWER");
	}
	{
		std::string info = std::format("{} reps", repList.size());
		float w = ImGui::CalcTextSize(info.c_str()).x + ImGui::CalcTextSize("Refresh").x + 40.0f;
		ImGui::SameLine(std::max(200.0f, ImGui::GetWindowContentRegionMax().x - w));
		ImGui::TextDisabled("%s", info.c_str());
		ImGui::SameLine();
		if (LinkButton("Refresh")) RefreshRepList();
	}

	gradeBook.TryCopy(uiAutoGrades);   // keeps the last copy if a refresh is busy
	std::string watchingFile;
	{
		std::lock_guard<std::mutex> lock(uiSegMutex);
		watchingFile = uiViewerFile;
	}

	// ---- Practice controls (while practicing) ----
	if (uiPracticeActive)
	{
		Heading("PRACTICE");
		ImGui::PushItemWidth(260);
		int pct = static_cast<int>(std::lround(uiPracticeSpeed.load() * 100.0f));
		if (ImGui::SliderInt("Game speed", &pct, 10, 100, "%d%%"))
		{
			pct = std::max(10, std::min(100, static_cast<int>(std::lround(pct / 5.0)) * 5));
			float sp = pct / 100.0f;
			uiPracticeSpeed = sp;
			onGame([this, sp]() { PracticeSetSpeed(sp); });
		}
		float lead = uiPracticeLeadIn.load();
		if (ImGui::SliderFloat("Lead-in before the moment", &lead, 0.0f, 2.0f, "%.2fs"))
		{
			lead = std::round(lead * 20.0f) / 20.0f;
			uiPracticeLeadIn = lead;
			onGame([this, lead]() { PracticeSetLeadIn(lead); PracticeRetry(); });
		}
		ImGui::PopItemWidth();
		bool ramp = uiPracticeAutoRamp;
		if (ImGui::Checkbox("Auto-ramp (3 hits in a row = +10% speed)", &ramp))
		{
			uiPracticeAutoRamp = ramp;
			onGame([this, ramp]() { practiceAutoRamp = ramp; practiceStreak = 0; });
		}
		if (AccentButton("Retry", ImVec2(90, 0))) onGame([this]() { PracticeRetry(); });
		ImGui::SameLine();
		if (OutlineButton("Stop practicing")) onGame([this]() { PracticeExit(); });
		ImGui::SameLine();
		ImGui::TextDisabled("results show on the in-game panel");
	}

	// ---- The review of the rep being watched ----
	if (uiViewerActive)
	{
		int total = uiViewerTotal;
		const float headF = uiViewerPlayhead.load();
		const int head = static_cast<int>(headF);
		std::vector<Segment> segs;
		std::vector<UiGrade> grades;
		std::vector<CoachNote> notes;
		std::vector<ReviewStat> stats;
		std::string watching, overallName, overallWhy, mechanic, keyWhat;
		int segTotal = 0, overallGrade = 0;
		{
			std::lock_guard<std::mutex> lock(uiSegMutex);
			segs = uiSegs;
			segTotal = uiSegTotal;
			grades = uiGrades;
			notes = uiNotes;
			watching = uiViewerFile;
			overallGrade = uiOverallGrade;
			overallName = uiOverallName;
			overallWhy = uiOverallWhy;
			mechanic = uiMechanic;
			stats = uiStats;
			keyWhat = uiWhatIfKeyWhat;
		}
		const RepListItem* cur = nullptr;
		for (const RepListItem& it : repList) if (it.file == watching) { cur = &it; break; }

		LineView line[2];
		{
			std::lock_guard<std::mutex> lock(whatIfMutex);
			for (int s = 0; s < 2; s++)
			{
				const WhatIfResult& r = lineResult[s];
				LineView& v = line[s];
				v.mine = lineFile[s] == watching;
				v.status = r.status; v.point = r.point; v.what = r.what; v.cue = r.cue; v.detail = r.detail; v.gradeWhy = r.gradeWhy;
				v.fixes = r.fixes; v.note = r.note; v.grade = r.grade; v.held = r.held; v.heldOf = r.heldOf; v.startTick = r.startTick;
				v.depth = r.depth; v.wentIn = r.wentIn;
			}
		}
		const bool busy = whatIfBusy;
		const int busySlot = busy ? whatIfRunningSlot.load() : -1;
		const bool pending = uiReviewPending;
		const bool simReady = predictor.Ready();
		if (pending && simReady && !busy) onGame([this]() { StartReview(); });

		// A new rep opens on Your shot
		if (uiTabFile != watching) { uiTabFile = watching; uiTab = 0; }

		// The root cause (the coach's), else the grades' biggest fix: the part of the rep to practice
		int rootSeg = -1, fixSeg = -1;
		std::string fixWhy;
		if (!notes.empty() && notes[0].rootCause)
			for (int i = 0; i < static_cast<int>(segs.size()); i++)
				if (segs[i].kind == notes[0].seg) rootSeg = i;
		for (const UiGrade& g : grades) if (g.biggest) { fixSeg = g.segIndex; fixWhy = g.why; }
		auto segAt = [&](int tick) -> int {
			for (int i = 0; i < static_cast<int>(segs.size()); i++)
				if (segs[i].kind != SegKind::Recovery && tick >= segs[i].start && tick <= segs[i].end) return i;
			return -1;
		};

		// The tab you're on decides what plays in the world: your rep, or that line once it's found
		{
			const int want = uiTab == 0 ? -1 : (line[uiTab - 1].shows() ? uiTab - 1 : -1);
			static int requested = -2;
			if (uiPracticeActive) requested = -2;
			else if (want == uiShowing.load()) requested = -2;
			else if (want != requested)
			{
				requested = want;
				onGame([this, want]() { ViewerShowLine(want, true); });
			}
		}

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float W = ImGui::GetContentRegionAvail().x;
		ImGui::Spacing();

		// ---------- Header: the grade gauge, what it was and why ----------
		{
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float D = 112.0f;
			const ImVec2 c(p0.x + D * 0.5f + 2.0f, p0.y + D * 0.5f);
			const ImVec4 gc = GradeColor(overallGrade);
			Gauge(dl, c, D * 0.5f - 12.0f, 9.0f, overallGrade, gc);
			{
				FontScope f(fBig);
				std::string n = overallGrade > 0 ? std::to_string(overallGrade) : "-";
				ImVec2 ts = ImGui::CalcTextSize(n.c_str());
				dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f - 5.0f), U32(kText), n.c_str());
			}
			{
				FontScope f(fLabel);
				ImVec2 ts = ImGui::CalcTextSize("OF 5");
				dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + 18.0f), U32(kMuted), "OF 5");
			}

			// Keep and close, top right
			const float bs = 30.0f;
			ImVec2 xPos(p0.x + W - bs, p0.y), starPos(p0.x + W - 2 * bs - 6.0f, p0.y);
			ImGui::SetCursorScreenPos(starPos);
			const bool kept = cur && cur->kept;
			if (ImGui::InvisibleButton("keep", ImVec2(bs, bs)) && cur) ToggleKeep(watching);
			bool hov = ImGui::IsItemHovered();
			dl->AddRect(starPos, ImVec2(starPos.x + bs, starPos.y + bs), U32(hov ? kLine : kLineSoft), 8.0f);
			DrawStar(dl, ImVec2(starPos.x + bs * 0.5f, starPos.y + bs * 0.5f + 1.0f), 8.0f, kept, kept ? U32(Hex(0xFBBF24)) : U32(hov ? kText : kMuted));
			if (hov) ImGui::SetTooltip(kept ? "Kept forever. Click to un-keep." : "Keep this rep forever (never cleaned up)");
			ImGui::SetCursorScreenPos(xPos);
			if (ImGui::InvisibleButton("close", ImVec2(bs, bs))) onGame([this]() { ViewerExit(); });
			hov = ImGui::IsItemHovered();
			dl->AddRect(xPos, ImVec2(xPos.x + bs, xPos.y + bs), U32(hov ? kLine : kLineSoft), 8.0f);
			{
				const ImVec2 m(xPos.x + bs * 0.5f, xPos.y + bs * 0.5f);
				const ImU32 col = U32(hov ? kText : kMuted);
				dl->AddLine(ImVec2(m.x - 5, m.y - 5), ImVec2(m.x + 5, m.y + 5), col, 2.0f);
				dl->AddLine(ImVec2(m.x + 5, m.y - 5), ImVec2(m.x - 5, m.y + 5), col, 2.0f);
			}
			if (hov) ImGui::SetTooltip("Stop watching");

			// The words, right of the gauge
			const float tx = p0.x + D + 20.0f;
			ImGui::SetCursorScreenPos(ImVec2(tx, p0.y + 6.0f));
			ImGui::BeginGroup();
			ImGui::PushTextWrapPos(p0.x + W - 2 * bs - 16.0f - (ImGui::GetWindowPos().x) + ImGui::GetScrollX());
			{
				FontScope f(fLabel);
				ImGui::TextColored(kAccent, "%s", mechanic.c_str());
				if (cur)
				{
					std::string when = cur->savedAt.size() >= 16 ? cur->savedAt.substr(11, 5) : cur->savedAt;
					ImGui::SameLine();
					ImGui::TextColored(kMuted, "%s", std::format("\xC2\xB7  {}  \xC2\xB7  {:.1f} S", when, cur->duration).c_str());
					if (cur->rating > 0) { ImGui::SameLine(); ImGui::TextColored(kMuted, "%s", std::format("\xC2\xB7  YOU RATED IT {}", cur->rating).c_str()); }
				}
			}
			{
				FontScope f(fHead);
				ImGui::TextWrapped("%s", overallName.empty() ? "Not graded" : overallName.c_str());
			}
			ImGui::PushStyleColor(ImGuiCol_Text, kSub);
			if (!overallWhy.empty()) ImGui::TextWrapped("%s", overallWhy.c_str());
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ImGui::EndGroup();
			const float bottom = std::max(p0.y + D, ImGui::GetItemRectMax().y);
			ImGui::SetCursorScreenPos(ImVec2(p0.x, bottom + 14.0f));
		}

		// ---------- The timeline card: markers, segments colored by grade, the playhead, transport ----------
		if (!segs.empty() && segTotal > 1)
		{
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float pad = 14.0f, markerH = 24.0f, barH = 30.0f, transportH = 32.0f;
			const float cardH = pad + markerH + barH + 10.0f + transportH + pad;
			dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + cardH), U32(kPanel2), 12.0f);
			dl->AddRect(p0, ImVec2(p0.x + W, p0.y + cardH), U32(kLineSoft), 12.0f);
			const float x0 = p0.x + pad, x1 = p0.x + W - pad, barY = p0.y + pad + markerH;
			auto xOf = [&](int tick) { return x0 + (x1 - x0) * static_cast<float>(std::clamp(tick, 0, segTotal - 1)) / static_cast<float>(segTotal - 1); };

			// Markers: the root cause, and where the line on the open tab takes over
			struct Marker { int tick; const char* text; ImVec4 color; };
			std::vector<Marker> marks;
			if (rootSeg >= 0) marks.push_back({ segs[rootSeg].start, "ROOT CAUSE", kRoot });
			{
				const int slot = uiTab == 2 ? kBestLine : kFixLine;
				if (line[slot].shows() && line[slot].startTick >= 0)
					marks.push_back({ line[slot].startTick, slot == kBestLine ? "ENGINE TAKES OVER" : "FIX", kAccent });
			}
			std::sort(marks.begin(), marks.end(), [](const Marker& a, const Marker& b) { return a.tick < b.tick; });
			{
				FontScope f(fLabel);
				// Centered over the diamond, unless two would collide: then the left one ends at its diamond, the right one starts at its
				bool clash = false;
				if (marks.size() == 2)
				{
					const float wa = ImGui::CalcTextSize(marks[0].text).x, wb = ImGui::CalcTextSize(marks[1].text).x;
					clash = xOf(marks[1].tick) - xOf(marks[0].tick) < (wa + wb) * 0.5f + 10.0f;
				}
				for (size_t i = 0; i < marks.size(); i++)
				{
					const float x = xOf(marks[i].tick);
					Diamond(dl, ImVec2(x, barY - 7.0f), 4.5f, marks[i].color);
					const ImVec2 ts = ImGui::CalcTextSize(marks[i].text);
					float lx = clash ? (i == 0 ? x - ts.x - 2.0f : x + 2.0f) : x - ts.x * 0.5f;
					lx = std::clamp(lx, x0, x1 - ts.x);
					dl->AddText(ImVec2(lx, barY - 13.0f - ts.y), U32(marks[i].color), marks[i].text);
				}
			}

			// Segments (the recovery runs as a thin strip underneath, since it overlaps the result)
			int hoverSeg = -1;
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			for (int si = 0; si < static_cast<int>(segs.size()); si++)
			{
				const Segment& sg = segs[si];
				std::string gradeName;
				for (const UiGrade& ug : grades) if (ug.segIndex == si) gradeName = ug.name;
				float sx0 = xOf(sg.start) + 1.5f, sx1 = std::max(xOf(sg.end + 1) - 1.5f, sx0 + 3.0f);
				if (sg.kind == SegKind::Recovery)
				{
					dl->AddRectFilled(ImVec2(sx0, barY + barH + 3.0f), ImVec2(sx1, barY + barH + 6.0f), U32(Hex(0xA78BFA), 0.55f), 2.0f);
					continue;
				}
				const ImVec4 col = gradeName.empty() ? kMuted : SegGradeColor(gradeName);
				const bool hovered = mouse.x >= sx0 && mouse.x < sx1 && mouse.y >= barY && mouse.y < barY + barH;
				if (hovered) hoverSeg = si;
				dl->AddRectFilled(ImVec2(sx0, barY), ImVec2(sx1, barY + barH), U32(col, gradeName.empty() ? 0.10f : (hovered ? 0.34f : 0.22f)), 5.0f);
				dl->AddRectFilled(ImVec2(sx0, barY), ImVec2(sx1, barY + 2.0f), U32(col, gradeName.empty() ? 0.35f : 1.0f), 2.0f);
				FontScope f(fLabel);
				std::string name = Segmenter::Name(sg.kind);
				ImVec2 ts = ImGui::CalcTextSize(name.c_str());
				if (sx1 - sx0 > ts.x + 12.0f)
					dl->AddText(ImVec2(sx0 + 7.0f, barY + (barH - ts.y) * 0.5f + 1.0f), U32(gradeName.empty() ? kMuted : kText, 0.92f), name.c_str());
			}
			// The bar scrubs: click or drag to move through the rep
			ImGui::SetCursorScreenPos(ImVec2(x0, barY));
			ImGui::InvisibleButton("scrubbar", ImVec2(x1 - x0, barH + 6.0f));
			{
				static int lastSent = -1;
				if (ImGui::IsItemActive())
				{
					const int tick = std::clamp(static_cast<int>(std::lround((mouse.x - x0) / (x1 - x0) * (segTotal - 1))), 0, segTotal - 1);
					if (tick != lastSent)
					{
						lastSent = tick;
						const float target = static_cast<float>(tick);
						onGame([this, target]() { viewerPlaying = false; uiViewerPlaying = false; ViewerSeek(target); });
					}
				}
				else lastSent = -1;
			}
			if (hoverSeg >= 0 && ImGui::IsItemHovered())
			{
				std::string gradeName, why;
				for (const UiGrade& ug : grades) if (ug.segIndex == hoverSeg) { gradeName = ug.name; why = ug.why; }
				const Segment& sg = segs[hoverSeg];
				ImGui::SetTooltip("%s%s%s: %.2fs to %.2fs%s%s", Segmenter::Name(sg.kind), gradeName.empty() ? "" : " - ", gradeName.c_str(),
					sg.start / 120.0f, (sg.end + 1) / 120.0f, why.empty() ? "" : "\n", why.c_str());
			}
			// Playhead
			{
				const float px = xOf(std::min(head, segTotal - 1));
				dl->AddLine(ImVec2(px, barY - 4.0f), ImVec2(px, barY + barH + 4.0f), U32(kText, 0.25f), 6.0f);
				dl->AddLine(ImVec2(px, barY - 4.0f), ImVec2(px, barY + barH + 4.0f), U32(kText), 2.0f);
			}

			// Transport: back to start, play, step, time, speed, loop
			ImGui::SetCursorScreenPos(ImVec2(x0, barY + barH + 12.0f));
			const ImVec2 sq(32, 28);
			if (ImGui::Button("|<", sq)) onGame([this]() { ViewerSeek(0.0f); });
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the start");
			ImGui::SameLine(0, 6);
			const bool playing = uiViewerPlaying;
			if (AccentButton(playing ? "||" : ">", ImVec2(44, 28)))
				onGame([this]() { viewerPlaying = !viewerPlaying; uiViewerPlaying = viewerPlaying; });
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(playing ? "Pause" : "Play");
			ImGui::SameLine(0, 6);
			if (ImGui::Button("<", sq)) onGame([this]() { viewerPlaying = false; ViewerSeek(std::floor(viewerPlayhead) - 1.0f); });
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back 1 tick");
			ImGui::SameLine(0, 6);
			if (ImGui::Button(">##step", sq)) onGame([this]() { viewerPlaying = false; ViewerSeek(std::floor(viewerPlayhead) + 1.0f); });
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward 1 tick");
			ImGui::SameLine(0, 12);
			{
				FontScope f(fMono);
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(kSub, "%s", std::format("{:.2f} / {:.2f} s", headF / 120.0f, total / 120.0f).c_str());
			}
			// Speed, as a small segmented control on the right
			{
				const float speeds[4] = { 0.1f, 0.25f, 0.5f, 1.0f };
				const char* names[4] = { "0.1x", "0.25x", "0.5x", "1x" };
				float segW = 0.0f;
				const ImGuiStyle& st = ImGui::GetStyle();
				for (const char* n : names) segW += ImGui::CalcTextSize(n).x + st.FramePadding.x * 2.0f + 2.0f;
				const float loopW = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("Loop").x + 10.0f;
				ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, (x1 - ImGui::GetWindowPos().x) - segW - loopW));
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 6));
				for (int i = 0; i < 4; i++)
				{
					if (i > 0) ImGui::SameLine();
					const bool on = std::fabs(uiViewerSpeed.load() - speeds[i]) < 0.001f;
					ImGui::PushStyleColor(ImGuiCol_Button, on ? Hex(0x1F3350) : ImVec4(0, 0, 0, 0));
					ImGui::PushStyleColor(ImGuiCol_Text, on ? kText : kMuted);
					if (ImGui::Button(names[i], ImVec2(0, 28)))
					{
						const float sp = speeds[i];
						uiViewerSpeed = sp;
						onGame([this, sp]() { viewerSpeed = sp; });
					}
					ImGui::PopStyleColor(2);
				}
				ImGui::PopStyleVar();
				ImGui::SameLine(0, 10);
				ImGui::AlignTextToFramePadding();
				if (ImGui::Checkbox("Loop", &uiLoop))
				{
					const bool loop = uiLoop;
					onGame([this, loop]() { viewerLoop = loop; });
				}
			}
			ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + cardH + 16.0f));
		}

		// ---------- Tabs: Your shot | Smallest fix | Best line ----------
		auto tabStatus = [&](int tab, std::string& gradeText, ImVec4& gradeCol, std::string& sub) {
			if (tab == 0)
			{
				gradeText = overallGrade > 0 ? std::to_string(overallGrade) : "-";
				gradeCol = GradeColor(overallGrade);
				sub = cur ? (cur->goal ? "goal" : overallGrade == 2 ? "close miss" : "miss") : "";
				return;
			}
			const int slot = tab - 1;
			const LineView& v = line[slot];
			gradeCol = kMuted;
			if (busySlot == slot || (busy && slot == kBestLine && busySlot == kFixLine && v.mine && v.status == WhatIfStatus::None) || (pending && !v.mine))
			{
				gradeText = busySlot == slot ? "" : "...";
				sub = busySlot == slot ? "thinking" : "waiting";
				if (!simReady) sub = "loading";
				return;
			}
			if (!v.mine || v.status == WhatIfStatus::None) { gradeText = "-"; sub = simReady ? "" : "loading"; return; }
			switch (v.status)
			{
			case WhatIfStatus::Found:
				gradeText = std::to_string(v.grade);
				gradeCol = GradeColor(v.grade);
				sub = slot == kFixLine ? "found" : (v.grade >= 5 ? "a 5" : v.grade == overallGrade ? "cleaner" : "found");
				return;
			case WhatIfStatus::CleanerWayIn: gradeText = "~"; gradeCol = kAccent; sub = "cleaner way in"; return;
			case WhatIfStatus::NotNeeded: gradeText = "-"; sub = "nothing to beat"; return;
			case WhatIfStatus::NothingBetter: gradeText = "-"; sub = "none found"; return;
			default: gradeText = "-"; sub = WhatIf::StatusText(v.status); return;
			}
		};
		{
			const char* names[3] = { "YOUR SHOT", "SMALLEST FIX", "BEST LINE" };
			const float gap = 8.0f, tabW = (W - 2 * gap) / 3.0f, tabH = 60.0f;
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			for (int t = 0; t < 3; t++)
			{
				const ImVec2 a(p0.x + t * (tabW + gap), p0.y), b(a.x + tabW, a.y + tabH);
				ImGui::SetCursorScreenPos(a);
				ImGui::PushID(t + 8000);
				if (ImGui::InvisibleButton("tab", ImVec2(tabW, tabH))) uiTab = t;
				const bool hov = ImGui::IsItemHovered();
				ImGui::PopID();
				const bool on = uiTab == t;
				dl->AddRectFilled(a, b, U32(on ? kPanel : (hov ? kPanel2 : kBg)), 12.0f, ImDrawCornerFlags_Top);
				dl->AddRect(a, ImVec2(b.x, b.y + (on ? 1.0f : 0.0f)), U32(on ? kLine : kLineSoft), 12.0f, ImDrawCornerFlags_Top);
				if (on) dl->AddRectFilled(ImVec2(a.x + 1, b.y - 1), ImVec2(b.x - 1, b.y + 2), U32(kPanel));   // open into the panel below
				std::string gText, sub;
				ImVec4 gCol;
				tabStatus(t, gText, gCol, sub);
				{
					FontScope f(fLabel);
					dl->AddText(ImVec2(a.x + 14, a.y + 10), U32(on ? kAccent : kMuted), names[t]);
				}
				float gx = a.x + 14;
				{
					FontScope f(fMono);
					const ImVec2 ts = ImGui::CalcTextSize(gText.c_str());
					dl->AddText(ImVec2(gx, a.y + 26), U32(gCol), gText.c_str());
					gx += ts.x + 8.0f;
				}
				const int slot = t - 1;
				if (t > 0 && busySlot == slot)
				{
					Spinner(dl, ImVec2(gx + 6.0f, a.y + 38.0f), 6.0f, 2.0f, kAccent);
					const ImVec2 ts = ImGui::CalcTextSize(sub.c_str());
					dl->AddText(ImVec2(gx + 20.0f, a.y + 38.0f - ts.y * 0.5f), U32(kMuted), sub.c_str());
				}
				else if (!sub.empty())
				{
					const ImVec2 ts = ImGui::CalcTextSize(sub.c_str());
					dl->AddText(ImVec2(gx, a.y + 38.0f - ts.y * 0.5f), U32(kMuted), sub.c_str());
				}
			}
			ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + tabH));
		}

		// ---------- The open tab's panel ----------
		{
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float pad = 18.0f;
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
			ImGui::BeginGroup();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - 2 * pad);
			const float innerW = W - 2 * pad;

			// Pieces the panels share
			auto tiles = [&](const std::vector<ReviewStat>& ts) {
				if (ts.empty()) return;
				const float g = 10.0f, tw = (innerW - g * (ts.size() - 1)) / static_cast<float>(ts.size()), th = 72.0f;
				const ImVec2 q = ImGui::GetCursorScreenPos();
				for (size_t i = 0; i < ts.size(); i++)
				{
					const ImVec2 a(q.x + i * (tw + g), q.y), b(a.x + tw, a.y + th);
					dl->AddRectFilled(a, b, U32(kPanel2), 10.0f);
					dl->AddRect(a, b, U32(kLineSoft), 10.0f);
					{ FontScope f(fLabel); dl->AddText(ImVec2(a.x + 12, a.y + 10), U32(kMuted), Upper(ts[i].label).c_str()); }
					{ FontScope f(fMono); dl->AddText(ImVec2(a.x + 12, a.y + 25), U32(kText), ts[i].value.c_str()); }
					dl->AddText(ImVec2(a.x + 12, a.y + 49), U32(kMuted), ts[i].sub.c_str());
				}
				ImGui::Dummy(ImVec2(innerW, th));
			};
			auto headline = [&](const std::string& text) {
				FontScope f(fHead);
				ImGui::TextWrapped("%s", text.c_str());
			};
			auto subline = [&](const std::string& text) {
				if (text.empty()) return;
				ImGui::PushStyleColor(ImGuiCol_Text, kSub);
				ImGui::TextWrapped("%s", text.c_str());
				ImGui::PopStyleColor();
			};
			const ImVec2 btn((innerW - 10.0f) * 0.5f, 40.0f);
			// An empty state: an icon, a title and a line of text
			auto emptyState = [&](int icon, const std::string& title, const std::string& text) {
				const ImVec2 q = ImGui::GetCursorScreenPos();
				const float s = 46.0f;
				const ImVec2 c(q.x + s * 0.5f, q.y + s * 0.5f);
				if (icon == 0) Spinner(dl, c, 17.0f, 4.0f, kAccent);
				else
				{
					const ImVec4 col = icon == 2 ? Hex(0x22D3EE) : kMuted;
					dl->AddRectFilled(q, ImVec2(q.x + s, q.y + s), U32(icon == 2 ? Hex(0x0D2A33) : kPanel2), 12.0f);
					dl->AddRect(q, ImVec2(q.x + s, q.y + s), U32(icon == 2 ? col : kLine), 12.0f);
					if (icon == 1)
					{
						dl->AddCircle(ImVec2(c.x - 2, c.y - 2), 7.0f, U32(col), 20, 2.0f);
						dl->AddLine(ImVec2(c.x + 3, c.y + 3), ImVec2(c.x + 8, c.y + 8), U32(col), 2.2f);
					}
					else
					{
						dl->AddLine(ImVec2(c.x - 8, c.y + 1), ImVec2(c.x - 2, c.y + 7), U32(col), 2.6f);
						dl->AddLine(ImVec2(c.x - 2, c.y + 7), ImVec2(c.x + 9, c.y - 6), U32(col), 2.6f);
					}
				}
				ImGui::SetCursorScreenPos(ImVec2(q.x + s + 16.0f, q.y + 2.0f));
				ImGui::BeginGroup();
				{ FontScope f(fBold); ImGui::TextWrapped("%s", title.c_str()); }
				subline(text);
				ImGui::EndGroup();
				ImGui::SetCursorScreenPos(ImVec2(q.x, std::max(q.y + s, ImGui::GetItemRectMax().y) + 8.0f));
			};
			auto segName = [&](int tick) -> std::string {
				const int si = segAt(tick);
				return si >= 0 ? Segmenter::Name(segs[si].kind) : "";
			};

			if (uiTab == 0)
			{
				// ---- Your shot ----
				label("WHAT HAPPENED", kMuted);
				std::string what = !notes.empty() ? notes[0].cue : fixSeg >= 0 && !fixWhy.empty() ? fixWhy : overallWhy;
				if (!what.empty()) what[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(what[0])));
				headline(what.empty() ? "No breakdown for this rep" : what);
				if (!notes.empty())
				{
					std::string why = rootSeg >= 0 ? std::string("The root cause, in the ") + Segmenter::Name(segs[rootSeg].kind) + "."
						: notes[0].label.empty() ? std::string() : notes[0].label.substr(0, notes[0].label.find(':')) + ".";
					if (notes.size() > 1) why += "  Also: " + notes[1].cue;
					subline(why);
					if (ImGui::IsItemHovered() && !notes[0].detail.empty()) ImGui::SetTooltip("%s", notes[0].detail.c_str());
				}
				ImGui::Dummy(ImVec2(0, 4));
				tiles(stats);
				ImGui::Dummy(ImVec2(0, 4));
				if (AccentButton("Watch your rep", btn)) onGame([this]() { ViewerShowLine(-1, false); });
				ImGui::SameLine(0, 10);
				const int practiceSeg = rootSeg >= 0 ? rootSeg : fixSeg;
				if (practiceSeg >= 0 && practiceSeg < static_cast<int>(segs.size()))
				{
					if (OutlineButton(rootSeg >= 0 ? "Practice the root cause" : "Practice the biggest fix", btn, kRoot))
						onGame([this, practiceSeg]() { PracticeStart(practiceSeg); });
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drops you into %s in free play, as many tries as you want.", Segmenter::Name(segs[practiceSeg].kind));
				}
				else if (OutlineButton(std::format("Practice from here ({:.2f}s)", head / 120.0f).c_str(), btn))
					onGame([this, head]() { PracticeStartAt(head); });
			}
			else
			{
				const int slot = uiTab - 1;
				const LineView& v = line[slot];
				const bool fixTab = slot == kFixLine;
				if (!simReady)
					emptyState(0, "Loading the simulator...", "The searches start as soon as it's ready.");
				else if (busySlot == slot || ((pending || busy) && (!v.mine || v.status == WhatIfStatus::None)))
				{
					std::string t = busySlot == slot
						? (fixTab ? (whatIfRunningPoint == static_cast<int>(WhatIfPoint::KeyMoment) ? "Trying different ways into " + keyWhat + "..." : "Trying small changes to your shot...")
							: (whatIfDepth >= 2 ? "Thinking harder..." : "Thinking..."))
						: "Waiting for the smallest fix to finish...";
					emptyState(0, t, fixTab ? "A few seconds. You can keep watching your rep meanwhile."
						: whatIfDepth >= 2 ? "Up to a minute, in the background." : "Up to about 15 seconds, in the background.");
				}
				else if (!v.mine || v.status == WhatIfStatus::None)
				{
					emptyState(1, "Not searched yet", "");
					if (AccentButton(fixTab ? "Find the smallest fix" : "Find the best line", btn))
						onGame([this, fixTab]() { StartWhatIf(1, fixTab ? WhatIfPoint::Shot : WhatIfPoint::Engine, -1); });
				}
				else if (v.status == WhatIfStatus::NotNeeded)
					emptyState(2, "Nothing to beat. Clean rep.", overallWhy + ". Keep it to set the bar for this shot.");
				else if (v.status == WhatIfStatus::Found || v.status == WhatIfStatus::CleanerWayIn)
				{
					if (v.status == WhatIfStatus::CleanerWayIn)
					{
						label(("A CLEANER WAY INTO " + Upper(v.what)).c_str(), kMuted);
						headline(v.cue);
						subline("No goal from a different way in, but it fixes what the coach flagged: " + v.fixes);
					}
					else
					{
						label(fixTab ? (v.point == WhatIfPoint::KeyMoment ? ("A DIFFERENT WAY INTO " + Upper(v.what)).c_str() : "THE LEAST YOU COULD CHANGE")
							: "THE ENGINE'S LINE", kMuted);
						headline(v.cue);
						subline(std::format("{}. It scored from {} of {} slightly different starts.", v.gradeWhy, v.held, v.heldOf));
					}
					if (ImGui::IsItemHovered() && !v.detail.empty()) ImGui::SetTooltip("%s", v.detail.c_str());
					ImGui::Dummy(ImVec2(0, 4));
					std::vector<ReviewStat> ts;
					ts.push_back({ fixTab ? "Starts at" : "Takes over", std::format("{:.2f} s", v.startTick / 120.0f), segName(v.startTick) });
					if (v.status == WhatIfStatus::Found)
						ts.push_back({ "Grade", std::to_string(v.grade), std::format("you: {}", overallGrade) });
					ts.push_back({ "Held up", std::format("{}/{}", v.held, v.heldOf), "nudged starts" });
					tiles(ts);
					ImGui::Dummy(ImVec2(0, 4));
					if (AccentButton("Watch it", btn)) onGame([this, slot]() { ViewerShowLine(slot, false); });
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Plays it with your car from just before it changes. Your real path shows in grey while it plays.");
					ImGui::SameLine(0, 10);
					if (OutlineButton("Practice it", btn)) onGame([this, slot]() { PracticeLine(slot); });
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Practice from where it takes over, with its path drawn faintly to follow.");
				}
				else
				{
					// Searched, nothing to show
					if (v.status == WhatIfStatus::NothingBetter)
						emptyState(1, fixTab ? "No small fix found" : "No better line found",
							fixTab ? "Small changes to your inputs didn't beat your shot. The best line tab doesn't stick to your inputs."
								: "Your shot was the best it could find from here.");
					else
						emptyState(1, WhatIf::StatusText(v.status), "");
					if (fixTab && rootSeg >= 0)
						subline("The coach's call, in the " + std::string(Segmenter::Name(segs[rootSeg].kind)) + ": " + notes[0].cue);
				}

				// Small extras: search more, or have the engine take over from the playhead
				if (simReady && !busy && v.mine && v.status != WhatIfStatus::None && v.status != WhatIfStatus::NotNeeded)
				{
					ImGui::Dummy(ImVec2(0, 2));
					if (fixTab)
					{
						if (v.depth < 2 && v.status != WhatIfStatus::Found)
						{
							const WhatIfPoint pt = v.point == WhatIfPoint::KeyMoment ? WhatIfPoint::KeyMoment : WhatIfPoint::Shot;
							if (LinkButton("Search deeper")) onGame([this, pt]() { StartWhatIf(2, pt); });
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("About 10x more tries: wider timing and aim, finer angles, and up to three changes together.\nUp to about 15 seconds.");
							ImGui::SameLine();
						}
					}
					else
					{
						if (v.depth < 2)
						{
							const int es = whatIfEngineStart;
							if (LinkButton("Think harder")) onGame([this, es]() { StartWhatIf(2, WhatIfPoint::Engine, es); });
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("About 5x more tries. Up to a minute in the background.");
							ImGui::SameLine();
						}
						if (LinkButton(std::format("Take over from here ({:.2f}s)", head / 120.0f).c_str()))
							onGame([this, head]() { StartWhatIf(1, WhatIfPoint::Engine, head); });
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("The engine takes over your car at the moment you've scrubbed to,\nfor something entirely different (another setup, another approach).");
						ImGui::SameLine();
					}
					if (!v.note.empty())
					{
						ImGui::TextDisabled("How it searched");
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", v.note.c_str());
					}
					else ImGui::NewLine();
				}
			}

			ImGui::PopTextWrapPos();
			ImGui::EndGroup();
			const float bottom = std::max(ImGui::GetItemRectMax().y, p0.y + 200.0f) + pad;
			dl->ChannelsSetCurrent(0);
			dl->AddRectFilled(p0, ImVec2(p0.x + W, bottom), U32(kPanel), 12.0f, ImDrawCornerFlags_Bot);
			dl->AddRect(p0, ImVec2(p0.x + W, bottom), U32(kLine), 12.0f, ImDrawCornerFlags_Bot);
			{
				// the open tab's top edge carries the accent
				const float tabW = (W - 16.0f) / 3.0f, ax = p0.x + uiTab * (tabW + 8.0f);
				dl->AddLine(ImVec2(p0.x + 1, p0.y), ImVec2(p0.x + W - 1, p0.y), U32(kLine));
				dl->AddRectFilled(ImVec2(ax + 1, p0.y - 1), ImVec2(ax + tabW - 1, p0.y + 1), U32(kPanel));
				dl->AddLine(ImVec2(ax + 12, p0.y - 60.0f + 1), ImVec2(ax + tabW - 12, p0.y - 60.0f + 1), U32(kAccent), 2.0f);
			}
			dl->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(p0.x, bottom + 14.0f));
		}

		// ---------- What went well / where it went wrong (the full breakdown, folded away) ----------
		{
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + W, a.y + 40.0f);
			if (ImGui::InvisibleButton("details", ImVec2(W, 40.0f))) uiDetailsOpen = !uiDetailsOpen;
			const bool hov = ImGui::IsItemHovered();
			dl->AddRectFilled(a, b, U32(hov ? kPanel : kPanel2), 10.0f);
			dl->AddRect(a, b, U32(kLineSoft), 10.0f);
			{
				FontScope f(fLabel);
				const char* t = "WHAT WENT WELL  \xC2\xB7  WHERE IT WENT WRONG";
				const ImVec2 ts = ImGui::CalcTextSize(t);
				dl->AddText(ImVec2(a.x + 16, a.y + (40.0f - ts.y) * 0.5f), U32(kSub), t);
			}
			{
				const ImVec2 c(b.x - 22.0f, a.y + 20.0f);
				const ImU32 col = U32(kSub);
				if (uiDetailsOpen) { dl->AddLine(ImVec2(c.x - 5, c.y + 3), ImVec2(c.x, c.y - 2), col, 2.0f); dl->AddLine(ImVec2(c.x, c.y - 2), ImVec2(c.x + 5, c.y + 3), col, 2.0f); }
				else { dl->AddLine(ImVec2(c.x - 5, c.y - 2), ImVec2(c.x, c.y + 3), col, 2.0f); dl->AddLine(ImVec2(c.x, c.y + 3), ImVec2(c.x + 5, c.y - 2), col, 2.0f); }
			}
		}
		if (uiDetailsOpen)
		{
			ImGui::Spacing();
			// v2 coaching: the root cause first, then one more; hover for the numbers behind it
			for (const CoachNote& n : notes)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, n.rootCause ? Hex(0xFDBA74) : kSub);
				ImGui::TextWrapped("%s %s", n.label.c_str(), n.cue.c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered() && !n.detail.empty()) ImGui::SetTooltip("%s", n.detail.c_str());
			}
			// Each graded part: practice it, its grade, the why. Click a line to jump there.
			auto practiceButton = [&](int si) {
				const bool root = si == rootSeg;
				ImGui::PushID(si + 2000);
				const bool clicked = root ? OutlineButton("Practice", ImVec2(84, 0), kRoot) : ImGui::Button("Practice", ImVec2(84, 0));
				ImGui::PopID();
				if (clicked) onGame([this, si]() { PracticeStart(si); });
				if (root && ImGui::IsItemHovered() && !notes.empty()) ImGui::SetTooltip("The root cause is in this part: %s", notes[0].cue.c_str());
			};
			bool rootListed = false;
			for (const UiGrade& ug : grades) if (ug.segIndex == rootSeg && !ug.name.empty()) rootListed = true;
			if (rootSeg >= 0 && !rootListed)
			{
				practiceButton(rootSeg);
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(Hex(0xFDBA74), "%s: root cause is here", Segmenter::Name(segs[rootSeg].kind));
			}
			for (const UiGrade& ug : grades)
			{
				if (ug.name.empty() || ug.segIndex < 0 || ug.segIndex >= static_cast<int>(segs.size())) continue;
				const Segment& sg = segs[ug.segIndex];
				const int si = ug.segIndex;
				practiceButton(si);
				ImGui::SameLine();
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight() - 4.0f) * 0.5f);
				Pill(ug.name, SegGradeColor(ug.name), 84.0f);
				ImGui::SameLine();
				if (ug.biggest) { FontScope f(fLabel); ImGui::AlignTextToFramePadding(); ImGui::TextColored(kWarn, "FIX"); ImGui::SameLine(); }
				std::string text = std::format("{}: {}", Segmenter::Name(sg.kind), ug.why);
				ImGui::PushID(si + 1000);
				if (ImGui::Selectable(text.c_str(), false))
				{
					const float target = static_cast<float>(sg.start);
					onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
				}
				ImGui::PopID();
			}
			ImGui::Spacing();
			if (OutlineButton(std::format("Practice from here ({:.2f}s)", head / 120.0f).c_str()))
				onGame([this, head]() { PracticeStartAt(head); });
			ImGui::SameLine();
			ImGui::TextDisabled("scrub to pick the moment; the lead-in still applies");

			if (cur)
			{
				// Your opinion of the recovery (same as keys 6-8 in the pack)
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Your recovery rating:");
				const int curRec = cur->recoveryRating;
				const std::string recFile = watching;
				for (int r = 1; r <= 3; r++)
				{
					ImGui::SameLine();
					const bool on = curRec == r;
					if (on)
					{
						unsigned char cr, cg, cb;
						RecoveryRgb(r, cr, cg, cb);
						ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(cr / 255.0f * 0.55f, cg / 255.0f * 0.55f, cb / 255.0f * 0.55f, 1.0f));
					}
					if (ImGui::SmallButton(RecoveryName(r))) SetRecoveryRating(recFile, r);
					if (on) ImGui::PopStyleColor();
				}
				// Mark the whole shot as a double-tap drill: a goal without a double tap (or triple, quad...) counts as a 1
				bool drill = shotBook.IsDoubleTapDrill(cur->shotKey);
				ImGui::SameLine(0, 18);
				if (ImGui::Checkbox("Double-tap drill", &drill) && writer)
				{
					shotBook.SetDoubleTapDrill(writer->Folder(), cur->shotKey, drill);
					gradeBook.Invalidate(cur->shotKey);
					StartGradeRefresh();
					RepListItem again = *cur;
					onGame([this, again]() { ViewerLoad(again); });   // redo this rep's breakdown now
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("For this shot, a goal that isn't a double tap counts as a 1.\nBackboard, ceiling and wall-top taps all count, and so do triple and quad taps.");
			}
			{
				int th = *targetHitbox;
				const char* thNames[3] = { "Off", "For your touch", "For the earliest intercept" };
				ImGui::PushItemWidth(220);
				if (ImGui::Combo("Target car outline", &th, thNames, 3))
				{
					const float v = static_cast<float>(th);
					onGame([this, v]() { cvarManager->getCvar("rlcoach_target_hitbox").setValue(v); });
				}
				ImGui::PopItemWidth();
			}
			if (ImGui::TreeNode("What the lines mean"))
			{
				ImGui::TextColored(Hex(0xFFA028), "Orange");                     ImGui::SameLine(); ImGui::TextDisabled("ball path");
				ImGui::TextColored(Hex(0x5ABEFF), "Blue");                       ImGui::SameLine(); ImGui::TextDisabled("car path (bright = already played)");
				ImGui::TextColored(kGold, "Yellow ring");                        ImGui::SameLine(); ImGui::TextDisabled("where you needed to hit the ball; yellow line = where it needed to go");
				ImGui::TextColored(kGood, "Green ring");                         ImGui::SameLine(); ImGui::TextDisabled("where you hit it; green line = where it went");
				ImGui::TextColored(kGold, "Yellow ghost ball");                  ImGui::SameLine(); ImGui::TextDisabled("earliest a fast, direct aerial could have met it");
				ImGui::TextColored(kGold, "Yellow box");                         ImGui::SameLine(); ImGui::TextDisabled("your hitbox placed and aimed to make that ideal hit");
				ImGui::TextColored(kGood, "Green box");                          ImGui::SameLine(); ImGui::TextDisabled("your real hitbox at contact");
				ImGui::TextColored(kAccent, "Cyan");                             ImGui::SameLine(); ImGui::TextDisabled("the line from the tab you last opened (lighter: its ball); grey = your real path while a line plays");
				ImGui::TextColored(ImVec4(1, 1, 1, 0.8f), "White dashes");       ImGui::SameLine(); ImGui::TextDisabled("predicted ball path");
				ImGui::TreePop();
			}
		}
	}
	else
	{
		ImGui::Spacing();
		ImGui::TextDisabled("Double-click a rep to watch it. Playback happens in free play; recording pauses while you watch.");
	}

	// ---- Rep list ----
	Heading("REPS");
	if (gradeThreadBusy) { ImGui::SameLine(); ImGui::TextDisabled("grading..."); }

	// Hide reps with no grade at all (no rating from you, no auto grade): bailed and reset reps, mostly
	{
		bool showAll = *showUngraded;
		ImGui::SameLine();
		if (ImGui::Checkbox("Show ungraded", &showAll))
		{
			float v = showAll ? 1.0f : 0.0f;
			gameWrapper->Execute([this, v](GameWrapper*) { cvarManager->getCvar("rlcoach_viewer_show_ungraded").setValue(v); });
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reps you didn't rate that also have no auto grade (bailed or reset).\nThey stay saved; this only hides them from the list.");
	}
	// Which reps: today (default), the last 7 days, starred, or everything
	{
		int range = *viewerRange;
		const char* ranges[4] = { "Today", "Last 7 days", "Starred", "All" };
		ImGui::SameLine();
		ImGui::PushItemWidth(120);
		if (ImGui::Combo("##range", &range, ranges, 4))
		{
			float v = static_cast<float>(range);
			gameWrapper->Execute([this, v](GameWrapper*) { cvarManager->getCvar("rlcoach_viewer_range").setValue(v); });
		}
		ImGui::PopItemWidth();
	}
	auto dayString = [](int daysAgo) {
		std::time_t t = std::time(nullptr) - static_cast<std::time_t>(daysAgo) * 86400;
		std::tm local{};
		localtime_s(&local, &t);
		char buf[16];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
		return std::string(buf);
	};
	const std::string today = dayString(0), weekStart = dayString(6);
	const int range = *viewerRange;

	std::vector<int> shown;
	shown.reserve(repList.size());
	int hiddenUngraded = 0;
	for (int i = 0; i < static_cast<int>(repList.size()); i++)
	{
		const RepListItem& it = repList[i];
		bool watching = it.file == watchingFile;
		std::string day = it.savedAt.substr(0, 10);
		bool inRange = range == 3 || (range == 2 && it.kept) || (range == 1 && day >= weekStart) || (range == 0 && day == today);
		if (!inRange && !watching) continue;
		auto ag = uiAutoGrades.find(it.file);
		bool graded = it.rating > 0 || (ag != uiAutoGrades.end() && ag->second.overall > 0);
		bool pending = ag == uiAutoGrades.end() && it.hasRecording && it.endReason != "reset";   // still being graded
		if (*showUngraded || graded || pending || watching) shown.push_back(i);
		else hiddenUngraded++;
	}
	ImGui::SameLine();
	if (hiddenUngraded > 0) ImGui::TextDisabled("%d shown, %d ungraded hidden", static_cast<int>(shown.size()), hiddenUngraded);
	else ImGui::TextDisabled("%d shown", static_cast<int>(shown.size()));
	if (shown.empty())
		ImGui::TextDisabled(range == 0 ? "No reps today yet. Switch to \"Last 7 days\" or \"All\" to see older ones."
			: range == 2 ? "No starred reps. Star a rep to keep it forever and find it here." : "No reps to show.");

	ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel2);
	ImGui::BeginChild("replist", ImVec2(0, uiViewerActive ? 220.0f : 380.0f), true);
	ImGui::PopStyleColor();
	ImGui::Columns(7, "repcols", false);
	ImGui::SetColumnWidth(0, 34); ImGui::SetColumnWidth(1, 170); ImGui::SetColumnWidth(2, 70); ImGui::SetColumnWidth(3, 64);
	ImGui::SetColumnWidth(4, 68); ImGui::SetColumnWidth(5, 70);
	const char* heads[7] = { "", "WHEN", "ENDED", "LENGTH", "TOUCHES", "YOU", "AUTO" };
	{
		FontScope f(fLabel);
		for (const char* h : heads) { ImGui::TextColored(kMuted, "%s", h); ImGui::NextColumn(); }
	}
	ImGui::Separator();
	std::string toggleKeep;
	// Only draw the rows that are on screen. Drawing all of them (1,500+) overflows ImGui's vertex buffer,
	// which shows up as smeared, garbled shapes. Every row is one fixed height so the clipper can skip the rest.
	const float rowH = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
	ImGuiListClipper clipper(static_cast<int>(shown.size()), rowH);
	while (clipper.Step())
	for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
	{
		const int i = shown[row];   // index into repList
		const RepListItem& it = repList[i];
		float rowTop = ImGui::GetCursorPosY();
		bool isWatching = uiViewerActive && it.file == watchingFile;

		// Keep star (only reps that still have a recording can be kept)
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			float h = ImGui::GetTextLineHeight();
			ImGui::PushID(i);
			if (it.hasRecording && ImGui::InvisibleButton("keep", ImVec2(h + 4, h))) toggleKeep = it.file;
			else if (!it.hasRecording) ImGui::Dummy(ImVec2(h + 4, h));
			bool hover = it.hasRecording && ImGui::IsItemHovered();
			ImGui::PopID();
			ImU32 col = it.kept ? IM_COL32(251, 191, 36, 255) : (hover ? U32(kText) : U32(kMuted, 0.6f));
			if (it.hasRecording || it.kept)
				DrawStar(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.5f + 2, p.y + h * 0.5f), h * 0.42f, it.kept, col);
			if (hover) ImGui::SetTooltip(it.kept ? "Kept forever. Click to un-keep." : "Keep this rep forever");
		}
		ImGui::NextColumn();

		std::string when = it.savedAt.size() >= 19 ? it.savedAt.substr(5, 11) : it.savedAt;
		std::string rowLabel = std::format("{}##row{}", when, i);
		if (!it.hasRecording) ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		if (ImGui::Selectable(rowLabel.c_str(), selectedRep == i || isWatching, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
		{
			selectedRep = i;
			if (ImGui::IsMouseDoubleClicked(0) && it.hasRecording)
			{
				RepListItem pick = it;
				onGame([this, pick]() { ViewerLoad(pick); });
			}
		}
		if (!it.hasRecording && ImGui::IsItemHovered())
			ImGui::SetTooltip(it.endReason == "reset" ? "Bailed rep: only the summary is saved"
				: "Recording cleaned up: summary and grades are kept");
		if (it.imported)
		{
			ImGui::SameLine();
			Pill(it.importedFrom, Hex(0xC4B5FD));
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shared with you (imported from %s)", it.importedFrom.c_str());
		}
		ImGui::NextColumn();
		ImGui::TextUnformatted(it.endReason.c_str()); ImGui::NextColumn();
		ImGui::Text("%.1fs", it.duration); ImGui::NextColumn();
		ImGui::Text("%d", it.touches); ImGui::NextColumn();
		if (!it.hasRecording) ImGui::PopStyleColor();
		if (it.rating > 0) Pill(std::to_string(it.rating), GradeColor(it.rating), 26.0f);
		else ImGui::TextDisabled("-");
		ImGui::NextColumn();
		auto ag = uiAutoGrades.find(it.file);
		if (ag == uiAutoGrades.end()) ImGui::TextDisabled(it.hasRecording ? "..." : "-");
		else if (ag->second.overall <= 0) ImGui::TextDisabled("-");
		else
		{
			Pill(std::to_string(ag->second.overall), GradeColor(ag->second.overall), 26.0f);
			static const char* kinds[6] = { "", "ground", "dbl tap", "dribble", "air drib", "reset" };
			const int st = ag->second.shotType;
			if (st >= 1 && st <= 5) { ImGui::SameLine(); ImGui::TextDisabled("%s", kinds[st]); }
		}
		ImGui::NextColumn();
		if (ImGui::GetCursorPosY() < rowTop + rowH) ImGui::SetCursorPosY(rowTop + rowH);   // keep every row the same height
	}
	ImGui::Columns(1);
	ImGui::EndChild();
	if (!toggleKeep.empty()) ToggleKeep(toggleKeep);

	bool canWatchSelected = selectedRep >= 0 && selectedRep < static_cast<int>(repList.size()) && repList[selectedRep].hasRecording;
	if (canWatchSelected && AccentButton("Watch selected", ImVec2(150, 34)))
	{
		RepListItem pick = repList[selectedRep];
		onGame([this, pick]() { ViewerLoad(pick); });
	}
	const RepListItem* latest = nullptr;
	for (int i : shown) if (repList[i].hasRecording) { latest = &repList[i]; break; }
	if (latest)
	{
		if (canWatchSelected) ImGui::SameLine();
		if (OutlineButton("Watch latest", ImVec2(150, 34)))
		{
			RepListItem pick = *latest;
			onGame([this, pick]() { ViewerLoad(pick); });
		}
	}
}
