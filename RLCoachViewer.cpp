#include "pch.h"
#include "RLCoach.h"
#include "Ratings.h"
#include "bakkesmod/wrappers/includes.h"
#include "Overlay3D.h"
#include "UiStyle.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <functional>

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

// Older reps (before the index stored it) have no event tick; the segmenter then uses the last frame
static int ViewerEventTick(const RepListItem& item) { return item.eventTick; }

void RLCoach::ViewerLoad(const RepListItem& item)
{
	if (!writer) return;
	PracticeExit();   // opening a rep ends any practice set
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
}

void RLCoach::ViewerExit()
{
	PracticeExit();
	if (!viewerActive) return;
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

void RLCoach::RenderWindow()
{
	using namespace UiStyle;
	if (!repListLoaded || repListStale.exchange(false)) RefreshRepList();

	auto onGame = [this](std::function<void()> fn) {
		gameWrapper->Execute([fn](GameWrapper*) { fn(); });
	};

	// ---- Title row ----
	ImGui::TextColored(kAccent, "RL COACH");
	ImGui::SameLine();
	ImGui::TextDisabled("rep viewer");
	{
		std::string info = std::format("{} reps", repList.size());
		float w = ImGui::CalcTextSize(info.c_str()).x + ImGui::CalcTextSize("Refresh").x + 40.0f;
		ImGui::SameLine(std::max(200.0f, ImGui::GetWindowContentRegionMax().x - w));
		ImGui::TextDisabled("%s", info.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Refresh")) RefreshRepList();
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
		if (AccentButton("Retry")) onGame([this]() { PracticeRetry(); });
		ImGui::SameLine();
		if (ImGui::Button("Stop practicing")) onGame([this]() { PracticeExit(); });
		ImGui::SameLine();
		ImGui::TextDisabled("results show on the in-game panel");
	}

	// ---- The rep being watched ----
	if (uiViewerActive)
	{
		int total = uiViewerTotal;
		int scrub = static_cast<int>(uiViewerPlayhead.load());
		std::vector<Segment> segs;
		std::vector<UiGrade> grades;
		std::vector<CoachNote> notes;
		std::string overall, watching;
		int segTotal = 0;
		{
			std::lock_guard<std::mutex> lock(uiSegMutex);
			segs = uiSegs;
			segTotal = uiSegTotal;
			grades = uiGrades;
			notes = uiNotes;
			overall = uiOverall;
			watching = uiViewerFile;
		}
		const RepListItem* cur = nullptr;
		for (const RepListItem& it : repList) if (it.file == watching) { cur = &it; break; }

		Heading("WATCHING");
		if (cur)
		{
			std::string when = cur->savedAt.size() >= 19 ? cur->savedAt.substr(5, 11) : cur->savedAt;
			ImGui::Text("%s", when.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s   %.1fs   %d touch%s", cur->endReason.c_str(), cur->duration, cur->touches, cur->touches == 1 ? "" : "es");
			ImGui::SameLine();
			auto ag = uiAutoGrades.find(cur->file);
			if (ag != uiAutoGrades.end() && ag->second.overall > 0) Pill(std::format("auto {}", ag->second.overall), GradeColor(ag->second.overall));
			if (ag != uiAutoGrades.end() && ag->second.shotType == 1) { ImGui::SameLine(); Pill("ground shot", kOk); }
			if (ag != uiAutoGrades.end() && ag->second.shotType == 2) { ImGui::SameLine(); Pill("double tap", kGold); }
			if (ag != uiAutoGrades.end() && ag->second.shotType == 3) { ImGui::SameLine(); Pill("dribble", kOk); }
			if (ag != uiAutoGrades.end() && ag->second.shotType == 4) { ImGui::SameLine(); Pill("air dribble", kGold); }
			if (ag != uiAutoGrades.end() && ag->second.shotType == 5) { ImGui::SameLine(); Pill("flip reset", kGold); }
			if (cur->rating > 0) { ImGui::SameLine(); Pill(std::format("you {}", cur->rating), GradeColor(cur->rating)); }
			ImGui::SameLine();
			bool kept = cur->kept;
			if (kept) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.36f, 0.08f, 1.0f));
			if (ImGui::SmallButton(kept ? "Kept" : "Keep")) ToggleKeep(watching);
			if (kept) ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(kept ? "Kept forever. Click to un-keep." : "Keep this rep forever (never cleaned up)");

			// Mark the whole shot as a double-tap drill: a goal without a double tap (or triple, quad...) counts as a 1
			bool drill = shotBook.IsDoubleTapDrill(cur->shotKey);
			ImGui::SameLine();
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

			// Your opinion of the recovery (same as keys 6-8 in the pack)
			ImGui::TextDisabled("Your recovery rating:");
			const int curRec = cur->recoveryRating;
			const std::string recFile = watching;
			for (int r = 1; r <= 3; r++)
			{
				ImGui::SameLine();
				bool on = curRec == r;
				if (on)
				{
					unsigned char cr, cg, cb;
					RecoveryRgb(r, cr, cg, cb);
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(cr / 255.0f * 0.55f, cg / 255.0f * 0.55f, cb / 255.0f * 0.55f, 1.0f));
				}
				if (ImGui::SmallButton(RecoveryName(r))) SetRecoveryRating(recFile, r);
				if (on) ImGui::PopStyleColor();
			}
		}
		if (!overall.empty())
		{
			// overall is "4: Good shot  (why...)"
			size_t colon = overall.find(':');
			int o = colon != std::string::npos ? std::atoi(overall.substr(0, colon).c_str()) : 0;
			ImGui::PushStyleColor(ImGuiCol_Text, GradeColor(o));
			ImGui::TextWrapped("Grade %s", overall.c_str());
			ImGui::PopStyleColor();
		}
		// v2 coaching: the root cause first, then one more; hover for the numbers behind it
		for (const CoachNote& n : notes)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, n.rootCause ? ImVec4(1.0f, 0.80f, 0.50f, 1.0f) : ImVec4(0.85f, 0.78f, 0.62f, 1.0f));
			ImGui::TextWrapped("%s %s", n.label.c_str(), n.cue.c_str());
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && !n.detail.empty()) ImGui::SetTooltip("%s", n.detail.c_str());
		}

		// Transport
		bool playing = uiViewerPlaying;
		if (AccentButton(playing ? "Pause" : "Play", ImVec2(64, 0)))
			onGame([this]() { viewerPlaying = !viewerPlaying; uiViewerPlaying = viewerPlaying; });
		ImGui::SameLine();
		if (ImGui::Button("|<")) onGame([this]() { ViewerSeek(0.0f); });
		ImGui::SameLine();
		if (ImGui::Button("<")) onGame([this]() { viewerPlaying = false; ViewerSeek(std::floor(viewerPlayhead) - 1.0f); });
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back 1 tick");
		ImGui::SameLine();
		if (ImGui::Button(">")) onGame([this]() { viewerPlaying = false; ViewerSeek(std::floor(viewerPlayhead) + 1.0f); });
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward 1 tick");
		ImGui::SameLine(0, 18);
		const float speeds[4] = { 0.1f, 0.25f, 0.5f, 1.0f };
		const char* names[4] = { "0.1x", "0.25x", "0.5x", "1x" };
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 6));
		for (int i = 0; i < 4; i++)
		{
			if (i > 0) ImGui::SameLine();
			bool current = std::fabs(uiViewerSpeed.load() - speeds[i]) < 0.001f;
			if (current) ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
			if (ImGui::Button(names[i]))
			{
				float sp = speeds[i];
				uiViewerSpeed = sp;
				onGame([this, sp]() { viewerSpeed = sp; });
			}
			if (current) ImGui::PopStyleColor();
		}
		ImGui::PopStyleVar();
		ImGui::SameLine(0, 18);
		if (ImGui::Checkbox("Loop", &uiLoop))
		{
			bool loop = uiLoop;
			onGame([this, loop]() { viewerLoop = loop; });
		}
		ImGui::SameLine();
		if (ImGui::Button("Stop watching")) onGame([this]() { ViewerExit(); });

		// Segment timeline: click a segment to jump to it
		if (!segs.empty() && segTotal > 1)
		{
			ImGui::Spacing();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 origin = ImGui::GetCursorScreenPos();
			float width = ImGui::GetContentRegionAvail().x;
			const float rowH = 24.0f, gap = 4.0f;
			auto colorFor = [](SegKind k, int alpha) -> ImU32 {
				switch (k)
				{
				case SegKind::Approach: return IM_COL32(70, 86, 112, alpha);
				case SegKind::Takeoff:  return IM_COL32(52, 112, 170, alpha);
				case SegKind::Flight:   return IM_COL32(44, 92, 156, alpha);
				case SegKind::Contact:  return IM_COL32(46, 150, 92, alpha);
				case SegKind::Result:   return IM_COL32(170, 116, 40, alpha);
				case SegKind::Recovery: return IM_COL32(120, 82, 160, alpha);
				case SegKind::Setup:    return IM_COL32(40, 130, 140, alpha);
				case SegKind::Read:     return IM_COL32(150, 100, 60, alpha);
				case SegKind::Dribble:  return IM_COL32(60, 140, 120, alpha);
				case SegKind::Flick:    return IM_COL32(46, 150, 92, alpha);
				case SegKind::AirDribble: return IM_COL32(60, 110, 170, alpha);
				case SegKind::Reset:    return IM_COL32(140, 90, 150, alpha);
				case SegKind::Flip:     return IM_COL32(52, 112, 170, alpha);
				}
				return IM_COL32(100, 100, 100, alpha);
			};
			auto xOf = [&](int tick) { return origin.x + width * static_cast<float>(tick) / static_cast<float>(segTotal - 1); };
			dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + 2 * rowH + gap), IM_COL32(22, 25, 32, 255), 5.0f);

			// Row 1: the play (approach to result). Row 2: recovery, which overlaps the result in time.
			for (int si = 0; si < static_cast<int>(segs.size()); si++)
			{
				const Segment& sg = segs[si];
				float y0 = origin.y + (sg.kind == SegKind::Recovery ? rowH + gap : 0.0f);
				float x0 = xOf(sg.start) + 1.0f, x1 = std::max(xOf(sg.end + 1) - 1.0f, x0 + 3.0f);
				std::string gradeName, why;
				for (const UiGrade& ug : grades) if (ug.segIndex == si) { gradeName = ug.name; why = ug.why; }

				ImGui::SetCursorScreenPos(ImVec2(x0, y0));
				ImGui::PushID(si);   // stable ID so a click's press and release land on the same button
				bool clicked = ImGui::InvisibleButton("seg", ImVec2(x1 - x0, rowH));
				bool hovered = ImGui::IsItemHovered();
				ImGui::PopID();

				dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + rowH), colorFor(sg.kind, hovered ? 255 : 215), 4.0f);
				if (!gradeName.empty())   // grade stripe along the bottom
					dl->AddRectFilled(ImVec2(x0 + 2, y0 + rowH - 4), ImVec2(x1 - 2, y0 + rowH - 1), ImGui::GetColorU32(SegGradeColor(gradeName)), 2.0f);
				std::string label = Segmenter::Name(sg.kind);
				ImVec2 ts = ImGui::CalcTextSize(label.c_str());
				if (x1 - x0 > ts.x + 8.0f)
					dl->AddText(ImVec2(x0 + 5.0f, y0 + (rowH - 3 - ts.y) * 0.5f), IM_COL32(240, 244, 250, 255), label.c_str());

				if (clicked)
				{
					float target = static_cast<float>(sg.start);
					onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
				}
				if (hovered)
					ImGui::SetTooltip("%s%s%s: %.2fs to %.2fs (click to jump here)%s%s", label.c_str(), gradeName.empty() ? "" : " - ",
						gradeName.c_str(), sg.start / 120.0f, (sg.end + 1) / 120.0f, why.empty() ? "" : "\n", why.c_str());
			}
			// Playhead
			float px = xOf(static_cast<int>(uiViewerPlayhead.load()));
			dl->AddLine(ImVec2(px, origin.y - 3.0f), ImVec2(px, origin.y + 2 * rowH + gap + 3.0f), IM_COL32(255, 255, 255, 235), 2.0f);
			dl->AddTriangleFilled(ImVec2(px - 5, origin.y - 7), ImVec2(px + 5, origin.y - 7), ImVec2(px, origin.y - 1), IM_COL32(255, 255, 255, 235));
			ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 2 * rowH + gap + 8.0f));
		}

		ImGui::PushItemWidth(-1);
		if (ImGui::SliderInt("##scrub", &scrub, 0, total > 0 ? total - 1 : 0,
			std::format("{:.2f}s / {:.2f}s", scrub / 120.0f, total / 120.0f).c_str()))
		{
			float target = static_cast<float>(scrub);
			onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
		}
		ImGui::PopItemWidth();

		// Breakdown: each segment's grade; the biggest fix is marked. Click a line to jump there, or practice it.
		Heading("BREAKDOWN");
		{
			int here = static_cast<int>(uiViewerPlayhead.load());
			if (AccentButton(std::format("Practice from here ({:.2f}s)", here / 120.0f).c_str()))
				onGame([this, here]() { PracticeStartAt(here); });
			ImGui::SameLine();
			ImGui::TextDisabled("scrub to pick the moment; the lead-in still applies");
		}
		// The part of the rep the coach's root cause is in: its Practice button is highlighted
		int rootSeg = -1;
		if (!notes.empty() && notes[0].rootCause)
			for (int i = 0; i < static_cast<int>(segs.size()); i++)
				if (segs[i].kind == notes[0].seg) rootSeg = i;
		bool rootListed = false;
		for (const UiGrade& ug : grades) if (ug.segIndex == rootSeg && !ug.name.empty()) rootListed = true;
		auto practiceButton = [&](int si) {
			bool root = si == rootSeg;
			if (root)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.42f, 0.10f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.55f, 0.15f, 1.0f));
			}
			ImGui::PushID(si + 2000);
			if (ImGui::SmallButton("Practice")) onGame([this, si]() { PracticeStart(si); });
			ImGui::PopID();
			if (root)
			{
				ImGui::PopStyleColor(2);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("The root cause is in this part: %s", notes[0].cue.c_str());
			}
		};
		// Root cause in a part that has no grade line (e.g. the pop before a flip reset): give it its own line
		if (rootSeg >= 0 && !rootListed)
		{
			practiceButton(rootSeg);
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.50f, 1.0f), "%s: root cause is here", Segmenter::Name(segs[rootSeg].kind));
		}
		for (const UiGrade& ug : grades)
		{
			if (ug.name.empty() || ug.segIndex < 0 || ug.segIndex >= static_cast<int>(segs.size())) continue;
			const Segment& sg = segs[ug.segIndex];
			int si = ug.segIndex;
			practiceButton(si);
			ImGui::SameLine();
			Pill(ug.name, SegGradeColor(ug.name), 84.0f);
			ImGui::SameLine();
			if (ug.biggest) { ImGui::TextColored(kGold, "FIX"); ImGui::SameLine(); }
			std::string line = std::format("{}: {}", Segmenter::Name(sg.kind), ug.why);
			ImGui::PushID(si + 1000);
			if (ImGui::Selectable(line.c_str(), false))
			{
				float target = static_cast<float>(sg.start);
				onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
			}
			ImGui::PopID();
		}

		{
			int th = *targetHitbox;
			const char* thNames[3] = { "Off", "For your touch", "For the earliest intercept" };
			ImGui::PushItemWidth(220);
			if (ImGui::Combo("Target car outline", &th, thNames, 3))
			{
				float v = static_cast<float>(th);
				onGame([this, v]() { cvarManager->getCvar("rlcoach_target_hitbox").setValue(v); });
			}
			ImGui::PopItemWidth();
		}
		if (ImGui::TreeNode("What the lines mean"))
		{
			ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.16f, 1.0f), "Orange");  ImGui::SameLine(); ImGui::TextDisabled("ball path");
			ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Blue");    ImGui::SameLine(); ImGui::TextDisabled("car path (bright = already played)");
			ImGui::TextColored(kGold, "Yellow ring");                        ImGui::SameLine(); ImGui::TextDisabled("where you needed to hit the ball; yellow line = where it needed to go");
			ImGui::TextColored(kGood, "Green ring");                         ImGui::SameLine(); ImGui::TextDisabled("where you hit it; green line = where it went");
			ImGui::TextColored(kGold, "Yellow ghost ball");                  ImGui::SameLine(); ImGui::TextDisabled("earliest a fast, direct aerial could have met it");
			ImGui::TextColored(kGold, "Yellow box");                         ImGui::SameLine(); ImGui::TextDisabled("your hitbox placed and aimed to make that ideal hit");
			ImGui::TextColored(kGood, "Green box");                          ImGui::SameLine(); ImGui::TextDisabled("your real hitbox at contact");
			ImGui::TextColored(ImVec4(1, 1, 1, 0.8f), "White dashes");       ImGui::SameLine(); ImGui::TextDisabled("predicted ball path");
			ImGui::TreePop();
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
		ImGui::PushItemWidth(110);
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

	// A small 5-point star; filled when kept
	auto drawStar = [](ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col) {
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
			dl->AddPolyline(pts, 10, col, true, 1.2f);
	};

	ImGui::BeginChild("replist", ImVec2(0, uiViewerActive ? 220.0f : 380.0f), false);
	ImGui::Columns(7, "repcols", false);
	ImGui::SetColumnWidth(0, 34); ImGui::SetColumnWidth(1, 170); ImGui::SetColumnWidth(2, 70); ImGui::SetColumnWidth(3, 60);
	ImGui::SetColumnWidth(4, 64); ImGui::SetColumnWidth(5, 70);
	const char* heads[7] = { "", "When", "Ended", "Length", "Touches", "You", "Auto" };
	for (const char* h : heads) { ImGui::TextDisabled("%s", h); ImGui::NextColumn(); }
	ImGui::Separator();
	std::string toggleKeep;
	// Only draw the rows that are on screen. Drawing all of them (1,500+) overflows ImGui's vertex buffer,
	// which shows up as smeared, garbled shapes. Every row is one fixed height so the clipper can skip the rest.
	const float rowH = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
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
			ImU32 col = it.kept ? IM_COL32(255, 205, 64, 255) : (hover ? IM_COL32(220, 224, 232, 255) : IM_COL32(90, 96, 110, 255));
			if (it.hasRecording || it.kept)
				drawStar(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.5f + 2, p.y + h * 0.5f), h * 0.45f, it.kept, col);
			if (hover) ImGui::SetTooltip(it.kept ? "Kept forever. Click to un-keep." : "Keep this rep forever");
		}
		ImGui::NextColumn();

		std::string when = it.savedAt.size() >= 19 ? it.savedAt.substr(5, 11) : it.savedAt;
		std::string label = std::format("{}##row{}", when, i);
		if (!it.hasRecording) ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		if (ImGui::Selectable(label.c_str(), selectedRep == i || isWatching, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
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
			Pill(it.importedFrom, ImVec4(0.75f, 0.6f, 1.0f, 1.0f));
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
			if (ag->second.shotType == 1) { ImGui::SameLine(); ImGui::TextDisabled("ground"); }
			if (ag->second.shotType == 2) { ImGui::SameLine(); ImGui::TextDisabled("dbl tap"); }
			if (ag->second.shotType == 3) { ImGui::SameLine(); ImGui::TextDisabled("dribble"); }
			if (ag->second.shotType == 4) { ImGui::SameLine(); ImGui::TextDisabled("air drib"); }
			if (ag->second.shotType == 5) { ImGui::SameLine(); ImGui::TextDisabled("reset"); }
		}
		ImGui::NextColumn();
		if (ImGui::GetCursorPosY() < rowTop + rowH) ImGui::SetCursorPosY(rowTop + rowH);   // keep every row the same height
	}
	ImGui::Columns(1);
	ImGui::EndChild();
	if (!toggleKeep.empty()) ToggleKeep(toggleKeep);

	bool canWatchSelected = selectedRep >= 0 && selectedRep < static_cast<int>(repList.size()) && repList[selectedRep].hasRecording;
	if (canWatchSelected && AccentButton("Watch selected"))
	{
		RepListItem pick = repList[selectedRep];
		onGame([this, pick]() { ViewerLoad(pick); });
	}
	const RepListItem* latest = nullptr;
	for (int i : shown) if (repList[i].hasRecording) { latest = &repList[i]; break; }
	if (latest)
	{
		if (canWatchSelected) ImGui::SameLine();
		if (ImGui::Button("Watch latest"))
		{
			RepListItem pick = *latest;
			onGame([this, pick]() { ViewerLoad(pick); });
		}
	}
}
