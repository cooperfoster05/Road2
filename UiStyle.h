#pragma once
// RL Coach's look for its ImGui windows: dark, rounded, one blue accent.
// Push a Scope before drawing and it's undone when the Scope goes away, so other plugins keep their style.

#include "IMGUI/imgui.h"
#include <algorithm>
#include <string>

namespace UiStyle
{
	inline const ImVec4 kAccent{ 0.33f, 0.60f, 1.00f, 1.00f };
	inline const ImVec4 kAccentDim{ 0.22f, 0.40f, 0.70f, 1.00f };
	inline const ImVec4 kText{ 0.90f, 0.92f, 0.96f, 1.00f };
	inline const ImVec4 kMuted{ 0.52f, 0.56f, 0.63f, 1.00f };
	inline const ImVec4 kGood{ 0.45f, 0.90f, 0.45f, 1.00f };
	inline const ImVec4 kOk{ 0.60f, 0.80f, 1.00f, 1.00f };
	inline const ImVec4 kWarn{ 1.00f, 0.80f, 0.30f, 1.00f };
	inline const ImVec4 kBad{ 1.00f, 0.45f, 0.40f, 1.00f };
	inline const ImVec4 kGold{ 1.00f, 0.80f, 0.25f, 1.00f };

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
				c(ImGuiCol_WindowBg, ImVec4(0.075f, 0.082f, 0.105f, 0.97f));
				c(ImGuiCol_TitleBg, ImVec4(0.065f, 0.072f, 0.092f, 1.0f));
				c(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.12f, 0.17f, 1.0f));
				c(ImGuiCol_TitleBgCollapsed, ImVec4(0.065f, 0.072f, 0.092f, 0.9f));
				c(ImGuiCol_Border, ImVec4(0.20f, 0.22f, 0.28f, 0.8f));
				f(ImGuiStyleVar_WindowRounding, 8.0f);
				f(ImGuiStyleVar_WindowBorderSize, 1.0f);
				v2(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
			}
			c(ImGuiCol_Text, kText);
			c(ImGuiCol_TextDisabled, kMuted);
			c(ImGuiCol_ChildBg, ImVec4(0.105f, 0.115f, 0.145f, 1.0f));
			c(ImGuiCol_FrameBg, ImVec4(0.15f, 0.165f, 0.205f, 1.0f));
			c(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.21f, 0.27f, 1.0f));
			c(ImGuiCol_FrameBgActive, ImVec4(0.22f, 0.25f, 0.33f, 1.0f));
			c(ImGuiCol_Button, ImVec4(0.17f, 0.19f, 0.24f, 1.0f));
			c(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.38f, 0.62f, 1.0f));
			c(ImGuiCol_ButtonActive, kAccentDim);
			c(ImGuiCol_Header, ImVec4(0.22f, 0.36f, 0.60f, 0.45f));
			c(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.42f, 0.70f, 0.55f));
			c(ImGuiCol_HeaderActive, ImVec4(0.28f, 0.46f, 0.78f, 0.70f));
			c(ImGuiCol_SliderGrab, kAccent);
			c(ImGuiCol_SliderGrabActive, ImVec4(0.50f, 0.72f, 1.0f, 1.0f));
			c(ImGuiCol_CheckMark, kAccent);
			c(ImGuiCol_Separator, ImVec4(0.22f, 0.24f, 0.30f, 1.0f));
			c(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
			c(ImGuiCol_ScrollbarGrab, ImVec4(0.22f, 0.24f, 0.30f, 1.0f));
			c(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.30f, 0.33f, 0.42f, 1.0f));
			c(ImGuiCol_PopupBg, ImVec4(0.09f, 0.10f, 0.13f, 0.98f));
			f(ImGuiStyleVar_FrameRounding, 5.0f);
			f(ImGuiStyleVar_ChildRounding, 6.0f);
			f(ImGuiStyleVar_GrabRounding, 5.0f);
			f(ImGuiStyleVar_PopupRounding, 6.0f);
			f(ImGuiStyleVar_ScrollbarRounding, 6.0f);
			f(ImGuiStyleVar_FrameBorderSize, 0.0f);
			f(ImGuiStyleVar_GrabMinSize, 10.0f);
			v2(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
			v2(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
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
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 8, (a.y + p.y) * 0.5f), ImVec2(right, (a.y + p.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_Separator));
	}

	// A rounded colored tag with text, e.g. a grade
	inline void Pill(const std::string& text, ImVec4 color, float minWidth = 0.0f)
	{
		ImVec2 ts = ImGui::CalcTextSize(text.c_str());
		float w = std::max(ts.x + 12.0f, minWidth), h = ts.y + 2.0f;
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.22f)), h * 0.5f);
		dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + 1.0f), ImGui::GetColorU32(color), text.c_str());
		ImGui::Dummy(ImVec2(w, h));
	}

	// A button drawn in the accent color (the main action on screen)
	inline bool AccentButton(const char* label, ImVec2 size = ImVec2(0, 0))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.52f, 0.88f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
		bool r = ImGui::Button(label, size);
		ImGui::PopStyleColor(3);
		return r;
	}

	inline ImVec4 GradeColor(int overall)
	{
		if (overall >= 4) return kGood;
		if (overall == 3) return kWarn;
		if (overall == 2) return ImVec4(1.0f, 0.63f, 0.31f, 1.0f);
		if (overall == 1) return kBad;
		return kMuted;
	}

	inline ImVec4 SegGradeColor(const std::string& g)
	{
		if (g == "Clean" || g == "Calculated") return kGood;
		if (g == "Solid") return kOk;
		if (g == "Sloppy") return kWarn;
		return kBad;
	}
}
