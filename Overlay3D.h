#pragma once
// Small 3D drawing helpers for the in-game overlays (game thread only).

#include "bakkesmod/wrappers/canvaswrapper.h"
#include "bakkesmod/wrappers/wrapperstructs.h"
#include <cmath>

namespace Overlay3D
{
	constexpr float kBallRadius = 92.75f;

	inline bool ToScreen(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd, const Vector& p, Vector2F& out)
	{
		Vector rel = p - camLoc;
		if (Vector::dot(rel, camFwd) < 10.0f) return false;   // behind the camera
		out = canvas.ProjectF(p);
		return true;
	}

	// A line with a soft dark edge under it, so it reads on bright and dark backgrounds
	inline void OutlinedLine(CanvasWrapper& canvas, Vector2F a, Vector2F b, float width,
		unsigned char r, unsigned char g, unsigned char bl, unsigned char alpha)
	{
		canvas.SetColor(0, 0, 0, static_cast<char>(alpha * 0.45f));
		canvas.DrawLine(a, b, width + 2.5f);
		canvas.SetColor(r, g, bl, alpha);
		canvas.DrawLine(a, b, width);
	}

	// A hollow ring lying on the ball's surface, centered where `normal` (from the ball center) points.
	// It sits a few units off the surface so it never sinks into the ball. Parts on the far side of the
	// ball (hidden in reality) are drawn faint.
	inline void BallRing(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd,
		const Vector& ballCenter, Vector normal, unsigned char r, unsigned char g, unsigned char b, float alpha = 1.0f)
	{
		float len = normal.magnitude();
		if (len < 1e-3f) return;
		normal = normal * (1.0f / len);
		Vector up(0, 0, 1);
		Vector e1 = Vector::cross(normal, up);
		if (e1.magnitude() < 0.1f) e1 = Vector::cross(normal, Vector(1, 0, 0));
		e1.normalize();
		Vector e2 = Vector::cross(normal, e1);

		const float lift = kBallRadius + 3.5f;   // just off the surface
		const float ang = 0.27f;                 // ring size: about a quarter of the ball across
		const int segs = 28;
		const float ca = std::cos(ang), sa = std::sin(ang);

		Vector2F prev{};
		bool prevOk = false, prevFront = true;
		for (int i = 0; i <= segs; i++)
		{
			float u = i * 6.2831853f / segs;
			Vector dir = normal * ca + (e1 * std::cos(u) + e2 * std::sin(u)) * sa;
			Vector p = ballCenter + dir * lift;
			bool front = Vector::dot(dir, camLoc - p) > 0.0f;
			Vector2F s;
			bool ok = ToScreen(canvas, camLoc, camFwd, p, s);
			if (ok && prevOk)
			{
				bool bothFront = front && prevFront;
				unsigned char a = static_cast<unsigned char>(255.0f * alpha * (bothFront ? 1.0f : 0.3f));
				OutlinedLine(canvas, prev, s, bothFront ? 3.0f : 2.0f, r, g, b, a);
			}
			prev = s;
			prevOk = ok;
			prevFront = front;
		}
	}

	// Faint outline of the ball, e.g. where it was when you hit it
	inline void BallGhost(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd, const Vector& center,
		unsigned char r, unsigned char g, unsigned char b, unsigned char alpha)
	{
		Vector2F c;
		if (!ToScreen(canvas, camLoc, camFwd, center, c)) return;
		Vector right = Vector::cross(camFwd, Vector(0, 0, 1));
		if (right.magnitude() < 0.1f) right = Vector(1, 0, 0);
		right.normalize();
		Vector2F edge;
		if (!ToScreen(canvas, camLoc, camFwd, center + right * kBallRadius, edge)) return;
		float rad = std::sqrt((edge.X - c.X) * (edge.X - c.X) + (edge.Y - c.Y) * (edge.Y - c.Y));
		const int segs = 32;
		canvas.SetColor(r, g, b, alpha);
		for (int i = 0; i < segs; i++)
		{
			float a0 = i * 6.2831853f / segs, a1 = (i + 1) * 6.2831853f / segs;
			canvas.DrawLine(Vector2F{ c.X + rad * std::cos(a0), c.Y + rad * std::sin(a0) },
				Vector2F{ c.X + rad * std::cos(a1), c.Y + rad * std::sin(a1) }, 1.5f);
		}
	}

	// A car's axes: forward, right, up
	struct Basis { Vector f{ 1, 0, 0 }, r{ 0, 1, 0 }, u{ 0, 0, 1 }; };

	// From Unreal rotator units (65536 = full turn)
	inline Basis FromRotator(int pitch, int yaw, int roll)
	{
		const float k = 3.14159265f / 32768.0f;
		float sp = std::sin(pitch * k), cp = std::cos(pitch * k);
		float sy = std::sin(yaw * k), cy = std::cos(yaw * k);
		float sr = std::sin(roll * k), cr = std::cos(roll * k);
		Basis b;
		b.f = Vector(cp * cy, cp * sy, sp);
		b.r = Vector(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
		b.u = Vector(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);
		return b;
	}

	// Axes pointing `forward`, rolled to stay as close to `upHint` as possible
	inline Basis Aim(Vector forward, Vector upHint)
	{
		Basis b;
		forward.normalize();
		Vector up = upHint - forward * Vector::dot(upHint, forward);
		if (up.magnitude() < 0.1f) { up = Vector(0, 0, 1) - forward * forward.Z; }
		if (up.magnitude() < 0.1f) up = Vector(1, 0, 0);
		up.normalize();
		b.f = forward;
		b.u = up;
		b.r = Vector::cross(up, forward);   // Unreal is left-handed: right = up x forward
		b.r.normalize();
		return b;
	}

	// Wireframe box (a car hitbox): center, axes, half sizes. The front face is drawn bolder.
	inline void HitboxWire(CanvasWrapper& canvas, const Vector& camLoc, const Vector& camFwd,
		const Vector& center, const Basis& b, const Vector& half, unsigned char r, unsigned char g, unsigned char bl, float alpha = 1.0f)
	{
		Vector2F s[8];
		bool ok[8];
		for (int i = 0; i < 8; i++)
		{
			float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f, sz = (i & 4) ? 1.0f : -1.0f;
			Vector p = center + b.f * (sx * half.X) + b.r * (sy * half.Y) + b.u * (sz * half.Z);
			ok[i] = ToScreen(canvas, camLoc, camFwd, p, s[i]);
		}
		const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
		for (const auto& e : edges)
		{
			if (!ok[e[0]] || !ok[e[1]]) continue;
			bool front = (e[0] & 1) && (e[1] & 1);   // both corners on the +forward face
			unsigned char a = static_cast<unsigned char>(255.0f * alpha * (front ? 1.0f : 0.75f));
			OutlinedLine(canvas, s[e[0]], s[e[1]], front ? 3.0f : 1.8f, r, g, bl, a);
		}
		// Nose arrow so the facing is obvious
		Vector nose = center + b.f * half.X;
		Vector2F n0, n1;
		if (ToScreen(canvas, camLoc, camFwd, nose, n0) && ToScreen(canvas, camLoc, camFwd, nose + b.f * 40.0f, n1))
			OutlinedLine(canvas, n0, n1, 2.0f, r, g, bl, static_cast<unsigned char>(200.0f * alpha));
	}
}
