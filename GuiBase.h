#pragma once
#include "bakkesmod/plugin/PluginSettingsWindow.h"
#include "bakkesmod/plugin/pluginwindow.h"

class SettingsWindowBase : public BakkesMod::Plugin::PluginSettingsWindow
{
public:
	std::string GetPluginName() override;
	void SetImGuiContext(uintptr_t ctx) override;
};

class PluginWindowBase : public BakkesMod::Plugin::PluginWindow
{
public:
	virtual ~PluginWindowBase() = default;

	bool isWindowOpen_ = false;
	std::string menuTitle_ = "Road2";
	// Nov 1: a size-conscious window. uiBaseSize_ is the "Window size" setting (RenderWindow sets it from its cvar).
	// The height always fits what's showing (fitHeight_, measured each frame), and dragging the window narrower than
	// it needs shrinks the whole UI with it instead of cutting things off.
	float uiBaseSize_ = 1.0f;
	float lastBaseSize_ = 1.0f;
	float fitHeight_ = 0.0f;
	float uiScaleNow_ = 1.0f;   // UiStyle::gUser while this window draws

	std::string GetMenuName() override;
	std::string GetMenuTitle() override;
	void SetImGuiContext(uintptr_t ctx) override;
	bool ShouldBlockInput() override;
	bool IsActiveOverlay() override;
	void OnOpen() override;
	void OnClose() override;
	void Render() override;

	virtual void RenderWindow() = 0;
};