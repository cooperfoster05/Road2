#include "pch.h"
#include "RLCoach.h"
#include "bakkesmod/wrappers/includes.h"

#include <algorithm>
#include <cmath>
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
	std::vector<Frame> loaded;
	std::string err;
	if (!RepLibrary::LoadRep(writer->Folder() / "reps" / item.file, loaded, err))
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
	shotBook.Update(writer->Folder());
	ShotReference viewerRef = shotBook.Get(item.shotKey);
	viewerGrade = Grader::GradeRep(viewerFrames, viewerSegs, viewerShot, item.goal, item.floorBeforeGoal, viewerRef);
	gradeBook.TrySet(item.file, item.endReason == "reset" ? 0 : viewerGrade.overall, item.shotKey, viewerRef.bestFile);
	{
		std::lock_guard<std::mutex> lock(uiSegMutex);
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
		auto box = [&](const Vector& p, float size, unsigned char r, unsigned char g, unsigned char b) {
			Vector2F s;
			if (!ToScreen(canvas, camLoc, camFwd, p, s)) return;
			canvas.SetColor(r, g, b, 255);
			canvas.SetPosition(Vector2F{ s.X - size / 2, s.Y - size / 2 });
			canvas.FillBox(Vector2F{ size, size });
		};
		auto line = [&](const Vector& a, const Vector& b, unsigned char r, unsigned char g, unsigned char bl) {
			Vector2F sa, sb;
			if (!ToScreen(canvas, camLoc, camFwd, a, sa) || !ToScreen(canvas, camLoc, camFwd, b, sb)) return;
			canvas.SetColor(r, g, bl, 230);
			canvas.DrawLine(sa, sb, 3.0f);
		};
		const ShotAnalysis& s = viewerShot;
		Vector c(s.ballCenter.x, s.ballCenter.y, s.ballCenter.z);

		// Yellow: where on the ball you needed to hit it, and the direction it needed to go
		box(Vector(s.idealContact.x, s.idealContact.y, s.idealContact.z), 12.0f, 255, 220, 0);
		line(c, Vector(c.X + s.idealDir.x * 900, c.Y + s.idealDir.y * 900, c.Z + s.idealDir.z * 900), 255, 220, 0);
		// Green: the direction your shot actually went
		line(c, Vector(c.X + s.shotDir.x * 900, c.Y + s.shotDir.y * 900, c.Z + s.shotDir.z * 900), 80, 255, 120);

		// Yellow ring: earliest a fast, direct aerial could have met the ball
		if (s.earliestTick >= 0 && s.earliestTick < n)
		{
			const Frame& e = viewerFrames[s.earliestTick];
			Vector2F es;
			if (ToScreen(canvas, camLoc, camFwd, Vector(e.ballX, e.ballY, e.ballZ), es))
			{
				canvas.SetColor(255, 220, 0, 255);
				for (int a = 0; a < 16; a++)
				{
					float a0 = a * 0.3927f, a1 = (a + 1) * 0.3927f;
					canvas.DrawLine(Vector2F{ es.X + 14 * std::cos(a0), es.Y + 14 * std::sin(a0) },
						Vector2F{ es.X + 14 * std::cos(a1), es.Y + 14 * std::sin(a1) }, 2.0f);
				}
			}
		}
	}

	// Ball touches: green squares where you hit the ball
	for (const Frame& f : viewerFrames)
	{
		if (!f.touch) continue;
		Vector2F s;
		if (!ToScreen(canvas, camLoc, camFwd, Vector(f.touchX, f.touchY, f.touchZ), s)) continue;
		canvas.SetColor(80, 255, 120, 255);
		canvas.SetPosition(Vector2F{ s.X - 5.0f, s.Y - 5.0f });
		canvas.FillBox(Vector2F{ 10.0f, 10.0f });
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
		gradeThreadBusy = false;
	});
}

