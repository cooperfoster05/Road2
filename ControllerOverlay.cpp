#include "pch.h"
#include "ControllerOverlay.h"
#include "HudDraw.h"
#include "PadShapes.h"

#include <algorithm>
#include <cmath>

using namespace HudDraw;

namespace
{
	struct Theme
	{
		Rgba body, bodyEdge, face, well, part, trig, label, cap, rim, bevel;
	};
	const Theme kDarkTheme{ { 30, 33, 42, 255 }, { 52, 57, 70, 255 }, { 38, 42, 53, 255 }, { 20, 22, 28, 255 }, { 48, 53, 66, 255 },
		{ 24, 26, 33, 255 }, { 150, 158, 175, 255 }, { 58, 63, 78, 255 }, { 10, 11, 14, 255 }, { 44, 48, 60, 255 } };
	const Theme kLightTheme{ { 232, 234, 238, 255 }, { 205, 208, 215, 255 }, { 218, 221, 228, 255 }, { 40, 43, 52, 255 }, { 60, 64, 76, 255 },
		{ 200, 203, 210, 255 }, { 90, 96, 110, 255 }, { 70, 75, 88, 255 }, { 28, 30, 36, 255 }, { 196, 199, 207, 255 } };
	const Rgba kAccent{ 90, 170, 255, 255 };
	const Rgba kWhite{ 255, 255, 255, 255 };
	const Rgba kInk{ 20, 22, 28, 255 };

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

	// Blend two opaque colors (no see-through, so overlapping shapes never show seams)
	Rgba Mix(Rgba a, Rgba b, float k)
	{
		auto m = [k](unsigned char x, unsigned char y) { return static_cast<unsigned char>(x * k + y * (1.0f - k)); };
		return { m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255 };
	}

	void Line(CanvasWrapper& c, float x0, float y0, float x1, float y1, float w, Rgba col)
	{
		SetColor(c, col);
		c.DrawLine(Vector2F{ x0, y0 }, Vector2F{ x1, y1 }, w);
	}

	template <size_t N>
	void Shape(CanvasWrapper& c, const float (&tris)[N][6], float x, float y, float s, Rgba col)
	{
		SetColor(c, col);
		for (size_t i = 0; i < N; i++)
		{
			const float* t = tris[i];
			c.FillTriangle(Vector2F{ x + t[0] * s, y + t[1] * s }, Vector2F{ x + t[2] * s, y + t[3] * s }, Vector2F{ x + t[4] * s, y + t[5] * s });
		}
	}

