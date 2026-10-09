#pragma once

#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "RepData.h"
#include "RepWriter.h"
#include "RepStore.h"
#include "RepLibrary.h"
#include "BallPredictor.h"
#include "ShotGeometry.h"
#include "Segmenter.h"
#include "ShotBook.h"
#include "Grader.h"
#include "GradeBook.h"
#include "ControllerOverlay.h"
#include <mutex>
#include <thread>
#include <unordered_map>

#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);


// Running average and recent peak of how long a piece of the plugin takes (milliseconds)
struct PerfStat
{
	float avgMs = 0.0f;
	float peakMs = 0.0f;          // worst in the last second
	float windowPeak = 0.0f;
	std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
	void Add(float ms)
	{
		avgMs = avgMs * 0.98f + ms * 0.02f;
		windowPeak = std::max(windowPeak, ms);
		auto now = std::chrono::steady_clock::now();
		if (now - windowStart > std::chrono::seconds(1)) { peakMs = windowPeak; windowPeak = 0.0f; windowStart = now; }
	}
};

// Times its own lifetime into a PerfStat
struct PerfTimer
{
	PerfStat& stat;
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	explicit PerfTimer(PerfStat& s) : stat(s) {}
	~PerfTimer() { stat.Add(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
};

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

	// ---- Storage upkeep (RLCoachStorage.cpp) ----
	std::shared_ptr<float> keepDays;          // rlcoach_keep_days: full recordings of unrated reps
	std::shared_ptr<float> keepRatedDays;     // rlcoach_keep_rated_days: full recordings of reps you rated
	std::shared_ptr<float> storageCapMb;      // rlcoach_storage_cap_mb: oldest un-kept recordings go first past this
	std::thread housekeepThread;
	std::atomic<bool> housekeepBusy{ false };
	std::atomic<bool> housekeepStop{ false };
	std::mutex storageMsgMutex;
	std::string storageMsg;
	std::chrono::steady_clock::time_point storageStatsAt{};
	uint64_t uiStorageBytes = 0;              // render thread
	void StartHousekeeping();                 // game or render thread
	void Housekeep(const std::filesystem::path& root, const std::filesystem::path& sessionBundle,
		float keepD, float keepRatedD, float capMb);
	int  TrimStorage(const std::filesystem::path& root, const std::filesystem::path& sessionBundle,
		float keepD, float keepRatedD, float capMb);
	void SetStorageMsg(const std::string& msg);
	std::string GetStorageMsg();
	void RenderStorageSettings();
	void ToggleKeep(const std::string& file);  // render thread
	std::thread shareThread;
	std::atomic<bool> shareBusy{ false };
	std::atomic<bool> repListStale{ false };   // the rep list should reload (e.g. after an import)
	void StartExport();                        // render thread; works in the background
	void StartImport();

	// Auto grades for the rep list, filled in on a worker thread
	GradeBook gradeBook;
	std::thread gradeThread;
	std::atomic<bool> gradeThreadBusy{ false };
	std::unordered_map<std::string, GradeBook::Brief> uiAutoGrades;   // render thread copy
	void StartGradeRefresh();

	// Copy of the segments for the viewer window (render thread)
	std::mutex uiSegMutex;
	std::vector<Segment> uiSegs;
	int uiSegTotal = 0;
	struct UiGrade { int segIndex; std::string name; std::string why; bool biggest; };
	std::vector<UiGrade> uiGrades;
	std::string uiOverall;
	std::string uiViewerFile;   // the rep being watched

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
	ShotEndTracker endTracker;      // when the shot is over (floor for aerials, stopped heading to goal for ground shots)
	int   predictedTouch = -1;      // the ground touch we already asked the ball predictor about
	bool  recovered = false;

	// ---- Saving and rating ----
	std::unique_ptr<RepWriter> writer;
	int   repsThisSession = 0;
	RepSummary lastRep;
	// Grade of the last saved rep, shown on the panel in the pack
	// A panel line can mix colors (e.g. "Takeoff " gray + "Sloppy" yellow)
	struct PanelChunk { std::string text; unsigned char r, g, b; };
	struct PanelGradeLine { std::vector<PanelChunk> chunks; };
	std::vector<PanelGradeLine> lastRepGradeLines;
	void GradeFinishedRep(const RepSummary& s);
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
	void DrawTargetHitbox(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd, const ShotAnalysis& shot,
		const std::vector<Frame>& fr, int tick, const Vector& ballNow, bool showActual);
	void StartPredictor();

	// ---- Practice mode (game thread) ----
	enum class PracticeState { Off, Frozen, Live };
	PracticeState practiceState = PracticeState::Off;
	std::vector<Frame> practiceRep;          // the rep being practiced
	ShotAnalysis practiceShot;               // its shot math (for the ideal-contact ring)
	RepListItem practiceItem;
	SegKind practiceKind = SegKind::Approach;
	int   practiceSegStart = 0;              // start tick of the practiced segment (for grading)
	int   practiceTargetTick = 0;            // the moment you picked (segment start, or the viewer's playhead)
	int   practiceStartTick = 0;             // where each attempt starts (target minus lead-in)
	int   practiceBallStartTick = 0;         // never start before the ball is moving
	float practiceLeadIn = 0.5f;             // seconds of lead-in before the target
	int   practiceFlipExpireTick = -1;       // tick (rep time) your flip runs out, -1 if no timer applies
	bool  practiceFlipHeld = false;          // we're holding the game's flip timer open
	float practiceOrigMaxDodge = 1.25f;
	std::string practiceLabel;
	std::vector<Frame> practiceLive;         // the attempt in progress, from practiceStartTick on
	float practiceSpeed = 1.0f;
	bool  practiceAutoRamp = false;
	int   practiceStreak = 0;
	int   practiceAttempts = 0;
	std::map<int, int> practiceHitsByPct;    // hits at each game speed (percent)
	int   practiceHitsTotal = 0;
	std::string practicePending;             // goal / floor once the shot is over
	int   practiceEventTick = -1;
	bool  practiceGoalPending = false;
	bool  practiceWentIn = false;
	bool  practiceFloorFirst = false;
	bool  practiceTouchPending = false;
	Vector practiceTouchLoc;
	int   practiceRecoverRun = 0;
	ShotEndTracker practiceEndTracker;
	bool  practiceHavePrev = false;
	Vector practicePrevCar, practicePrevBall;
	float practiceSavedGameSpeed = 1.0f;
	std::vector<PanelGradeLine> practiceResultLines;
	std::vector<Frame> practiceRefFrames;    // your best rep on this shot, drawn faintly
	std::string practiceRefLabel;

	// Copies for the viewer window
	std::atomic<bool>  uiPracticeActive{ false };
	std::atomic<float> uiPracticeSpeed{ 1.0f };
	std::atomic<bool>  uiPracticeAutoRamp{ false };
	std::atomic<float> uiPracticeLeadIn{ 0.5f };
	std::atomic<int>   uiPracticeTarget{ -1 };

	void PracticeStart(int segIndex);        // from the rep being watched
	void PracticeStartAt(int tick);          // "Practice from here": the viewer's current moment
	void PracticeBegin(int targetTick, SegKind kind, int segStart, const std::string& label);
	void PracticeSetLeadIn(float sec);
	void PracticeReleaseFlip(CarWrapper& car);
	void PracticeTick(CarWrapper& car, const ControllerInput& in);
	void PracticeApplyStart(CarWrapper& car, BallWrapper& ball, bool frozen);
	void PracticeFinishAttempt(const std::string& reason);
	void PracticeRetry();
	void PracticeExit();
	void PracticeSetSpeed(float speed);
	void PracticePickReference();
	void DrawPracticeOverlay(CanvasWrapper& canvas);
	Frame CaptureAt(CarWrapper& car, BallWrapper& ball, const ControllerInput& in, int tick, float wallMs);

	// ---- Timing readout ----
	std::shared_ptr<bool> showTiming;         // rlcoach_show_timing
	PerfStat perfTick, perfInput, perfDraw;   // game thread only

	// ---- Drawing (RLCoachHud.cpp) ----
	void RenderHud(CanvasWrapper canvas);
	void DrawPanel(CanvasWrapper& canvas, CarWrapper& car, const PadInput& pad);

	// ---- Look settings (RLCoachHud.cpp) ----
	std::shared_ptr<bool>  hudCompact;        // rlcoach_hud_compact: only the essentials on the panel
	std::shared_ptr<float> hudScale;          // rlcoach_hud_scale
	std::shared_ptr<float> hudX, hudY;        // rlcoach_hud_x / rlcoach_hud_y: panel corner, fraction of the screen
	std::shared_ptr<int>   speedUnits;        // rlcoach_speed_units: 0 uu/s, 1 km/h, 2 mph
	std::shared_ptr<int>   inputDisplay;      // rlcoach_input_display: 0 none, 1 text on the panel, 2 PS4, 3 Xbox
	std::shared_ptr<float> padX, padY, padScale;   // rlcoach_pad_x / _y / _scale
	std::shared_ptr<int>   padTheme;          // rlcoach_pad_theme: 0 dark, 1 light
	std::shared_ptr<int>   padJump, padBoost, padSlide, padRollLeft, padRollRight;   // rlcoach_pad_<action>: button index
	std::shared_ptr<int>   targetHitbox;      // rlcoach_target_hitbox: 0 off, 1 for your touch, 2 for the earliest intercept
	void RegisterLookCvars();
	void RenderLookSettings();
	void RenderPadSettings();

public:
	void RenderSettings() override;
	void RenderWindow() override;
};
