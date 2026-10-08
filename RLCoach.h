#pragma once

#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);


class RLCoach: public BakkesMod::Plugin::BakkesModPlugin
{
	// On/off switch for the test readout (console: rlcoach_hud 0 or 1)
	std::shared_ptr<bool> hudEnabled;

	void onLoad() override;
	void onUnload() override;

	// Draws the readout on screen every frame
	void RenderHud(CanvasWrapper canvas);
};