	void FaceSymbol(CanvasWrapper& c, int b, PadStyle st, float cx, float cy, float s, Rgba col)
	{
		if (st == PadStyle::Xbox)
		{
			const char* letter = b == PB_South ? "A" : b == PB_East ? "B" : b == PB_West ? "X" : "Y";
			TextCentered(c, cx, cy, letter, 0.85f * s, col);
			return;
		}
		float r = 3.4f * s, w = 1.4f * s;
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

void ControllerOverlay::Draw(CanvasWrapper& c, const PadInput& in, PadStyle st, PadTheme theme, const PadLayout& lay,
	float x, float y, float s)
{
	const bool ps = st == PadStyle::PS4;
	const Theme& T = theme == PadTheme::Light ? kLightTheme : kDarkTheme;
	auto X = [&](float u) { return x + u * s; };
	auto Y = [&](float v) { return y + v * s; };
	auto RBox = [&](float ux, float uy, float uw, float uh, float ur, Rgba col) { RoundBox(c, X(ux), Y(uy), uw * s, uh * s, ur * s, col); };
	auto DiscU = [&](float ux, float uy, float ur, Rgba col) { Disc(c, X(ux), Y(uy), ur * s, col); };
	auto RingU = [&](float ux, float uy, float ur, float w, Rgba col) { Ring(c, X(ux), Y(uy), ur * s, w * s, col); };

	// Which physical buttons are down, from the actions you bound to them
	bool down[PB_Count] = {};
	auto press = [&](int b, bool on) { if (on && b > PB_None && b < PB_Count) down[b] = true; };
	press(lay.jump, in.jump);
	press(lay.boost, in.boost);
	press(lay.slide, in.handbrake);
	press(lay.rollLeft, !in.handbrake && in.roll < -0.5f);
	press(lay.rollRight, !in.handbrake && in.roll > 0.5f);

	// Triggers behind the body, filling from the bottom: reverse (L2/LT) and throttle (R2/RT)
	for (int side = 0; side < 2; side++)
	{
		bool left = side == 0;
		float val = std::max(0.0f, std::min(1.0f, left ? -in.throttle : in.throttle));
		float tx = ps ? (left ? 36.0f : 164.0f) : (left ? 40.0f : 160.0f);
		float ty = ps ? 8.0f : 2.0f, tw = 40.0f, th = ps ? 30.0f : 38.0f;
		RBox(tx - 1.5f, ty - 1.5f, tw + 3.0f, th + 3.0f, 11.5f, T.rim);
		RBox(tx, ty, tw, th, 10.0f, T.trig);
		if (val > 0.02f)
		{
			float fh = th * val;
			RBox(tx, ty + th - fh, tw, fh, std::min(10.0f, fh * 0.5f), left ? Rgba{ 255, 150, 90, 255 } : Rgba{ 90, 220, 140, 255 });
		}
		const char* lbl = ps ? (left ? "L2" : "R2") : (left ? "LT" : "RT");
		TextCentered(c, X(tx + tw * 0.5f), Y(ty + 11.0f), lbl, 0.7f * s, val > 0.5f ? kInk : T.label);
	}

	// Bumpers peek out above the shoulders
	float bumpY = ps ? 30.0f : 32.0f;
	RBox(34, bumpY, 56, 10, 5, down[PB_L1] ? kAccent : T.part);
	RBox(150, bumpY, 56, 10, 5, down[PB_R1] ? kAccent : T.part);

	// Body: dark rim, bevel, face
	if (ps)
	{
		Shape(c, kDs4Rim, x, y, s, T.rim);
		Shape(c, kDs4Bevel, x, y, s, T.bevel);
		Shape(c, kDs4Face, x, y, s, T.body);
	}
	else
	{
		Shape(c, kXboxRim, x, y, s, T.rim);
		Shape(c, kXboxBevel, x, y, s, T.bevel);
		Shape(c, kXboxFace, x, y, s, T.body);
	}

	// Layout differs: PS4 has the D-pad top-left and both sticks low; Xbox has the left stick top-left
	float dpx, dpy, lsx, lsy, rsx, rsy, fx, fy;
	if (ps)
	{
		RBox(86, 38, 68, 40, 7, T.face);              // touchpad
		RBox(72, 42, 6, 12, 3, T.part);               // share
		RBox(162, 42, 6, 12, 3, T.part);              // options
		DiscU(120, 98, 6, T.part);                    // home button
		dpx = 58; dpy = 70; lsx = 88; lsy = 106; rsx = 152; rsy = 106; fx = 182; fy = 70;
	}
	else
	{
		DiscU(120, 56, 9, T.part);                    // guide button
		RingU(120, 56, 9, 1.2f, theme == PadTheme::Dark ? T.bevel : T.rim);
		DiscU(120, 56, 4, T.label);
		DiscU(105, 76, 3.5f, T.part);                 // view
		DiscU(135, 76, 3.5f, T.part);                 // menu
		lsx = 62; lsy = 70; dpx = 94; dpy = 106; rsx = 148; rsy = 106; fx = 180; fy = 70;
	}

	// D-pad (just for looks)
	DiscU(dpx, dpy, 21, T.face);
	RBox(dpx - 5, dpy - 15, 10, 30, 2.5f, T.part);
	RBox(dpx - 15, dpy - 5, 30, 10, 2.5f, T.part);

	// Sticks: the cap moves with the left stick (steer / yaw and pitch). The right stick is the camera, which isn't recorded.
	float stickX = std::fabs(in.steer) >= std::fabs(in.yaw) ? in.steer : in.yaw;
	auto stick = [&](float cx, float cy, float ix, float iy, bool clicked) {
		float mag = std::sqrt(ix * ix + iy * iy);
		if (mag > 1.0f) { ix /= mag; iy /= mag; }
		bool on = mag > 0.08f;
		DiscU(cx, cy, 19, T.well);
		if (on) Line(c, X(cx), Y(cy), X(cx + ix * 13), Y(cy + iy * 13), 2.0f * s, kAccent);
		float kx = cx + ix * 7, ky = cy + iy * 7;
		DiscU(kx, ky, 13, T.cap);
		RingU(kx, ky, 13, 1.4f, (on || clicked) ? kAccent : T.bodyEdge);
		DiscU(kx, ky, 8.5f, on ? T.cap : T.part);
		RingU(kx, ky, 8.5f, 0.8f, T.bodyEdge);
	};
	stick(lsx, lsy, stickX, in.pitch, down[PB_L3]);
	stick(rsx, rsy, 0, 0, down[PB_R3]);

	// Face buttons
	DiscU(fx, fy, 24, T.face);
	const int order[4] = { PB_North, PB_West, PB_East, PB_South };
	const float off[4][2] = { { 0, -15 }, { -15, 0 }, { 15, 0 }, { 0, 15 } };
	for (int i = 0; i < 4; i++)
	{
		int b = order[i];
		float bx = fx + off[i][0], by = fy + off[i][1];
		Rgba col = FaceColor(b, st);
		if (down[b])
		{
			DiscU(bx, by, 10.5f, Mix(col, T.face, 0.45f));   // glow
			DiscU(bx, by, 8, col);
			FaceSymbol(c, b, st, X(bx), Y(by), s, kWhite);
		}
		else
		{
			DiscU(bx, by, 8, T.well);
			FaceSymbol(c, b, st, X(bx), Y(by), s, col);
		}
	}

	// Air roll tag under the pad
	if (std::fabs(in.roll) > 0.3f)
	{
		const char* tag = in.handbrake ? "FREE AIR ROLL" : (in.roll < 0 ? "AIR ROLL LEFT" : "AIR ROLL RIGHT");
		TextCentered(c, X(120), Y(170), tag, 0.7f * s, Rgba{ 190, 150, 255, 255 }, true);
	}
}
