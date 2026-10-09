#pragma once
// Draws a live controller (PS4 or Xbox style) showing what you're pressing: sticks, triggers and
// the buttons your actions are bound to. Game thread only (canvas drawing).

#include "bakkesmod/wrappers/canvaswrapper.h"

struct PadInput
{
	float throttle = 0, steer = 0, pitch = 0, yaw = 0, roll = 0;
	bool  jump = false, boost = false, handbrake = false;
	float boostAmount = -1;   // 0-100, -1 = unknown (bar hidden)
};

enum class PadStyle { PS4, Xbox };
enum class PadTheme { Dark, Light };

// Physical buttons an action can be bound to (index stored in the rlcoach_pad_* cvars)
enum PadButton { PB_None = 0, PB_South, PB_East, PB_West, PB_North, PB_L1, PB_R1, PB_L3, PB_R3, PB_Count };

// Which button does what. Defaults are Rocket League's default controller binds.
struct PadLayout
{
	int jump = PB_South;
	int boost = PB_East;
	int slide = PB_West;      // powerslide / free air roll
	int rollLeft = PB_None;   // directional air roll
	int rollRight = PB_None;
};

namespace ControllerOverlay
{
	constexpr float kWidth = 240.0f;    // size at scale 1
	constexpr float kHeight = 178.0f;

	void Draw(CanvasWrapper& canvas, const PadInput& in, PadStyle style, PadTheme theme, const PadLayout& layout,
		float x, float y, float scale);
	const char* ButtonName(int button, PadStyle style);
}
