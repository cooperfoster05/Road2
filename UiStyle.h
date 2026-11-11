#pragma once
// Road2's look for its ImGui windows. Style A, "Kickoff" (Nov 3, Coop's pick from three directions on the design
// canvas): Rocket League's own visual language. Dark navy, the team blue for anything you can act on (main buttons,
// selected things, the engine), team orange for what to look at (the root cause, the 2 in ROAD2), condensed italic
// capitals for titles and big numbers, and one slant, 12 degrees, on badges, chips, buttons and timeline blocks.
// Grades keep their own colors (5 teal, 4 green, 3 amber, 2 orange, 1 red) and mean only the grade. Push a Scope before
// drawing and it's undone when the Scope goes away, so other plugins keep their style. Fonts are in UiFonts.h;
// everything here still works (in the default font) if they didn't load.

#include "IMGUI/imgui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace UiStyle
{
	// The viewer's size (Oct 24: at 1.0 it took over a quarter of a 1080p screen). Everything in the window is drawn at
	// this scale; the fonts in UiFonts.h are loaded at the matching sizes, so change both together.
	inline constexpr float kScale = 0.8f;
	// Nov 1: the window's own size on top of that. Set every frame by the window (GuiBase.cpp): the "Window size"
	// setting, times how far you've dragged the window narrower than it needs (the whole UI shrinks with it, down to
	// 0.6). Sc() is the scale to draw at; the fonts are scaled to match with SetWindowFontScale.
	inline float gUser = 1.0f;
	inline float Sc() { return kScale * gUser; }

	inline ImVec4 Hex(unsigned rgb, float a = 1.0f)
	{
		return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
	}
	inline ImU32 U32(const ImVec4& c, float alpha = -1.0f)
	{
		return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, alpha >= 0.0f ? alpha : c.w));
	}

	// ---- The palette (style A) ----
	inline const ImVec4 kBg = Hex(0x0B0F1A);          // window
	inline const ImVec4 kPanel = Hex(0x121A2C);       // cards, the open tab
	inline const ImVec4 kPanel2 = Hex(0x0E1322);      // the timeline card, stat tiles, rows, the header strip
	inline const ImVec4 kChip = Hex(0x1A2440);        // quiet chips and buttons
	inline const ImVec4 kLine = Hex(0x24314D);        // outlines
	inline const ImVec4 kLineSoft = Hex(0x1E2A44);    // quieter outlines
	inline const ImVec4 kTrack = Hex(0x1A2440);       // empty pips and tracks
	inline const ImVec4 kAccent = Hex(0x2E8BFF);      // team blue: anything you act on, selected things, the engine
	inline const ImVec4 kAccentDim = Hex(0x1B4F94);
	inline const ImVec4 kAccentText = Hex(0x7DB8FF);
	inline const ImVec4 kOnAccent = Hex(0x04101F);    // text on a blue (or any light) fill
	inline const ImVec4 kOrange = Hex(0xFF8A1F);      // team orange: what to look at
	inline const ImVec4 kText = Hex(0xEEF2F8);
	inline const ImVec4 kSub = Hex(0xA7B3C8);         // secondary text
	inline const ImVec4 kMuted = Hex(0x8592AB);       // labels
	inline const ImVec4 kRoot = Hex(0xFF8A1F);        // the root cause marker

	inline const ImVec4 kGood = Hex(0x3DDC84);
	inline const ImVec4 kOk = Hex(0x2E8BFF);
	inline const ImVec4 kWarn = Hex(0xFFB020);
	inline const ImVec4 kBad = Hex(0xFF5A5A);
	inline const ImVec4 kGold = Hex(0xFFB020);

	// The slant: tan(12 degrees). Every slanted shape leans by this much of its height.
	inline constexpr float kSlant = 0.2126f;
	// The condensed faces the helpers draw with (set every frame by the window from UiFonts; null = the current font)
	inline ImFont* gCaps = nullptr;      // Barlow Condensed Bold: labels, chips, button text
	inline ImFont* gHero = nullptr;      // Barlow Condensed ExtraBold Italic, large: grade badges, big numbers

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
				c(ImGuiCol_WindowBg, Hex(0x0B0F1A, 0.98f));
				c(ImGuiCol_TitleBg, Hex(0x0E1322));
				c(ImGuiCol_TitleBgActive, Hex(0x0E1322));
				c(ImGuiCol_TitleBgCollapsed, Hex(0x0E1322, 0.9f));
				c(ImGuiCol_Border, kLineSoft);
				f(ImGuiStyleVar_WindowRounding, 8.0f * Sc());
				f(ImGuiStyleVar_WindowBorderSize, 1.0f);
				v2(ImGuiStyleVar_WindowPadding, ImVec2(18 * Sc(), 14 * Sc()));
			}
			c(ImGuiCol_Text, kText);
			c(ImGuiCol_TextDisabled, kMuted);
			c(ImGuiCol_ChildBg, kPanel);
			c(ImGuiCol_FrameBg, Hex(0x1A2440));
			c(ImGuiCol_FrameBgHovered, Hex(0x223056));
			c(ImGuiCol_FrameBgActive, Hex(0x2A3A66));
			c(ImGuiCol_Button, Hex(0x1A2440));
			c(ImGuiCol_ButtonHovered, Hex(0x243259));
			c(ImGuiCol_ButtonActive, kAccentDim);
			c(ImGuiCol_Header, Hex(0x1B4F94, 0.45f));
			c(ImGuiCol_HeaderHovered, Hex(0x243259));
			c(ImGuiCol_HeaderActive, Hex(0x1B4F94, 0.8f));
			c(ImGuiCol_SliderGrab, kAccent);
			c(ImGuiCol_SliderGrabActive, kAccentText);
			c(ImGuiCol_CheckMark, kAccent);
			c(ImGuiCol_Separator, kLineSoft);
			c(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
			c(ImGuiCol_ScrollbarGrab, Hex(0x243259));
			c(ImGuiCol_ScrollbarGrabHovered, Hex(0x2E4170));
			c(ImGuiCol_PopupBg, Hex(0x0E1322, 0.98f));
			f(ImGuiStyleVar_FrameRounding, 3.0f * Sc());
			f(ImGuiStyleVar_ChildRounding, 8.0f * Sc());
			f(ImGuiStyleVar_GrabRounding, 2.0f);
			f(ImGuiStyleVar_PopupRounding, 8.0f);
			f(ImGuiStyleVar_ScrollbarRounding, 6.0f);
			f(ImGuiStyleVar_FrameBorderSize, 0.0f);
			f(ImGuiStyleVar_GrabMinSize, 10.0f);
			v2(ImGuiStyleVar_FramePadding, ImVec2(10 * Sc(), 5 * Sc()));
			v2(ImGuiStyleVar_ItemSpacing, ImVec2(8 * Sc(), 7 * Sc()));
		}
		~Scope()
		{
			ImGui::PopStyleVar(vars);
			ImGui::PopStyleColor(colors);
		}
	};

	// ---- Slanted pieces ----

	// A parallelogram over the box a..b, leaning right by kSlant (its middle stays where the box's middle is)
	inline void SlantQuad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, bool filled = true, float thick = 1.5f)
	{
		const float o = (b.y - a.y) * kSlant * 0.5f;
		ImVec2 p[4] = { ImVec2(a.x + o, a.y), ImVec2(b.x + o, a.y), ImVec2(b.x - o, b.y), ImVec2(a.x - o, b.y) };
		if (filled) dl->AddConvexPolyFilled(p, 4, col);
		else dl->AddPolyline(p, 4, col, true, thick);
	}

	// Dark text on a light fill, light text on a dark one
	inline ImVec4 OnFill(ImVec4 c)
	{
		const float lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
		return lum > 0.42f ? kOnAccent : kText;
	}

	inline std::string Caps(const char* s)
	{
		std::string out;
		for (const char* q = s; *q && !(q[0] == '#' && q[1] == '#'); q++) out += static_cast<char>(std::toupper(static_cast<unsigned char>(*q)));
		return out;
	}

	struct CapsFont
	{
		bool on;
		explicit CapsFont(ImFont* f = gCaps) : on(f != nullptr) { if (on) ImGui::PushFont(f); }
		~CapsFont() { if (on) ImGui::PopFont(); }
	};

	// Condensed capitals in a color with a rule after them, to start a section
	inline void Heading(const char* text)
	{
		ImGui::Spacing();
		{
			CapsFont f;
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextUnformatted(Caps(text).c_str());
			ImGui::PopStyleColor();
		}
		ImVec2 p = ImGui::GetItemRectMax();
		ImVec2 a = ImGui::GetItemRectMin();
		float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 8, (a.y + p.y) * 0.5f), ImVec2(right, (a.y + p.y) * 0.5f), U32(kLineSoft));
	}

	// A slanted chip, filled with its color, its text in capitals (a grade, ROOT CAUSE, a speed). kMuted or kChip
	// draws a quiet chip instead (dark fill, grey text).
	inline void Pill(const std::string& text, ImVec4 color, float minWidth = 0.0f)
	{
		const bool quiet = (color.x == kMuted.x && color.y == kMuted.y && color.z == kMuted.z) || (color.x == kChip.x && color.y == kChip.y && color.z == kChip.z);
		CapsFont f;
		const std::string t = Caps(text.c_str());
		ImVec2 ts = ImGui::CalcTextSize(t.c_str());
		float h = ts.y + 3.0f * Sc(), w = std::max(ts.x + 14.0f * Sc() + h * kSlant, minWidth);
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		SlantQuad(dl, p, ImVec2(p.x + w, p.y + h), U32(quiet ? kChip : color));
		dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + 1.5f * Sc()), U32(quiet ? kSub : OnFill(color)), t.c_str());
		ImGui::Dummy(ImVec2(w, h));
	}

	// A slanted button: filled (fill color, its text dark or light to match) or outlined (border color, its text tinted)
	inline bool SlantButton(const char* label, ImVec2 size, ImVec4 fill, bool filled, ImVec4 textCol)
	{
		CapsFont f;
		const std::string t = Caps(label);
		const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
		const ImGuiStyle& st = ImGui::GetStyle();
		const float h = size.y > 0.0f ? size.y : ts.y + st.FramePadding.y * 2.0f + 2.0f * Sc();
		const float w = size.x > 0.0f ? size.x : ts.x + st.FramePadding.x * 2.0f + 8.0f * Sc() + h * kSlant;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool r = ImGui::InvisibleButton(label, ImVec2(w, h));
		const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 b(p.x + w, p.y + h);
		if (filled)
		{
			ImVec4 c = fill;
			if (hov) c = ImVec4(std::min(1.0f, c.x * 1.12f + 0.04f), std::min(1.0f, c.y * 1.12f + 0.04f), std::min(1.0f, c.z * 1.12f + 0.04f), c.w);
			if (act) c = ImVec4(c.x * 0.85f, c.y * 0.85f, c.z * 0.85f, c.w);
			SlantQuad(dl, p, b, U32(c));
		}
		else
		{
			if (hov || act) SlantQuad(dl, p, b, U32(fill, act ? 0.24f : 0.12f));
			const float inset = 1.0f;
			SlantQuad(dl, ImVec2(p.x + inset, p.y + inset), ImVec2(b.x - inset, b.y - inset), U32(fill), false, 2.0f);
		}
		dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), U32(textCol), t.c_str());
		return r;
	}

	// The main action on screen: filled team blue, dark capitals
	inline bool AccentButton(const char* label, ImVec2 size = ImVec2(0, 0))
	{
		return SlantButton(label, size, kAccent, true, kOnAccent);
	}

	// A second action next to the main one: outlined in a color, its text tinted
	inline bool OutlineButton(const char* label, ImVec2 size = ImVec2(0, 0), ImVec4 color = kAccent)
	{
		const ImVec4 t(std::min(1.0f, color.x * 0.6f + 0.4f), std::min(1.0f, color.y * 0.6f + 0.4f), std::min(1.0f, color.z * 0.6f + 0.4f), 1.0f);
		return SlantButton(label, size, color, false, t);
	}

	// A quiet action (transport, restart): a dark slanted block, light capitals
	inline bool QuietButton(const char* label, ImVec2 size = ImVec2(0, 0))
	{
		return SlantButton(label, size, kChip, true, kText);
	}

	// A quiet text-only button (small extras like "Think harder")
	inline bool LinkButton(const char* label)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x1A2440));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDim);
		ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
		bool r = ImGui::SmallButton(label);
		ImGui::PopStyleColor(4);
		return r;
	}

	// ROAD2 in the display face (the 2 in team orange) and the screen's name on a quiet chip, centered on it
	inline void Wordmark(ImFont* display, const char* section)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		float h;
		{
			if (display) ImGui::PushFont(display);
			ImGui::TextColored(kText, "ROAD");
			ImGui::SameLine(0, 0);
			ImGui::TextColored(kOrange, "2");
			h = ImGui::GetItemRectSize().y;
			if (display) ImGui::PopFont();
		}
		ImGui::SameLine(0, 10.0f * Sc());
		CapsFont f;
		const float ch = ImGui::GetTextLineHeight() + 3.0f * Sc();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y + (h - ch) * 0.5f));
		Pill(section, kChip);
	}

	// The grade badge (style A): a slanted block in the grade's color with a hard shadow, the number big in the hero
	// face, "OF 5" under it. size = the block's height; returns the width it took.
	inline float GradeBadge(ImDrawList* dl, ImVec2 p, float size, int grade, ImVec4 color, bool ofFive = true)
	{
		const float w = size;
		const float sh = size * 0.06f;
		SlantQuad(dl, ImVec2(p.x + sh, p.y + sh), ImVec2(p.x + w + sh, p.y + size + sh), U32(ImVec4(color.x * 0.25f, color.y * 0.25f, color.z * 0.25f, 1.0f)));
		SlantQuad(dl, p, ImVec2(p.x + w, p.y + size), U32(grade > 0 ? color : kChip));
		const std::string n = grade > 0 ? std::to_string(grade) : "-";
		const ImVec4 tc = grade > 0 ? OnFill(color) : kSub;
		ImFont* font = gHero ? gHero : ImGui::GetFont();
		const float fs = size * (ofFive ? 0.74f : 0.86f);
		const ImVec2 nts = font->CalcTextSizeA(fs, 1e9f, 0.0f, n.c_str());
		const float ny = p.y + (ofFive ? size * 0.06f : (size - nts.y) * 0.5f);
		dl->AddText(font, fs, ImVec2(p.x + (w - nts.x) * 0.5f, ny), U32(tc), n.c_str());
		if (ofFive)
		{
			ImFont* cf = gCaps ? gCaps : ImGui::GetFont();
			const float cs = std::max(10.0f, size * 0.17f);
			const ImVec2 cts = cf->CalcTextSizeA(cs, 1e9f, 0.0f, "OF 5");
			dl->AddText(cf, cs, ImVec2(p.x + (w - cts.x) * 0.5f - size * kSlant * 0.25f, p.y + size - cts.y - size * 0.07f), U32(tc, 0.8f), "OF 5");
		}
		return w + sh;
	}

	inline ImVec4 GradeColor(int overall)
	{
		if (overall >= 5) return Hex(0x2DE2E6);
		if (overall == 4) return kGood;
		if (overall == 3) return kWarn;
		if (overall == 2) return Hex(0xFF8A1F);
		if (overall == 1) return kBad;
		return kMuted;
	}

	inline ImVec4 SegGradeColor(const std::string& g)
	{
		if (g == "Clean" || g == "Calculated") return kGood;
		if (g == "Solid") return Hex(0x5EB8FF);
		if (g == "Sloppy") return kWarn;
		return Hex(0xFF8A1F);
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

	// ---- Style A stand-ins for ImGui's square widgets (Nov 3, "leftover squares"): the same calls and returns as
	// ImGui's, drawn slanted. Click-to-type and the mouse wheel aren't there; nothing here used them. ----

	// The visible part of a label (ImGui hides everything from "##")
	inline const char* LabelEnd(const char* label) { const char* e = std::strstr(label, "##"); return e ? e : label + std::strlen(label); }

	// The label after a widget, in the body face, centred on the row
	inline float LabelAfter(ImDrawList* dl, const char* label, float x, float y, float rowH, ImVec4 col = kText)
	{
		const char* end = LabelEnd(label);
		if (end == label) return 0.0f;
		const ImVec2 ts = ImGui::CalcTextSize(label, end);
		dl->AddText(ImVec2(x, y + (rowH - ts.y) * 0.5f), U32(col), label, end);
		return ts.x;
	}

	// A slanted switch: blue track with a light knob on the right when on, a dark one with a grey knob when off
	inline bool Checkbox(const char* label, bool* v)
	{
		const ImGuiStyle& st = ImGui::GetStyle();
		const float rowH = ImGui::GetFrameHeight();
		const float h = std::round(rowH * 0.62f), w = std::round(h * 2.0f);
		const char* end = LabelEnd(label);
		const float lw = end == label ? 0.0f : st.ItemInnerSpacing.x + ImGui::CalcTextSize(label, end).x;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton(label, ImVec2(w + lw + h * kSlant, rowH));
		if (clicked) *v = !*v;
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float y = p.y + (rowH - h) * 0.5f, x = p.x + h * kSlant * 0.5f;
		ImVec4 track = *v ? kAccent : kTrack;
		if (hov) track = *v ? kAccentText : Hex(0x243259);
		SlantQuad(dl, ImVec2(x, y), ImVec2(x + w, y + h), U32(track));
		const float kw = std::round(w * 0.42f), in = std::max(2.0f, std::round(h * 0.16f));
		const float kx = *v ? x + w - kw - in : x + in;
		SlantQuad(dl, ImVec2(kx, y + in), ImVec2(kx + kw, y + h - in), U32(*v ? kText : kMuted));
		LabelAfter(dl, label, x + w + h * kSlant * 0.5f + st.ItemInnerSpacing.x, p.y, rowH, *v ? kText : kSub);
		return clicked;
	}

	// A slanted radio: a chip, blue when it's the one picked
	inline bool RadioButton(const char* label, bool active)
	{
		CapsFont f;
		const std::string t = Caps(std::string(label, LabelEnd(label)).c_str());
		const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
		const float h = ImGui::GetFrameHeight() * 0.8f, w = ts.x + 16.0f * Sc() + h * kSlant;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool r = ImGui::InvisibleButton(label, ImVec2(w, h));
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		SlantQuad(dl, p, ImVec2(p.x + w, p.y + h), U32(active ? kAccent : hov ? Hex(0x243259) : kChip));
		dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), U32(active ? kOnAccent : kSub), t.c_str());
		return r;
	}

	// The slider: a slanted track filled blue up to the value, a slanted grab, the value in condensed capitals on it
	inline bool SlantSliderCore(const char* label, double* v, double lo, double hi, int decimals, const char* fmt, bool isInt)
	{
		const ImGuiStyle& st = ImGui::GetStyle();
		const float rowH = ImGui::GetFrameHeight();
		const float w = std::max(40.0f, ImGui::CalcItemWidth());
		const char* end = LabelEnd(label);
		const float lw = end == label ? 0.0f : st.ItemInnerSpacing.x + ImGui::CalcTextSize(label, end).x;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(label, ImVec2(w + lw, rowH));
		const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
		const float h = std::round(rowH * 0.82f), y = p.y + (rowH - h) * 0.5f, lean = h * kSlant * 0.5f;
		const float x0 = p.x + lean, x1 = p.x + w - lean, gw = std::max(6.0f, std::round(h * 0.45f));
		bool changed = false;
		if (act && hi > lo)
		{
			const float t = std::clamp((ImGui::GetIO().MousePos.x - x0 - gw * 0.5f) / std::max(1.0f, x1 - x0 - gw), 0.0f, 1.0f);
			double nv = lo + t * (hi - lo);
			const double m = std::pow(10.0, decimals);
			nv = isInt ? std::round(nv) : std::round(nv * m) / m;
			nv = std::clamp(nv, lo, hi);
			if (nv != *v) { *v = nv; changed = true; }
		}
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float t = hi > lo ? static_cast<float>(std::clamp((*v - lo) / (hi - lo), 0.0, 1.0)) : 0.0f;
		const float gx = x0 + t * (x1 - x0 - gw);
		SlantQuad(dl, ImVec2(p.x, y), ImVec2(p.x + w, y + h), U32(hov || act ? Hex(0x1F2B4C) : kTrack));
		if (gx > x0 + 1.0f) SlantQuad(dl, ImVec2(p.x, y), ImVec2(gx + gw * 0.5f, y + h), U32(kAccentDim));
		SlantQuad(dl, ImVec2(gx, y - 1.0f), ImVec2(gx + gw, y + h + 1.0f), U32(act ? kAccentText : kAccent));
		char buf[64];
		if (isInt) std::snprintf(buf, sizeof buf, fmt, static_cast<int>(std::lround(*v)));
		else std::snprintf(buf, sizeof buf, fmt, static_cast<float>(*v));
		{
			CapsFont f;
			const std::string vt = Caps(buf);
			const ImVec2 ts = ImGui::CalcTextSize(vt.c_str());
			dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f), U32(kText), vt.c_str());
		}
		LabelAfter(dl, label, p.x + w + st.ItemInnerSpacing.x, p.y, rowH, kSub);
		return changed;
	}
	inline int FormatDecimals(const char* fmt)
	{
		const char* pc = std::strchr(fmt, '%');
		if (!pc) return 2;
		const char* dot = std::strchr(pc, '.');
		const char* f = std::strpbrk(pc + 1, "fFeEgG");
		return dot && f && dot < f ? std::atoi(dot + 1) : 3;
	}
	inline bool SliderFloat(const char* label, float* v, float lo, float hi, const char* fmt = "%.3f", float = 1.0f)
	{
		double d = *v;
		const bool r = SlantSliderCore(label, &d, lo, hi, FormatDecimals(fmt), fmt, false);
		if (r) *v = static_cast<float>(d);
		return r;
	}
	inline bool SliderInt(const char* label, int* v, int lo, int hi, const char* fmt = "%d")
	{
		double d = *v;
		const bool r = SlantSliderCore(label, &d, lo, hi, 0, fmt, true);
		if (r) *v = static_cast<int>(std::lround(d));
		return r;
	}

	// A dropdown with a slanted frame (ImGui's popup list; the box behind it drawn under the text)
	inline bool Combo(const char* label, int* current, const char* const items[], int count)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, kText);
		const float w = ImGui::CalcItemWidth();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool r = ImGui::Combo(label, current, items, count);
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopStyleColor(7);
		dl->ChannelsSetCurrent(0);
		const float h = ImGui::GetFrameHeight();
		SlantQuad(dl, p, ImVec2(p.x + w, p.y + h), U32(hov ? Hex(0x243259) : kChip));
		const float bw = h;   // the arrow's end, a shade darker
		SlantQuad(dl, ImVec2(p.x + w - bw, p.y), ImVec2(p.x + w, p.y + h), U32(kAccentDim));
		dl->ChannelsMerge();
		return r;
	}

	// A section that opens and closes (ImGui's CollapsingHeader): a dark slanted strip with a blue edge when open,
	// the name in condensed capitals and a + or a - at the end. Remembers its state like ImGui does.
	inline bool gOpenAll = false;   // the offscreen layout harness opens every section
	inline bool Section(const char* label, bool defaultOpen = false)
	{
		ImGuiStorage* store = ImGui::GetStateStorage();
		const ImGuiID id = ImGui::GetID(label);
		bool open = store->GetBool(id, defaultOpen) || gOpenAll;
		ImGui::Spacing();
		CapsFont f;
		const std::string t = Caps(std::string(label, LabelEnd(label)).c_str());
		const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
		const float h = ts.y + 12.0f * Sc();
		const float w = ImGui::GetContentRegionAvail().x;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		if (ImGui::InvisibleButton(label, ImVec2(w, h))) { open = !open; store->SetBool(id, open); }
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float lean = h * kSlant * 0.5f;
		SlantQuad(dl, ImVec2(p.x + lean, p.y), ImVec2(p.x + w - lean, p.y + h), U32(hov ? Hex(0x16203A) : kPanel2));
		SlantQuad(dl, ImVec2(p.x + lean, p.y), ImVec2(p.x + lean + 5.0f * Sc(), p.y + h), U32(open ? kAccent : kLine));
		dl->AddText(ImVec2(p.x + lean + 16.0f * Sc(), p.y + (h - ts.y) * 0.5f), U32(open ? kText : kSub), t.c_str());
		const char* sign = open ? "-" : "+";
		const ImVec2 ss = ImGui::CalcTextSize(sign);
		dl->AddText(ImVec2(p.x + w - lean - 14.0f * Sc() - ss.x, p.y + (h - ss.y) * 0.5f), U32(open ? kAccentText : kMuted), sign);
		if (open) ImGui::Spacing();
		return open;
	}
}
