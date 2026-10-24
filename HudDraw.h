#pragma once
// Flat 2D drawing helpers for the in-game panel and controller overlay (game thread only).

#include "bakkesmod/wrappers/canvaswrapper.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace HudDraw
{
	struct Rgba { unsigned char r, g, b, a; };

	inline void SetColor(CanvasWrapper& c, Rgba col) { c.SetColor(col.r, col.g, col.b, col.a); }

	inline void Box(CanvasWrapper& c, float x, float y, float w, float h, Rgba col)
	{
		if (w <= 0 || h <= 0) return;
		SetColor(c, col);
		c.SetPosition(Vector2F{ x, y });
		c.FillBox(Vector2F{ w, h });
	}

	// Filled pie slice from angle a0 to a1 (radians), as a small triangle fan
	inline void Fan(CanvasWrapper& c, float cx, float cy, float r, float a0, float a1, int steps)
	{
		for (int i = 0; i < steps; i++)
		{
			float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
			c.FillTriangle(Vector2F{ cx, cy }, Vector2F{ cx + r * std::cos(t0), cy + r * std::sin(t0) },
				Vector2F{ cx + r * std::cos(t1), cy + r * std::sin(t1) });
		}
	}

	// Rectangle with rounded corners
	inline void RoundBox(CanvasWrapper& c, float x, float y, float w, float h, float r, Rgba col)
	{
		r = std::max(0.0f, std::min(r, std::min(w, h) * 0.5f));
		Box(c, x + r, y, w - 2 * r, h, col);
		Box(c, x, y + r, r, h - 2 * r, col);
		Box(c, x + w - r, y + r, r, h - 2 * r, col);
		if (r <= 0) return;
		SetColor(c, col);
		const float pi = 3.14159265f;
		int steps = std::max(3, static_cast<int>(r / 2));
		Fan(c, x + r, y + r, r, pi, 1.5f * pi, steps);
		Fan(c, x + w - r, y + r, r, 1.5f * pi, 2.0f * pi, steps);
		Fan(c, x + w - r, y + h - r, r, 0.0f, 0.5f * pi, steps);
		Fan(c, x + r, y + h - r, r, 0.5f * pi, pi, steps);
	}

	inline void Disc(CanvasWrapper& c, float cx, float cy, float r, Rgba col)
	{
		SetColor(c, col);
		Fan(c, cx, cy, r, 0.0f, 6.2831853f, std::max(12, static_cast<int>(r * 1.2f)));
	}

	inline void Ring(CanvasWrapper& c, float cx, float cy, float r, float width, Rgba col)
	{
		SetColor(c, col);
		int steps = std::max(16, static_cast<int>(r * 1.5f));
		for (int i = 0; i < steps; i++)
		{
			float a0 = i * 6.2831853f / steps, a1 = (i + 1) * 6.2831853f / steps;
			c.DrawLine(Vector2F{ cx + r * std::cos(a0), cy + r * std::sin(a0) },
				Vector2F{ cx + r * std::cos(a1), cy + r * std::sin(a1) }, width);
		}
	}

	// An arc from angle a0 to a1 (radians, clockwise on screen), drawn as short line segments
	inline void Arc(CanvasWrapper& c, float cx, float cy, float r, float a0, float a1, float width, Rgba col)
	{
		SetColor(c, col);
		int steps = std::max(6, static_cast<int>(std::fabs(a1 - a0) * r * 0.5f));
		for (int i = 0; i < steps; i++)
		{
			float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
			c.DrawLine(Vector2F{ cx + r * std::cos(t0), cy + r * std::sin(t0) }, Vector2F{ cx + r * std::cos(t1), cy + r * std::sin(t1) }, width);
		}
	}

	// A one-pixel outline with rounded corners
	inline void RoundOutline(CanvasWrapper& c, float x, float y, float w, float h, float r, Rgba col)
	{
		r = std::max(0.0f, std::min(r, std::min(w, h) * 0.5f));
		SetColor(c, col);
		c.DrawLine(Vector2F{ x + r, y }, Vector2F{ x + w - r, y }, 1.0f);
		c.DrawLine(Vector2F{ x + r, y + h }, Vector2F{ x + w - r, y + h }, 1.0f);
		c.DrawLine(Vector2F{ x, y + r }, Vector2F{ x, y + h - r }, 1.0f);
		c.DrawLine(Vector2F{ x + w, y + r }, Vector2F{ x + w, y + h - r }, 1.0f);
		const float pi = 3.14159265f;
		Arc(c, x + r, y + r, r, pi, 1.5f * pi, 1.0f, col);
		Arc(c, x + w - r, y + r, r, 1.5f * pi, 2.0f * pi, 1.0f, col);
		Arc(c, x + w - r, y + h - r, r, 0.0f, 0.5f * pi, 1.0f, col);
		Arc(c, x + r, y + h - r, r, 0.5f * pi, pi, 1.0f, col);
	}

	inline Vector2F TextSize(CanvasWrapper& c, const std::string& s, float scale)
	{
		return c.GetStringSize(s, scale, scale);
	}

	inline void Text(CanvasWrapper& c, float x, float y, const std::string& s, float scale, Rgba col, bool shadow = true)
	{
		SetColor(c, col);
		c.SetPosition(Vector2F{ x, y });
		c.DrawString(s, scale, scale, shadow);
	}

	// Text centered on (cx, cy)
	inline void TextCentered(CanvasWrapper& c, float cx, float cy, const std::string& s, float scale, Rgba col, bool shadow = false)
	{
		Vector2F sz = TextSize(c, s, scale);
		Text(c, cx - sz.X * 0.5f, cy - sz.Y * 0.5f, s, scale, col, shadow);
	}
}
