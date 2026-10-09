#include "pch.h"
#include "ControllerOverlay.h"
#include "HudDraw.h"

#include <algorithm>
#include <cmath>

using namespace HudDraw;

namespace
{
	const Rgba kBody{ 24, 27, 35, 255 };
	const Rgba kPart{ 40, 44, 56, 255 };
	const Rgba kEdge{ 72, 78, 96, 255 };
	const Rgba kDim{ 120, 126, 140, 255 };

	// PS4 face colors (cross, circle, square, triangle) and Xbox (A, B, X, Y)
	Rgba FaceColor(int b, PadStyle st)
	{
		if (st == PadStyle::PS4)
		{
			switch (b)
			{
			case PB_South: return { 124, 178, 232, 255 };
			case PB_East:  return { 255, 102, 102, 255 };
			case PB_West:  return { 214, 145, 193, 255 };
			default:       return { 64, 226, 160, 255 };
			}
		}
		switch (b)
		{
		case PB_South: return { 96, 185, 64, 255 };
		case PB_East:  return { 224, 70, 50, 255 };
		case PB_West:  return { 52, 132, 232, 255 };
		default:       return { 245, 200, 30, 255 };
		}
	}

	Rgba Fade(Rgba c, float k) { return { c.r, c.g, c.b, static_cast<unsigned char>(c.a * k) }; }

	void Line(CanvasWrapper& c, float x0, float y0, float x1, float y1, float w, Rgba col)
	{
		SetColor(c, col);
		c.DrawLine(Vector2F{ x0, y0 }, Vector2F{ x1, y1 }, w);
	}

	void FaceSymbol(CanvasWrapper& c, int b, PadStyle st, float cx, float cy, float s, Rgba col)
	{
		if (st == PadStyle::Xbox)
		{
			const char* letter = b == PB_South ? "A" : b == PB_East ? "B" : b == PB_West ? "X" : "Y";
			TextCentered(c, cx, cy, letter, 0.85f * s, col);
			return;
		}
		float r = 3.6f * s, w = 1.6f * s;
		switch (b)
		{
		case PB_South:
			Line(c, cx - r, cy - r, cx + r, cy + r, w, col);
			Line(c, cx - r, cy + r, cx + r, cy - r, w, col);
			break;
		case PB_East:
			Ring(c, cx, cy, r, w, col);
			break;
		case PB_West:
			Line(c, cx - r, cy - r, cx + r, cy - r, w, col);
			Line(c, cx + r, cy - r, cx + r, cy + r, w, col);
			Line(c, cx + r, cy + r, cx - r, cy + r, w, col);
			Line(c, cx - r, cy + r, cx - r, cy - r, w, col);
			break;
		default:
			Line(c, cx, cy - r, cx + r, cy + r * 0.8f, w, col);
			Line(c, cx + r, cy + r * 0.8f, cx - r, cy + r * 0.8f, w, col);
			Line(c, cx - r, cy + r * 0.8f, cx, cy - r, w, col);
			break;
		}
	}

	void Stick(CanvasWrapper& c, float cx, float cy, float s, float ix, float iy, bool clicked, Rgba accent)
	{
		float r = 15.0f * s;
		Disc(c, cx, cy, r, kPart);
		Ring(c, cx, cy, r, 1.5f * s, clicked ? accent : kEdge);
		// faint crosshair
		Line(c, cx - r * 0.6f, cy, cx + r * 0.6f, cy, 1.0f, Fade(kEdge, 0.6f));
		Line(c, cx, cy - r * 0.6f, cx, cy + r * 0.6f, 1.0f, Fade(kEdge, 0.6f));
		float mag = std::sqrt(ix * ix + iy * iy);
		if (mag > 1.0f) { ix /= mag; iy /= mag; }
		float dx = cx + ix * 10.0f * s, dy = cy + iy * 10.0f * s;
		bool active = mag > 0.08f;
		if (active) Line(c, cx, cy, dx, dy, 2.0f * s, Fade(accent, 0.8f));
		Disc(c, dx, dy, 6.0f * s, active ? Rgba{ 245, 247, 250, 255 } : kDim);
	}

	void Trigger(CanvasWrapper& c, float x, float y, float w, float h, float s, float value, Rgba fill, const char* label)
	{
		RoundBox(c, x, y, w, h, 4.0f * s, kPart);
		value = std::max(0.0f, std::min(1.0f, value));
		if (value > 0.02f) RoundBox(c, x, y, w * value, h, 4.0f * s, fill);
		TextCentered(c, x + w * 0.5f, y + h * 0.5f, label, 0.7f * s, value > 0.5f ? Rgba{ 20, 22, 28, 255 } : kDim);
	}
}

const char* ControllerOverlay::ButtonName(int b, PadStyle st)
{
	bool ps = st == PadStyle::PS4;
	switch (b)
	{
	case PB_South: return ps ? "Cross (X)" : "A";
	case PB_East:  return ps ? "Circle" : "B";
	case PB_West:  return ps ? "Square" : "X";
	case PB_North: return ps ? "Triangle" : "Y";
	case PB_L1:    return ps ? "L1" : "LB";
	case PB_R1:    return ps ? "R1" : "RB";
	case PB_L3:    return ps ? "L3 (left stick click)" : "LS (left stick click)";
	case PB_R3:    return ps ? "R3 (right stick click)" : "RS (right stick click)";
	default:       return "Not bound";
	}
}

