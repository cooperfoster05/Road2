#pragma once

#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "RepData.h"
#include "RepWriter.h"
#include "RepLibrary.h"
#include "BallPredictor.h"
#include "ShotGeometry.h"
#include "Segmenter.h"
#include "ShotBook.h"
#include "Grader.h"
#include <mutex>
#include <thread>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);


class RLCoach: public BakkesMod::Plugin::BakkesModPlugin
	, public SettingsWindowBase  // adds an RLCoach tab in the F2 menu
	, public PluginWindowBase    // the rep viewer window
{
	// ---- Settings (cvars) ----
	std::shared_ptr<bool>  hudEnabled;        // rlcoach_hud
	std::shared_ptr<bool>  recordingEnabled;  // rlcoach_recording (data farming on/off, starts off every session)
	std::shared_ptr<float> maxRepSeconds;     // rlcoach_max_rep_seconds
	std::shared_ptr<bool>  useNumpad;         // rlcoach_rate_numpad (rate with numpad 1-5 instead of number row)
	std::shared_ptr<float> postRollSeconds;   // rlcoach_postroll_seconds (minimum recording after the shot, catches late goals)
	std::shared_ptr<float> postRollMaxSeconds;// rlcoach_postroll_max_seconds (keep going until you recover, up to this long)
	std::shared_ptr<bool>  livePrediction;    // rlcoach_live_prediction (draw the predicted ball path while you play)

	// ---- Ball prediction (RocketSim) ----
	BallPredictor predictor;
	std::vector<BallSample> livePred;      // predicted path for the rep in progress
	std::vector<BallSample> viewerPred;    // predicted path for the rep being watched
	ShotAnalysis viewerShot;               // shot math for the rep being watched
	RepSegments viewerSegs;                // segments of the rep being watched
	RepGrade viewerGrade;                  // grades for the rep being watched

	// Your fastest goal on each shot, for timing grades
	ShotBook shotBook;
	std::thread shotBookThread;

	// Copy of the segments for the viewer window (render thread)
	std::mutex uiSegMutex;
	std::vector<Segment> uiSegs;
	int uiSegTotal = 0;
	struct UiGrade { int segIndex; std::string name; std::string why; bool biggest; };
	std::vector<UiGrade> uiGrades;
	std::string uiOverall;

	// ---- Rep detection ----
	enum class RepState { Idle, WaitReset, Armed, Recording };
	RepState state = RepState::Idle;

	std::vector<Frame> frames;
	std::chrono::steady_clock::time_point repStartTime;
	bool  havePrev = false;
	Vector prevCarLoc, prevBallLoc;
	bool  goalPending = false;
	bool  touchPending = false;
	Vector touchLoc;
	int   touchCount = 0;
	int   lastTouchTick = -1000;
	std::string pendingEndReason;   // set when the shot is over but recovery is still being recorded
	int   eventTick = -1;
	bool  floorBeforeGoal = false;
	Vector armedCarLoc, armedBallLoc;
	int   armedTicks = 0;
	int   queuedRating = 0;         // rating pressed during a rep, applied when it saves
	int   recoverRun = 0;           // ticks in a row the car has been back in control after the shot
	bool  recovered = false;

	// ---- Saving and rating ----
	std::unique_ptr<RepWriter> writer;
	int   repsThisSession = 0;
	RepSummary lastRep;
	bool  haveLastRep = false;
	int   lastRating = 0;
	int   keyIndex[5] = {};
	bool  keyWasDown[5] = {};
	std::string statusMessage;

	// Latest input, for the HUD
	ControllerInput lastInput{};

	// ---- Plugin lifecycle ----
	void onLoad() override;
	void onUnload() override;

	// ---- Recorder ----
	void OnVehicleInput(CarWrapper car);       // runs every physics tick
	void OnBallTouch(CarWrapper car, void* params);
	void OnGoal();
	Frame Capture(CarWrapper& car, BallWrapper& ball, const ControllerInput& in);
	void FinishRep(const std::string& reason);
	void SetRecording(bool on);                // game thread only
	void EnterArmed();
	bool IsOfflinePractice();

	// ---- Rating ----
	void UpdateRatingKeys();
	void RateLastRep(int rating);
	void HandleRatingPress(int rating);
	void LoadRatingKeys();

	// ---- Test tools ----
	void TestSetState();

	// ---- Rep viewer (game thread state) ----
	bool  viewerActive = false;
	bool  viewerPlaying = false;
	bool  viewerLoop = true;
	float viewerSpeed = 1.0f;
	float viewerPlayhead = 0.0f;       // fractional tick
	float savedGameSpeed = 1.0f;
	std::vector<Frame> viewerFrames;
	RepListItem viewerRep;
	std::string viewerStatus;

	// Copies the viewer window can read safely from the render thread
	std::atomic<bool>  uiViewerActive{ false };
	std::atomic<bool>  uiViewerPlaying{ false };
	std::atomic<int>   uiViewerTotal{ 0 };
	std::atomic<float> uiViewerPlayhead{ 0.0f };
	std::atomic<float> uiViewerSpeed{ 1.0f };

	// Viewer window state (render thread only)
	std::vector<RepListItem> repList;
	bool  repListLoaded = false;
	int   selectedRep = -1;
	bool  uiLoop = true;

	void ViewerLoad(const RepListItem& item);  // game thread
	void ViewerExit();                         // game thread
	void ViewerSeek(float tick);               // game thread
	void ViewerTick(CarWrapper& car);          // game thread, every physics tick
	void OverrideViewerInput(CarWrapper& car, void* params); // game thread, before the game reads input
	void RefreshRepList();                     // render thread
	void DrawViewerPaths(CanvasWrapper& canvas);
	void DrawPredictedPath(CanvasWrapper& canvas, const std::vector<BallSample>& path);
	void StartPredictor();

	// ---- Drawing ----
	void RenderHud(CanvasWrapper canvas);

public:
	void RenderSettings() override;
	void RenderWindow() override;
};