void RLCoach::RenderWindow()
{
	if (!repListLoaded) RefreshRepList();

	auto onGame = [this](std::function<void()> fn) {
		gameWrapper->Execute([fn](GameWrapper*) { fn(); });
	};

	// ---- Rep list ----
	if (ImGui::Button("Refresh")) RefreshRepList();
	ImGui::SameLine();
	ImGui::Text("%d reps saved (newest first). Double-click to watch.", static_cast<int>(repList.size()));

	gradeBook.TryCopy(uiAutoGrades);   // keeps the last copy if a refresh is busy
	if (gradeThreadBusy) { ImGui::SameLine(); ImGui::TextDisabled("(grading...)"); }

	auto overallColor = [](int o) -> ImVec4 {
		if (o >= 4) return ImVec4(0.45f, 0.9f, 0.45f, 1.0f);
		if (o == 3) return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
		if (o == 2) return ImVec4(1.0f, 0.63f, 0.31f, 1.0f);
		if (o == 1) return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
		return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
	};

	ImGui::BeginChild("replist", ImVec2(0, 260), true);
	ImGui::Columns(6, "repcols", false);
	ImGui::SetColumnWidth(0, 110); ImGui::SetColumnWidth(1, 70); ImGui::SetColumnWidth(2, 60);
	ImGui::SetColumnWidth(3, 60); ImGui::SetColumnWidth(4, 70);
	const char* heads[6] = { "When", "Ended", "Length", "Touches", "Your grade", "Auto grade" };
	for (const char* h : heads) { ImGui::TextDisabled("%s", h); ImGui::NextColumn(); }
	ImGui::Separator();
	for (int i = 0; i < static_cast<int>(repList.size()); i++)
	{
		const RepListItem& it = repList[i];
		std::string when = it.savedAt.size() >= 19 ? it.savedAt.substr(5, 11) : it.savedAt;
		std::string label = std::format("{}##row{}", when, i);
		if (ImGui::Selectable(label.c_str(), selectedRep == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
		{
			selectedRep = i;
			if (ImGui::IsMouseDoubleClicked(0))
			{
				RepListItem pick = it;
				onGame([this, pick]() { ViewerLoad(pick); });
			}
		}
		ImGui::NextColumn();
		ImGui::TextUnformatted(it.endReason.c_str()); ImGui::NextColumn();
		ImGui::Text("%.1fs", it.duration); ImGui::NextColumn();
		ImGui::Text("%d", it.touches); ImGui::NextColumn();
		if (it.rating > 0) ImGui::TextColored(overallColor(it.rating), "%d", it.rating);
		else ImGui::TextDisabled("-");
		ImGui::NextColumn();
		auto ag = uiAutoGrades.find(it.file);
		if (ag == uiAutoGrades.end()) ImGui::TextDisabled("...");
		else if (ag->second <= 0) ImGui::TextDisabled("n/a");
		else ImGui::TextColored(overallColor(ag->second), "%d", ag->second);
		ImGui::NextColumn();
	}
	ImGui::Columns(1);
	ImGui::EndChild();

	if (selectedRep >= 0 && ImGui::Button("Watch selected"))
	{
		RepListItem pick = repList[selectedRep];
		onGame([this, pick]() { ViewerLoad(pick); });
	}
	if (!repList.empty())
	{
		if (selectedRep >= 0) ImGui::SameLine();
		if (ImGui::Button("Watch latest"))
		{
			RepListItem pick = repList.front();
			onGame([this, pick]() { ViewerLoad(pick); });
		}
	}

	ImGui::Separator();

	// ---- Playback controls ----
	if (!uiViewerActive)
	{
		ImGui::TextWrapped("Pick a rep to watch. Playback happens in free play; recording pauses while you watch.");
		return;
	}

	bool playing = uiViewerPlaying;
	if (ImGui::Button(playing ? "Pause" : "Play"))
		onGame([this]() { viewerPlaying = !viewerPlaying; uiViewerPlaying = viewerPlaying; });
	ImGui::SameLine();
	if (ImGui::Button("|<")) onGame([this]() { ViewerSeek(0.0f); });
	ImGui::SameLine();
	if (ImGui::Button("< 1 tick")) onGame([this]() { viewerPlaying = false; ViewerSeek(std::floor(viewerPlayhead) - 1.0f); });
	ImGui::SameLine();
	if (ImGui::Button("1 tick >")) onGame([this]() { viewerPlaying = false; ViewerSeek(std::floor(viewerPlayhead) + 1.0f); });
	ImGui::SameLine();
	if (ImGui::Button("Stop watching")) onGame([this]() { ViewerExit(); });

	ImGui::Text("Speed:");
	const float speeds[4] = { 0.1f, 0.25f, 0.5f, 1.0f };
	const char* names[4] = { "0.1x", "0.25x", "0.5x", "1x" };
	for (int i = 0; i < 4; i++)
	{
		ImGui::SameLine();
		bool current = std::fabs(uiViewerSpeed.load() - speeds[i]) < 0.001f;
		if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
		if (ImGui::Button(names[i]))
		{
			float sp = speeds[i];
			uiViewerSpeed = sp;
			onGame([this, sp]() { viewerSpeed = sp; });
		}
		if (current) ImGui::PopStyleColor();
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Loop", &uiLoop))
	{
		bool loop = uiLoop;
		onGame([this, loop]() { viewerLoop = loop; });
	}

	int total = uiViewerTotal;
	int head = static_cast<int>(uiViewerPlayhead.load());
	int scrub = head;
	// ---- Segment timeline: click a segment to jump to it ----
	std::vector<Segment> segs;
	std::vector<UiGrade> grades;
	std::string overall;
	int segTotal = 0;
	{
		std::lock_guard<std::mutex> lock(uiSegMutex);
		segs = uiSegs;
		segTotal = uiSegTotal;
		grades = uiGrades;
		overall = uiOverall;
	}
	auto gradeColor = [](const std::string& g) -> ImVec4 {
		if (g == "Clean" || g == "Calculated") return ImVec4(0.45f, 0.9f, 0.45f, 1.0f);
		if (g == "Solid") return ImVec4(0.6f, 0.8f, 1.0f, 1.0f);
		if (g == "Sloppy") return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
		return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
	};
	if (!overall.empty()) ImGui::TextWrapped("Grade %s", overall.c_str());
	if (!segs.empty() && segTotal > 1)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 origin = ImGui::GetCursorScreenPos();
		float width = ImGui::GetContentRegionAvail().x;
		const float rowH = 22.0f;
		auto colorFor = [](SegKind k) -> ImU32 {
			switch (k)
			{
			case SegKind::Approach: return IM_COL32(90, 110, 140, 255);
			case SegKind::Takeoff:  return IM_COL32(70, 150, 220, 255);
			case SegKind::Flight:   return IM_COL32(60, 120, 200, 255);
			case SegKind::Contact:  return IM_COL32(80, 220, 120, 255);
			case SegKind::Result:   return IM_COL32(230, 160, 50, 255);
			case SegKind::Recovery: return IM_COL32(170, 110, 210, 255);
			}
			return IM_COL32(128, 128, 128, 255);
		};
		auto xOf = [&](int tick) { return origin.x + width * static_cast<float>(tick) / static_cast<float>(segTotal - 1); };

		// Row 1: the play (approach to result). Row 2: recovery, which overlaps the result in time.
		for (int si = 0; si < static_cast<int>(segs.size()); si++)
		{
			const Segment& sg = segs[si];
			float y0 = origin.y + (sg.kind == SegKind::Recovery ? rowH + 4.0f : 0.0f);
			float x0 = xOf(sg.start), x1 = std::max(xOf(sg.end + 1), x0 + 3.0f);
			dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + rowH), colorFor(sg.kind));
			dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y0 + rowH), IM_COL32(20, 20, 20, 255));
			std::string labelStr = Segmenter::Name(sg.kind);
			std::string why;
			for (const UiGrade& ug : grades)
				if (ug.segIndex == si) { labelStr += ": " + ug.name; why = ug.why; }
			const char* label = labelStr.c_str();
			ImVec2 ts = ImGui::CalcTextSize(label);
			if (x1 - x0 > ts.x + 6.0f)
				dl->AddText(ImVec2(x0 + 4.0f, y0 + (rowH - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), label);

			ImGui::SetCursorScreenPos(ImVec2(x0, y0));
			ImGui::PushID(si);  // stable ID so a click's press and release land on the same button
			if (ImGui::InvisibleButton("seg", ImVec2(x1 - x0, rowH)))
			{
				float target = static_cast<float>(sg.start);
				onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s: %.2fs to %.2fs (click to jump here)%s%s", label, sg.start / 120.0f, (sg.end + 1) / 120.0f,
					why.empty() ? "" : "\n", why.c_str());
			ImGui::PopID();
		}
		// Playhead
		float px = xOf(static_cast<int>(uiViewerPlayhead.load()));
		dl->AddLine(ImVec2(px, origin.y - 2.0f), ImVec2(px, origin.y + 2 * rowH + 6.0f), IM_COL32(255, 255, 255, 255), 2.0f);
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 2 * rowH + 10.0f));
	}

	// Segment grades, in order; the biggest fix is marked and clickable
	for (const UiGrade& ug : grades)
	{
		if (ug.name.empty() || ug.segIndex < 0 || ug.segIndex >= static_cast<int>(segs.size())) continue;
		const Segment& sg = segs[ug.segIndex];
		ImGui::TextColored(gradeColor(ug.name), "%-8s", ug.name.c_str());
		ImGui::SameLine();
		std::string line = std::format("{}{}: {}", ug.biggest ? ">> " : "", Segmenter::Name(sg.kind), ug.why);
		ImGui::PushID(ug.segIndex + 1000);
		if (ImGui::Selectable(line.c_str(), ug.biggest))
		{
			float target = static_cast<float>(sg.start);
			onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
		}
		ImGui::PopID();
	}

	ImGui::PushItemWidth(-1);
	if (ImGui::SliderInt("##scrub", &scrub, 0, total > 0 ? total - 1 : 0,
		std::format("{:.2f}s / {:.2f}s", scrub / 120.0f, total / 120.0f).c_str()))
	{
		float target = static_cast<float>(scrub);
		onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
	}
	ImGui::PopItemWidth();
}