void ControllerOverlay::Draw(CanvasWrapper& c, const PadInput& in, PadStyle st, const PadLayout& lay, float x, float y, float s)
{
	bool ps = st == PadStyle::PS4;
	const Rgba accent{ 90, 170, 255, 255 };
	auto P = [&](float ux, float uy) { return Vector2F{ x + ux * s, y + uy * s }; };

	// Which physical buttons are down, from the actions you bound to them
	bool down[PB_Count] = {};
	auto press = [&](int b, bool on) { if (on && b > PB_None && b < PB_Count) down[b] = true; };
	press(lay.jump, in.jump);
	press(lay.boost, in.boost);
	press(lay.slide, in.handbrake);
	press(lay.rollLeft, !in.handbrake && in.roll < -0.5f);
	press(lay.rollRight, !in.handbrake && in.roll > 0.5f);

	// Triggers: throttle forward (R2/RT) and reverse (L2/LT)
	Trigger(c, P(28, 2).X, P(28, 2).Y, 50 * s, 15 * s, s, -in.throttle, Rgba{ 255, 150, 90, 255 }, ps ? "L2" : "LT");
	Trigger(c, P(152, 2).X, P(152, 2).Y, 50 * s, 15 * s, s, in.throttle, Rgba{ 90, 220, 140, 255 }, ps ? "R2" : "RT");

	// Body with two grips
	Disc(c, P(54, 100).X, P(54, 100).Y, 30 * s, kBody);
	Disc(c, P(176, 100).X, P(176, 100).Y, 30 * s, kBody);
	RoundBox(c, P(10, 24).X, P(10, 24).Y, 210 * s, 80 * s, 30 * s, kBody);

	// Bumpers
	RoundBox(c, P(26, 20).X, P(26, 20).Y, 54 * s, 9 * s, 4 * s, down[PB_L1] ? accent : kEdge);
	RoundBox(c, P(150, 20).X, P(150, 20).Y, 54 * s, 9 * s, 4 * s, down[PB_R1] ? accent : kEdge);
	TextCentered(c, P(53, 24.5f).X, P(53, 24.5f).Y, ps ? "L1" : "LB", 0.6f * s, down[PB_L1] ? Rgba{ 20, 22, 28, 255 } : Rgba{ 190, 195, 205, 255 });
	TextCentered(c, P(177, 24.5f).X, P(177, 24.5f).Y, ps ? "R1" : "RB", 0.6f * s, down[PB_R1] ? Rgba{ 20, 22, 28, 255 } : Rgba{ 190, 195, 205, 255 });

	// Where things sit differs: PS4 has the D-pad top-left, Xbox the left stick
	float lsx = ps ? 84 : 54, lsy = ps ? 82 : 52;
	float dpx = ps ? 54 : 84, dpy = ps ? 52 : 82;
	float rsx = 146, rsy = 82;
	float fx = 176, fy = 52;

	// D-pad (just for looks)
	RoundBox(c, P(dpx - 4, dpy - 13).X, P(dpx - 4, dpy - 13).Y, 8 * s, 26 * s, 2 * s, kPart);
	RoundBox(c, P(dpx - 13, dpy - 4).X, P(dpx - 13, dpy - 4).Y, 26 * s, 8 * s, 2 * s, kPart);

	// Middle: touchpad (PS4) or guide button (Xbox)
	if (ps) RoundBox(c, P(94, 30).X, P(94, 30).Y, 42 * s, 24 * s, 5 * s, Rgba{ 32, 36, 46, 255 });
	else Disc(c, P(115, 40).X, P(115, 40).Y, 7 * s, kPart);

	// Sticks: left stick shows steer / yaw and pitch; right stick is the camera (not recorded)
	float stickX = std::fabs(in.steer) >= std::fabs(in.yaw) ? in.steer : in.yaw;
	Stick(c, P(lsx, lsy).X, P(lsx, lsy).Y, s, stickX, in.pitch, down[PB_L3], accent);
	Stick(c, P(rsx, rsy).X, P(rsx, rsy).Y, s, 0, 0, down[PB_R3], accent);

	// Face buttons
	const int order[4] = { PB_North, PB_West, PB_East, PB_South };
	const float off[4][2] = { { 0, -15 }, { -15, 0 }, { 15, 0 }, { 0, 15 } };
	for (int i = 0; i < 4; i++)
	{
		int b = order[i];
		Vector2F p = P(fx + off[i][0], fy + off[i][1]);
		Rgba col = FaceColor(b, st);
		if (down[b])
		{
			Disc(c, p.X, p.Y, 8.5f * s, col);
			FaceSymbol(c, b, st, p.X, p.Y, s, Rgba{ 255, 255, 255, 255 });
		}
		else
		{
			Disc(c, p.X, p.Y, 8.5f * s, kPart);
			FaceSymbol(c, b, st, p.X, p.Y, s, Fade(col, 0.75f));
		}
	}

	// Boost meter and air roll tag under the pad
	float by = 126;
	if (in.boostAmount >= 0)
	{
		float frac = std::max(0.0f, std::min(1.0f, in.boostAmount / 100.0f));
		RoundBox(c, P(40, by).X, P(40, by).Y, 150 * s, 6 * s, 3 * s, kPart);
		if (frac > 0.01f) RoundBox(c, P(40, by).X, P(40, by).Y, 150 * s * frac, 6 * s, 3 * s,
			in.boost ? Rgba{ 255, 180, 60, 255 } : Rgba{ 230, 140, 40, 220 });
		Text(c, P(194, by - 3).X, P(194, by - 3).Y, std::to_string(static_cast<int>(in.boostAmount + 0.5f)), 0.7f * s, Rgba{ 230, 150, 60, 255 });
	}
	if (std::fabs(in.roll) > 0.3f)
	{
		const char* tag = in.handbrake ? "FREE AIR ROLL" : (in.roll < 0 ? "AIR ROLL LEFT" : "AIR ROLL RIGHT");
		TextCentered(c, P(115, 136).X, P(115, 136).Y, tag, 0.65f * s, Rgba{ 190, 150, 255, 255 }, true);
	}
}
