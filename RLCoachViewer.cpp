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

	viewerFrames = std::move(loaded);
	viewerRep = item;
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
	uiViewerActive = false;
	uiViewerPlaying = false;

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

	const float last = static_cast<float>(viewerFrames.size() - 1);
	if (viewerPlaying)
	{
		viewerPlayhead += viewerSpeed;
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

	// While playing, give things their recorded velocity (scaled to playback speed) so they look natural.
	// While paused, hold them perfectly still.
	float vScale = viewerPlaying ? viewerSpeed : 0.0f;

	car.SetLocation(Vector(Lerp(f0.carX, f1.carX, a), Lerp(f0.carY, f1.carY, a), Lerp(f0.carZ, f1.carZ, a)));
	car.SetRotation(Rotator(LerpAngle(f0.carPitch, f1.carPitch, a), LerpAngle(f0.carYaw, f1.carYaw, a), LerpAngle(f0.carRoll, f1.carRoll, a)));
	car.SetVelocity(Vector(f0.carVX * vScale, f0.carVY * vScale, f0.carVZ * vScale));
	car.SetAngularVelocity(Vector(f0.carAVX * vScale, f0.carAVY * vScale, f0.carAVZ * vScale), false);
	BoostWrapper boostComp = car.GetBoostComponent();
	if (!boostComp.IsNull()) boostComp.SetCurrentBoostAmount(f0.boostAmount / 100.0f);

	float bx = Lerp(f0.ballX, f1.ballX, a), by = Lerp(f0.ballY, f1.ballY, a), bz = Lerp(f0.ballZ, f1.ballZ, a);
	bool inGoalMouth = std::fabs(bx) < kGoalHalfWidth && bz < kGoalHeight;
	bool guarded = inGoalMouth && std::fabs(by) > kGoalLineY;
	if (guarded) by = std::copysign(kGoalLineY, by);
	ball.SetLocation(Vector(bx, by, bz));
	if (guarded)
		ball.SetVelocity(Vector(0.0f, 0.0f, 0.0f));
	else
		ball.SetVelocity(Vector(f0.ballVX * vScale, f0.ballVY * vScale, f0.ballVZ * vScale));
	ball.SetAngularVelocity(Vector(f0.ballAVX * vScale, f0.ballAVY * vScale, f0.ballAVZ * vScale), false);

	uiViewerPlayhead = viewerPlayhead;
	uiViewerPlaying = viewerPlaying;
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

	drawPath([](const Frame& f) { return Vector(f.ballX, f.ballY, f.ballZ); }, 255, 160, 40);
	drawPath([](const Frame& f) { return Vector(f.carX, f.carY, f.carZ); }, 90, 190, 255);

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

// ============================================================
// Viewer window (render thread). Anything that touches the game
// is handed to the game thread with gameWrapper->Execute.
// ============================================================

void RLCoach::RefreshRepList()
{
	repList = writer ? RepLibrary::ListReps(writer->Folder()) : std::vector<RepListItem>{};
	repListLoaded = true;
	if (selectedRep >= static_cast<int>(repList.size())) selectedRep = -1;
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

	ImGui::BeginChild("replist", ImVec2(0, 230), true);
	for (int i = 0; i < static_cast<int>(repList.size()); i++)
	{
		const RepListItem& it = repList[i];
		std::string when = it.savedAt.size() >= 19 ? it.savedAt.substr(5, 11) : it.savedAt;
		std::string label = std::format("{}   {:<8} {:>4.1f}s   {} touch   {}##{}",
			when, it.endReason, it.duration, it.touches,
			it.rating > 0 ? std::format("rated {}", it.rating) : std::string("unrated"), i);
		if (ImGui::Selectable(label.c_str(), selectedRep == i, ImGuiSelectableFlags_AllowDoubleClick))
		{
			selectedRep = i;
			if (ImGui::IsMouseDoubleClicked(0))
			{
				RepListItem pick = it;
				onGame([this, pick]() { ViewerLoad(pick); });
			}
		}
	}
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
	ImGui::PushItemWidth(-1);
	if (ImGui::SliderInt("##scrub", &scrub, 0, total > 0 ? total - 1 : 0,
		std::format("{:.2f}s / {:.2f}s", scrub / 120.0f, total / 120.0f).c_str()))
	{
		float target = static_cast<float>(scrub);
		onGame([this, target]() { viewerPlaying = false; ViewerSeek(target); });
	}
	ImGui::PopItemWidth();
}
