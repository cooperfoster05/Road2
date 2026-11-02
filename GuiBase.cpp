#include "pch.h"
#include "GuiBase.h"
#include "UiStyle.h"
#include "IMGUI/imgui_internal.h"
#include <algorithm>
#include <cmath>

std::string SettingsWindowBase::GetPluginName()
{
	return "RLCoach";
}

void SettingsWindowBase::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

std::string PluginWindowBase::GetMenuName()
{
	return "RLCoach";
}

std::string PluginWindowBase::GetMenuTitle()
{
	return menuTitle_;
}

void PluginWindowBase::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

bool PluginWindowBase::ShouldBlockInput()
{
	return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

bool PluginWindowBase::IsActiveOverlay()
{
	return true;
}

void PluginWindowBase::OnOpen()
{
	isWindowOpen_ = true;
}

void PluginWindowBase::OnClose()
{
	isWindowOpen_ = false;
}

void PluginWindowBase::Render()
{
	{
		// Size-conscious (Nov 1). The width is yours to drag; the height always fits what the window is showing (a
		// Combine set is short, the rep viewer with its list is tall), up to most of the screen, then it scrolls. The UI
		// needs about 480 wide at the "Window size" you picked: drag it narrower and everything shrinks with it (down to
		// 0.6), so nothing gets cut off. Changing "Window size" resizes the window to match.
		const float base = std::clamp(uiBaseSize_, 0.6f, 1.6f);
		const float need = 480.0f * base, minW = need * 0.6f;
		const float screenH = ImGui::GetIO().DisplaySize.y > 0 ? ImGui::GetIO().DisplaySize.y : 1080.0f;
		ImGui::SetNextWindowSize(ImVec2(540.0f * base, 600.0f), ImGuiCond_FirstUseEver);
		if (ImGuiWindow* prev = ImGui::FindWindowByName(menuTitle_.c_str()); prev && std::fabs(base - lastBaseSize_) > 0.001f)
			ImGui::SetNextWindowSize(ImVec2(prev->Size.x * base / lastBaseSize_, prev->Size.y), ImGuiCond_Always);
		lastBaseSize_ = base;
		const float h = fitHeight_ > 0 ? std::min(fitHeight_, screenH * 0.92f) : 0.0f;
		if (h > 0) ImGui::SetNextWindowSizeConstraints(ImVec2(minW, h), ImVec2(100000.0f, h));
		else ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 100.0f), ImVec2(100000.0f, 100000.0f));

		UiStyle::gUser = uiScaleNow_;   // its padding at last frame's size
		{
			UiStyle::Scope style;   // RL Coach's look, only for this window
			if (ImGui::Begin(menuTitle_.c_str(), &isWindowOpen_, ImGuiWindowFlags_None))
			{
				const float shrink = std::clamp(ImGui::GetWindowWidth() / need, 0.6f, 1.0f);
				UiStyle::gUser = uiScaleNow_ = base * shrink;
				ImGui::SetWindowFontScale(UiStyle::gUser);
				RenderWindow();
				// What it took: the bottom of the lowest thing drawn, plus the bottom padding (next frame's height)
				ImGuiWindow* w = ImGui::GetCurrentWindow();
				fitHeight_ = std::ceil(w->DC.CursorMaxPos.y - w->Pos.y + w->Scroll.y + ImGui::GetStyle().WindowPadding.y) + 1.0f;   // (rounded up: a part-pixel short shows a scrollbar)
			}
			ImGui::End();
		}
		UiStyle::gUser = 1.0f;   // anything else drawn in RL Coach's style (the settings tab) stays at full size
	}

	if (!isWindowOpen_)
	{
		_globalCvarManager->executeCommand("togglemenu " + GetMenuName());
	}
}
