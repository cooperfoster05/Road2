#pragma once

#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "RepData.h"
#include "RepWriter.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);


class RLCoach: public BakkesMod::Plugin::BakkesModPlugin
	, public SettingsWindowBase  // adds an RLCoach tab in the F2 menu
{
	// ---- Settings (cvars) ----
	std::shared_ptr<bool>  hudEnabled;        // rlcoach_hud
	std::shared_ptr<bool>  recordingEnabled;  // rlcoach_recording (data farming on/off, starts off every session)
	std::shared_ptr<float> maxRepSeconds;     // rlcoach_max_rep_seconds
	std::shared_ptr<bool>  useNumpad;         // rlcoach_rate_numpad (rate with numpad 1-5 instead of number row)

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
	bool IsOfflinePractice();

	// ---- Rating ----
	void UpdateRatingKeys();
	void RateLastRep(int rating);
	void LoadRatingKeys();

	// ---- Test tools ----
	void TestSetState();

	// ---- Drawing ----
	void RenderHud(CanvasWrapper canvas);

public:
	void RenderSettings() override;
};
