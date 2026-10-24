#pragma once
// RL Coach's look for its ImGui windows (Oct 24 redesign): deep navy, slightly lighter panels, thin blue-grey
// outlines, one bright cyan accent for the thing to press. Grades have their own colors (5 cyan, 4 green, 3 amber,
// 2 orange, 1 red). Push a Scope before drawing and it's undone when the Scope goes away, so other plugins keep
// their style. Fonts are in UiFonts.h; everything here still works (in the default font) if they didn't load.

#include "IMGUI/imgui.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace UiStyle
{
	// The viewer's size (Oct 24: at 1.0 it took over a quarter of a 1080p screen). Everything in the window is drawn at
	// this scale; the fonts in UiFonts.h are loaded at the matching sizes, so change both together.
	inline constexpr float kScale = 0.8f;

	inline ImVec4 Hex(unsigned rgb, float a = 1.0f)
	{
		return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
	}
	inline ImU32 U32(const ImVec4& c, float alpha = -1.0f)
	{
		return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, alpha >= 0.0f ? alpha : c.w));
	}

	// ---- The palette ----
	inline const ImVec4 kBg = Hex(0x0B1220);          // window
	inline const ImVec4 kPanel = Hex(0x111B2E);       // cards, the open tab
	inline const ImVec4 kPanel2 = Hex(0x0E1727);      // the timeline card, stat tiles
	inline const ImVec4 kLine = Hex(0x23395A);        // outlines
	inline const ImVec4 kLineSoft = Hex(0x172741);    // quieter outlines
	inline const ImVec4 kTrack = Hex(0x18263D);       // the empty part of the gauge
	inline const ImVec4 kAccent = Hex(0x38BDF8);      // the one bright color: the main button, selected things
	inline const ImVec4 kAccentDim = Hex(0x1B5E85);
	inline const ImVec4 kAccentText = Hex(0x7DD3FC);
	inline const ImVec4 kOnAccent = Hex(0x04121F);    // text on an accent button
	inline const ImVec4 kText = Hex(0xE6EDF7);
	inline const ImVec4 kSub = Hex(0xA9B8CF);         // secondary text
	inline const ImVec4 kMuted = Hex(0x8DA0BC);       // labels
	inline const ImVec4 kRoot = Hex(0xFB923C);        // the root cause marker

	inline const ImVec4 kGood = Hex(0x4ADE80);
	inline const ImVec4 kOk = Hex(0x38BDF8);
	inline const ImVec4 kWarn = Hex(0xFBBF24);
	inline const ImVec4 kBad = Hex(0xF87171);
	inline const ImVec4 kGold = Hex(0xFBBF24);

	struct Scope
	{
		int colors = 0, vars = 0;
		explicit Scope(bool windowToo = true)
		{
			auto c = [&](ImGuiCol idx, ImVec4 v) { ImGui::PushStyleColor(idx, v); colors++; };
			auto f = [&](ImGuiStyleVar idx, float v) { ImGui::PushStyleVar(idx, v); vars++; };
			auto v2 = [&](ImGuiStyleVar idx, ImVec2 v) { ImGui::PushStyleVar(idx, v); vars++; };
			if (windowToo)
			{
				c(ImGuiCol_WindowBg, Hex(0x0B1220, 0.98f));
				c(ImGuiCol_TitleBg, Hex(0x0E1727));
				c(ImGuiCol_TitleBgActive, Hex(0x0E1727));
				c(ImGuiCol_TitleBgCollapsed, Hex(0x0E1727, 0.9f));
				c(ImGuiCol_Border, kLine);
				f(ImGuiStyleVar_WindowRounding, 12.0f * kScale);
				f(ImGuiStyleVar_WindowBorderSize, 1.0f);
				v2(ImGuiStyleVar_WindowPadding, ImVec2(18 * kScale, 14 * kScale));
			}
			c(ImGuiCol_Text, kText);
			c(ImGuiCol_TextDisabled, kMuted);
			c(ImGuiCol_ChildBg, kPanel);
			c(ImGuiCol_FrameBg, Hex(0x15223A));
			c(ImGuiCol_FrameBgHovered, Hex(0x1C2D4B));
			c(ImGuiCol_FrameBgActive, Hex(0x22385C));
			c(ImGuiCol_Button, Hex(0x15223A));
			c(ImGuiCol_ButtonHovered, Hex(0x1F3350));
			c(ImGuiCol_ButtonActive, kAccentDim);
			c(ImGuiCol_Header, Hex(0x1B5E85, 0.45f));
			c(ImGuiCol_HeaderHovered, Hex(0x1F3350));
			c(ImGuiCol_HeaderActive, Hex(0x1B5E85, 0.8f));
			c(ImGuiCol_SliderGrab, kAccent);
			c(ImGuiCol_SliderGrabActive, kAccentText);
			c(ImGuiCol_CheckMark, kAccent);
			c(ImGuiCol_Separator, kLineSoft);
			c(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
			c(ImGuiCol_ScrollbarGrab, Hex(0x1F3350));
			c(ImGuiCol_ScrollbarGrabHovered, Hex(0x2A456B));
			c(ImGuiCol_PopupBg, Hex(0x0E1727, 0.98f));
			f(ImGuiStyleVar_FrameRounding, 7.0f * kScale);
			f(ImGuiStyleVar_ChildRounding, 10.0f * kScale);
			f(ImGuiStyleVar_GrabRounding, 6.0f);
			f(ImGuiStyleVar_PopupRounding, 8.0f);
			f(ImGuiStyleVar_ScrollbarRounding, 6.0f);
			f(ImGuiStyleVar_FrameBorderSize, 0.0f);
			f(ImGuiStyleVar_GrabMinSize, 10.0f);
			v2(ImGuiStyleVar_FramePadding, ImVec2(10 * kScale, 5 * kScale));
			v2(ImGuiStyleVar_ItemSpacing, ImVec2(8 * kScale, 7 * kScale));
		}
		~Scope()
		{
			ImGui::PopStyleVar(vars);
			ImGui::PopStyleColor(colors);
		}
	};

	// Small muted capitals with a rule after them, to start a section
	inline void Heading(const char* text)
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
		ImVec2 p = ImGui::GetItemRectMax();
		ImVec2 a = ImGui::GetItemRectMin();
		float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 8, (a.y + p.y) * 0.5f), ImVec2(right, (a.y + p.y) * 0.5f), U32(kLineSoft));
	}

	// A rounded colored tag with text, e.g. a grade
	inline void Pill(const std::string& text, ImVec4 color, float minWidth = 0.0f)
	{
		ImVec2 ts = ImGui::CalcTextSize(text.c_str());
		float w = std::max(ts.x + 14.0f * kScale, minWidth), h = ts.y + 4.0f * kScale;
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(color, 0.16f), h * 0.5f);
		dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + 2.0f * kScale), U32(color), text.c_str());
		ImGui::Dummy(ImVec2(w, h));
	}

	// The main action on screen: filled cyan with dark text
	inline bool AccentButton(const char* label, ImVec2 size = ImVec2(0, 0))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentText);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, Hex(0x0EA5E9));
		ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
		bool r = ImGui::Button(label, size);
		ImGui::PopStyleColor(4);
		return r;
	}

	// A second action next to the main one: outlined, colored text
	inline bool OutlineButton(const char* label, ImVec2 size = ImVec2(0, 0), ImVec4 color = kAccent)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.14f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.26f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(std::min(1.0f, color.x * 0.6f + 0.4f), std::min(1.0f, color.y * 0.6f + 0.4f), std::min(1.0f, color.z * 0.6f + 0.4f), 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Border, color);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		bool r = ImGui::Button(label, size);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(5);
		return r;
	}

	// A quiet text-only button (small extras like "Think harder")
	inline bool LinkButton(const char* label)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x1F3350));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDim);
		ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
		bool r = ImGui::SmallButton(label);
		ImGui::PopStyleColor(4);
		return r;
	}

	inline ImVec4 GradeColor(int overall)
	{
		if (overall >= 5) return Hex(0x22D3EE);
		if (overall == 4) return kGood;
		if (overall == 3) return kWarn;
		if (overall == 2) return Hex(0xFB923C);
		if (overall == 1) return kBad;
		return kMuted;
	}

	inline ImVec4 SegGradeColor(const std::string& g)
	{
		if (g == "Clean" || g == "Calculated") return kGood;
		if (g == "Solid") return Hex(0x5EC8E5);
		if (g == "Sloppy") return kWarn;
		return Hex(0xFB923C);
	}

	// ---- Drawn pieces for the review screen ----

	// The grade gauge: a 270 degree ring, filled from 1 to 5 in the grade's color
	inline void Gauge(ImDrawList* dl, ImVec2 c, float r, float thick, int grade, ImVec4 color)
	{
		const float a0 = 3.14159265f * 0.75f, sweep = 3.14159265f * 1.5f;
		auto arc = [&](float from, float to, ImU32 col) {
			if (to <= from) return;
			const int n = std::max(6, static_cast<int>((to - from) / sweep * 64.0f));
			dl->PathArcTo(c, r, a0 + sweep * from, a0 + sweep * to, n);
			dl->PathStroke(col, false, thick);
			// round ends
			for (float t : { from, to })
				dl->AddCircleFilled(ImVec2(c.x + r * std::cos(a0 + sweep * t), c.y + r * std::sin(a0 + sweep * t)), thick * 0.5f, col, 16);
		};
		arc(0.0f, 1.0f, U32(kTrack));
		const float frac = std::clamp(grade / 5.0f, 0.0f, 1.0f);
		if (frac > 0.0f)
		{
			arc(0.0f, frac, U32(color, 0.18f));   // a soft glow under the fill
			dl->PathArcTo(c, r, a0, a0 + sweep * frac, std::max(6, static_cast<int>(frac * 64.0f)));
			dl->PathStroke(U32(color, 0.22f), false, thick + 6.0f);
			arc(0.0f, frac, U32(color));
		}
	}

	// A small glowing diamond (timeline markers)
	inline void Diamond(ImDrawList* dl, ImVec2 c, float r, ImVec4 color)
	{
		for (int g = 3; g >= 1; g--)
		{
			float rr = r + g * 2.5f;
			dl->AddQuadFilled(ImVec2(c.x, c.y - rr), ImVec2(c.x + rr, c.y), ImVec2(c.x, c.y + rr), ImVec2(c.x - rr, c.y), U32(color, 0.10f));
		}
		dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), U32(color));
	}

	// A spinning arc for "thinking"
	inline void Spinner(ImDrawList* dl, ImVec2 c, float r, float thick, ImVec4 color)
	{
		dl->AddCircle(c, r, U32(kTrack), 32, thick);
		const float t = static_cast<float>(ImGui::GetTime()) * 4.0f;
		dl->PathArcTo(c, r, t, t + 1.8f, 16);
		dl->PathStroke(U32(color), false, thick);
	}
}
