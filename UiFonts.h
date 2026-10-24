#pragma once
// Fonts for the rep viewer (Oct 24 redesign): Inter for text, JetBrains Mono for numbers. Both are under the SIL
// Open Font License (the OFL files sit next to them), so they can ship with the plugin. The plugin keeps them in
// data/RLCoach/fonts; BakkesMod only loads fonts from data/fonts, so they're copied there (with an RLCoach_ prefix)
// the first time. If anything fails, the window keeps ImGui's default font: ImGui::PushFont(nullptr) is safe.

#include "IMGUI/imgui.h"
#include <filesystem>
#include <system_error>

namespace UiFonts
{
	enum Face { Body, Bold, Label, Head, Mono, Big, kCount };
	struct Spec { const char* name; const char* source; const char* file; int size; };
	// Sizes are for UiStyle::kScale = 0.8 (16, 16, 12, 20, 19, 44 at full size); the names carry the size, so a
	// changed size loads as a new font instead of reusing one BakkesMod already loaded at the old size
	inline const Spec kSpecs[kCount] = {
		{ "rlcoach_body13",  "Inter-Regular.ttf",       "RLCoach_Inter-Regular.ttf",       13 },
		{ "rlcoach_bold13",  "Inter-SemiBold.ttf",      "RLCoach_Inter-SemiBold.ttf",      13 },
		{ "rlcoach_label10", "Inter-Bold.ttf",          "RLCoach_Inter-Bold.ttf",          10 },
		{ "rlcoach_head16",  "Inter-SemiBold.ttf",      "RLCoach_Inter-SemiBold.ttf",      16 },
		{ "rlcoach_mono15",  "JetBrainsMono-Bold.ttf",  "RLCoach_JetBrainsMono-Bold.ttf",  15 },
		{ "rlcoach_big35",   "JetBrainsMono-Bold.ttf",  "RLCoach_JetBrainsMono-Bold.ttf",  35 },
	};

	// Latin, plus the few symbols the review uses: dashes, bullet, ellipsis, arrows, minus
	inline const ImWchar* Ranges()
	{
		static const ImWchar r[] = { 0x0020, 0x00FF, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0x2190, 0x2193, 0x2212, 0x2212, 0 };
		return r;
	}

	// Copies data/RLCoach/fonts/<source> to data/fonts/<file> where it's missing. Returns false if any is missing after.
	inline bool Install(const std::filesystem::path& dataFolder)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path from = dataFolder / "RLCoach" / "fonts", to = dataFolder / "fonts";
		fs::create_directories(to, ec);
		bool ok = true;
		for (const Spec& s : kSpecs)
		{
			if (fs::exists(to / s.file, ec)) continue;
			fs::copy_file(from / s.source, to / s.file, fs::copy_options::skip_existing, ec);
			ok = ok && fs::exists(to / s.file, ec);
		}
		return ok;
	}
}
