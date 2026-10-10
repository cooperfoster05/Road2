#include "pch.h"
#include "RLCoach.h"
#include "Ratings.h"
#include "HudDraw.h"
#include "ControllerOverlay.h"
#include "Units.h"
#include "bakkesmod/wrappers/includes.h"

#include <algorithm>
#include <cmath>
#include <format>

using namespace HudDraw;

// ============================================================
// In-game panel and controller overlay (game thread, every frame)
// ============================================================

namespace
{
	// Panel colors
	const Rgba kPanelBg{ 13, 15, 21, 205 };
	const Rgba kHeaderBg{ 27, 31, 42, 235 };
	const Rgba kTitle{ 238, 242, 250, 255 };
	const Rgba kSection{ 128, 138, 156, 255 };
	const Rgba kDivider{ 255, 255, 255, 22 };
}

void RLCoach::RenderHud(CanvasWrapper canvas)
{
	UpdateRatingKeys();
	Units::SpeedMode() = *speedUnits;

	if (!IsOfflinePractice()) return;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (car.IsNull()) return;

	// World overlays first, so the panel draws on top of them
	if (practiceState != PracticeState::Off) DrawPracticeOverlay(canvas);
	else if (viewerActive && !viewerFrames.empty()) DrawViewerPaths(canvas);
	else if (state == RepState::Recording && *livePrediction && !livePred.empty()) DrawPredictedPath(canvas, livePred);

	// What the controller is doing: the recording while watching, your pad otherwise
	PadInput pad;
	bool replaying = practiceState == PracticeState::Off && viewerActive && !viewerFrames.empty();
	if (replaying)
	{
		int i = std::min(static_cast<int>(viewerPlayhead), static_cast<int>(viewerFrames.size()) - 1);
		const Frame& f = viewerFrames[i];
		pad = PadInput{ f.throttle, f.steer, f.pitch, f.yaw, f.roll, f.jump, f.boost, f.handbrake, f.boostAmount };
	}
	else
	{
		const ControllerInput& in = lastInput;
		pad = PadInput{ in.Throttle, in.Steer, in.Pitch, in.Yaw, in.Roll, in.Jump != 0,
			(in.ActivateBoost || in.HoldingBoost) != 0, in.Handbrake != 0, -1.0f };
		BoostWrapper bc = car.GetBoostComponent();
		if (!bc.IsNull()) pad.boostAmount = bc.GetCurrentBoostAmount() * 100.0f;
	}

	if (*inputDisplay >= 2)
	{
		Vector2 screen = canvas.GetSize();
		PadLayout layout{ *padJump, *padBoost, *padSlide, *padRollLeft, *padRollRight };
		ControllerOverlay::Draw(canvas, pad, *inputDisplay == 3 ? PadStyle::Xbox : PadStyle::PS4,
			*padTheme == 1 ? PadTheme::Light : PadTheme::Dark, layout,
			*padX * screen.X, *padY * screen.Y, *padScale * (screen.Y / 1080.0f));
	}

	if (!*hudEnabled) return;
	DrawPanel(canvas, car, pad);
}

void RLCoach::DrawPanel(CanvasWrapper& canvas, CarWrapper& car, const PadInput& pad)
{
	// One line on the panel. Detail lines are hidden in compact mode.
	struct HudLine
	{
		enum Kind { Text, Section } kind = Text;
		std::vector<PanelChunk> chunks;
		float scale = 1.0f;
		bool detail = false;
	};
	std::vector<HudLine> lines;
	auto add = [&](std::string text, unsigned char r = 225, unsigned char g = 230, unsigned char b = 238, bool detail = false, float scale = 1.0f) {
		HudLine l;
		l.chunks = { { std::move(text), r, g, b } };
		l.detail = detail;
		l.scale = scale;
		lines.push_back(std::move(l));
	};
	auto addChunks = [&](const std::vector<PanelChunk>& chunks, bool detail = false) {
		HudLine l;
		l.chunks = chunks;
		l.detail = detail;
		lines.push_back(std::move(l));
	};
	auto section = [&](const char* title, bool detail = false) {
		HudLine l;
		l.kind = HudLine::Section;
		l.chunks = { { title, kSection.r, kSection.g, kSection.b } };
		l.detail = detail;
		l.scale = 0.8f;
		lines.push_back(std::move(l));
	};
	auto inputText = [&]() {
		if (*inputDisplay != 1) return;
		add(std::format("Thr {:+.2f}  Str {:+.2f}  P {:+.2f}  Y {:+.2f}  R {:+.2f}",
			pad.throttle, pad.steer, pad.pitch, pad.yaw, pad.roll), 160, 205, 255);
		add(std::format("Jump {}   Boost {}   Slide {}", pad.jump ? "X" : "-", pad.boost ? "X" : "-", pad.handbrake ? "X" : "-"), 160, 205, 255);
	};
	auto gradeColor = [](Grade gr) -> PanelChunk {
		switch (gr)
		{
		case Grade::Clean: case Grade::Calculated: return { "", 115, 230, 115 };
		case Grade::Solid: return { "", 153, 204, 255 };
		case Grade::Sloppy: return { "", 255, 204, 77 };
		default: return { "", 255, 115, 102 };
		}
	};

	// Mode tag in the header
	std::string tag;
	Rgba tagCol{ 170, 176, 190, 255 };

	if (practiceState != PracticeState::Off)
	{
		tag = "PRACTICE";
		tagCol = { 80, 200, 230, 255 };
		int pct = static_cast<int>(practiceSpeed * 100.0f + 0.5f);
		add(practiceLabel, 240, 244, 250);
		add(std::format("{}% speed   {:.2f}s lead-in{}", pct, practiceLeadIn, practiceAutoRamp ? "   auto-ramp" : ""), 150, 160, 178, true);
		if (practiceState == PracticeState::Frozen) add("Move to start the attempt", 255, 204, 77);
		else add("GO", 115, 230, 115);
		std::string bySpeed;
		for (const auto& [p, h] : practiceHitsByPct) bySpeed += std::format("{}{}%: {}", bySpeed.empty() ? "" : "   ", p, h);
		add(std::format("Hits {}/{}   streak {}", practiceHitsTotal, practiceAttempts, practiceStreak));
		if (!bySpeed.empty()) add(bySpeed, 150, 160, 178, true);
		inputText();
		if (!practiceResultLines.empty())
		{
			section("LAST ATTEMPT");
			for (const PanelGradeLine& gl : practiceResultLines) addChunks(gl.chunks);
		}
		if (!practiceRefLabel.empty()) add(practiceRefLabel, 130, 138, 152, true);
		add("Retry: rlcoach_practice_retry   Stop: rlcoach_practice_exit", 110, 118, 132, true);
	}
	else if (viewerActive && !viewerFrames.empty())
	{
		tag = "REPLAY";
		tagCol = { 120, 160, 255, 255 };
		int i = std::min(static_cast<int>(viewerPlayhead), static_cast<int>(viewerFrames.size()) - 1);
		const Frame& f = viewerFrames[i];
		std::string when = viewerRep.savedAt.size() >= 19 ? viewerRep.savedAt.substr(5, 11) : viewerRep.savedAt;
		add(std::format("{}   {}   {}", when, viewerRep.endReason,
			viewerRep.imported ? "from " + viewerRep.importedFrom
				: viewerRep.rating > 0 ? std::format("you rated {}", viewerRep.rating) : std::string("unrated")), 240, 244, 250);
		add(std::format("{:.2f}s / {:.2f}s   {}   {}x", f.t, viewerFrames.back().t, viewerPlaying ? "playing" : "paused", viewerSpeed), 150, 160, 178);
		float spd = std::sqrt(f.carVX * f.carVX + f.carVY * f.carVY + f.carVZ * f.carVZ);
		add(std::format("{}   boost {:.0f}   {}{}", Units::Speed(spd), f.boostAmount,
			f.onGround ? "grounded" : "airborne", f.touch ? "   TOUCH" : ""), 150, 160, 178, true);
		inputText();

		if (viewerGrade.overall > 0)
		{
			section("GRADE");
			unsigned char r, g, b;
			OverallRgb(viewerGrade.overall, r, g, b);
			add(std::format("{}  {}", viewerGrade.overall, Grader::OverallName(viewerGrade.overall)), r, g, b, false, 1.15f);
			if (viewerShot.valid && viewerShot.type != ShotType::Aerial)
				add(std::format("Graded as a {}", viewerShot.type == ShotType::Ground ? "ground shot"
					: viewerShot.type == ShotType::Dribble ? "dribble" : "double tap"), 150, 160, 178, true);
			add(viewerGrade.overallWhy, 150, 160, 178, true);
		}
		if (viewerGrade.biggestFix >= 0)
		{
			const SegmentGrade& fix = viewerGrade.segs[viewerGrade.biggestFix];
			PanelChunk c = gradeColor(fix.grade);
			addChunks({ { "Fix  ", 150, 160, 178 },
				{ std::format("{} ({}): {}", Segmenter::Name(viewerSegs.segs[fix.segIndex].kind), Grader::Name(fix.grade), fix.why), c.r, c.g, c.b } });
		}
		for (int si = 0; si < static_cast<int>(viewerSegs.segs.size()); si++)
		{
			const Segment& sg = viewerSegs.segs[si];
			if (i < sg.start || i > sg.end || sg.kind == SegKind::Recovery) continue;
			std::vector<PanelChunk> now = { { "Now  ", 150, 160, 178 },
				{ std::format("{}{}", Segmenter::Name(sg.kind), sg.touch > 1 ? std::format(" (touch {})", sg.touch) : std::string()), 240, 244, 250 } };
			for (const SegmentGrade& gs : viewerGrade.segs)
				if (gs.segIndex == si && gs.grade != Grade::None)
				{
					PanelChunk c = gradeColor(gs.grade);
					now.push_back({ std::format("  {}", Grader::Name(gs.grade)), c.r, c.g, c.b });
					now.push_back({ std::format(" - {}", gs.why), 150, 160, 178 });
				}
			addChunks(now);
		}
		if (viewerSegs.whiff) add("Whiff: no touch on this rep", 255, 120, 120);

		if (viewerShot.valid)
		{
			const ShotAnalysis& s = viewerShot;
			section("SHOT", true);
			if (!s.reachesGoalLine) add("Never reached the goal line", 255, 120, 120, true);
			else if (s.missDist <= 0.0f) add("On target", 120, 230, 120, true);
			else add(std::format("Missed {} by {:.0f}", s.missDir, s.missDist), 255, 120, 120, true);
			add(std::format("Aim {:.1f} deg off   contact {:.0f} {}, {:.0f} {} of ideal", s.aimErrDeg,
				std::fabs(s.contactUp), s.contactUp >= 0 ? "high" : "low",
				std::fabs(s.contactLeft), s.contactLeft >= 0 ? "left" : "right"), 225, 230, 238, true);
			if (s.earliestTick >= 0) add(std::format("Touched {:.2f}s after the earliest possible intercept", s.lateBy), 225, 230, 238, true);
			add(std::format("Shot speed {}", Units::Speed(s.shotSpeed)), 225, 230, 238, true);
		}
		if (!viewerStatus.empty()) add(viewerStatus, 255, 200, 0);
	}
	else
	{
		float speed = car.GetVelocity().magnitude();
		if (!*recordingEnabled) { tag = "OFF"; add("Recording off  (F2 > Plugins > RLCoach)", 150, 160, 178); }
		else if (state == RepState::Recording && !pendingEndReason.empty())
		{
			tag = "REC"; tagCol = { 255, 90, 90, 255 };
			add(std::format("Shot over ({}), catching your recovery", pendingEndReason), 240, 244, 250);
		}
		else if (state == RepState::Recording) { tag = "REC"; tagCol = { 255, 90, 90, 255 }; add("Recording rep", 240, 244, 250); }
		else if (state == RepState::Armed) { tag = "ARMED"; tagCol = { 255, 200, 60, 255 }; add("Rep starts when you move", 240, 244, 250); }
		else { tag = "READY"; add("Reset the shot or ball to start", 200, 205, 215); }

		if (*recordingEnabled) add(std::format("{} rep{} this session", repsThisSession, repsThisSession == 1 ? "" : "s"), 150, 160, 178, true);
		add(std::format("{}{}   boost {:.0f}   {}", Units::Speed(speed), speed >= 2200.0f ? " (supersonic)" : "",
			pad.boostAmount < 0 ? 0.0f : pad.boostAmount, car.IsOnGround() ? "grounded" : "airborne"), 150, 160, 178, true);
		inputText();

		if (*recordingEnabled && state == RepState::Recording && queuedRating > 0)
			add(std::format("This rep will be rated {}: {}", queuedRating, RatingName(queuedRating)), 120, 230, 120);

		if (*recordingEnabled && haveLastRep)
		{
			section("LAST REP");
			add(std::format("{}   {:.2f}s   {} touch{}", lastRep.endReason, lastRep.durationS,
				lastRep.touches, lastRep.touches == 1 ? "" : "es"), 150, 160, 178, true);
			for (const PanelGradeLine& gl : lastRepGradeLines) addChunks(gl.chunks);
			if (lastRating == 0) add("Rate it: 1 Missed  2 Close  3 Bad-in  4 Good-in  5 Great-in", 255, 200, 0);
			else add(std::format("You rated it {}: {}", lastRating, RatingName(lastRating)), 120, 230, 120);
		}
		if (!statusMessage.empty()) add(statusMessage, 130, 138, 152, true);
	}

	if (*showTiming)
		add(std::format("Plugin cost  tick {:.3f} ms (peak {:.2f})   draw {:.2f} ms (peak {:.2f})",
			perfTick.avgMs + perfInput.avgMs, perfTick.peakMs + perfInput.peakMs, perfDraw.avgMs, perfDraw.peakMs), 130, 200, 160);

	// Compact mode drops the detail lines (and any section left empty)
	if (*hudCompact)
	{
		std::vector<HudLine> kept;
		for (size_t k = 0; k < lines.size(); k++)
		{
			if (lines[k].detail) continue;
			if (lines[k].kind == HudLine::Section)
			{
				bool hasBody = false;
				for (size_t j = k + 1; j < lines.size() && lines[j].kind != HudLine::Section; j++)
					if (!lines[j].detail) { hasBody = true; break; }
				if (!hasBody) continue;
			}
			kept.push_back(lines[k]);
		}
		lines.swap(kept);
	}

	// ---- Layout ----
	Vector2 screen = canvas.GetSize();
	const float S = *hudScale * (screen.Y / 1080.0f);
	const float x = *hudX * screen.X, y = *hudY * screen.Y;
	const float padding = 12.0f * S, lineH = 17.0f * S, headerH = 26.0f * S, accentW = 3.0f * S, radius = 8.0f * S;
	const float textScale = S;

	auto lineHeight = [&](const HudLine& l) { return l.kind == HudLine::Section ? lineH * 0.8f + 7.0f * S : lineH * l.scale; };
	float bodyH = 0.0f, width = 250.0f * S;
	for (const HudLine& l : lines)
	{
		bodyH += lineHeight(l);
		float w = 0.0f;
		for (const PanelChunk& c : l.chunks) w += TextSize(canvas, c.text, l.scale * textScale).X;
		width = std::max(width, w + padding * 2 + accentW);
	}
	float tagW = TextSize(canvas, tag, 0.75f * textScale).X + 14.0f * S;
	width = std::max(width, TextSize(canvas, "RL COACH", 1.0f * textScale).X + tagW + padding * 3);
	const float height = headerH + padding * 0.6f + bodyH + padding * 0.7f;

	// ---- Draw ----
	RoundBox(canvas, x, y, width, height, radius, kPanelBg);
	RoundBox(canvas, x, y, width, headerH, radius, kHeaderBg);
	Box(canvas, x, y + headerH - radius, width, radius, kHeaderBg);   // square off the header's bottom corners
	Box(canvas, x, y + headerH, accentW, height - headerH - radius, Rgba{ tagCol.r, tagCol.g, tagCol.b, 200 });

	float titleH = TextSize(canvas, "RL COACH", textScale).Y;
	Text(canvas, x + padding, y + (headerH - titleH) * 0.5f, "RL COACH", textScale, kTitle);

	// Mode tag pill
	float pillH = headerH - 9.0f * S;
	float pillX = x + width - padding - tagW, pillY = y + (headerH - pillH) * 0.5f;
	RoundBox(canvas, pillX, pillY, tagW, pillH, pillH * 0.5f, Rgba{ tagCol.r, tagCol.g, tagCol.b, 55 });
	if (tag == "REC") Disc(canvas, pillX + 8.0f * S, pillY + pillH * 0.5f, 3.0f * S, tagCol);
	TextCentered(canvas, pillX + tagW * 0.5f + (tag == "REC" ? 4.0f * S : 0.0f), pillY + pillH * 0.5f, tag, 0.75f * textScale, tagCol);

	float cy = y + headerH + padding * 0.6f;
	bool first = true;
	for (const HudLine& l : lines)
	{
		float cx = x + padding + accentW;
		if (l.kind == HudLine::Section)
		{
			if (!first) Box(canvas, cx, cy + 2.0f * S, width - padding * 2 - accentW, 1.0f, kDivider);
			Text(canvas, cx, cy + 6.0f * S, l.chunks[0].text, l.scale * textScale, kSection, false);
		}
		else
		{
			for (const PanelChunk& c : l.chunks)
			{
				Text(canvas, cx, cy, c.text, l.scale * textScale, Rgba{ c.r, c.g, c.b, 255 });
				cx += TextSize(canvas, c.text, l.scale * textScale).X;
			}
		}
		cy += lineHeight(l);
		first = false;
	}
}

// ============================================================
// Look settings: panel, units, controller overlay
// ============================================================

void RLCoach::RegisterLookCvars()
{
	hudCompact = std::make_shared<bool>(false);
	hudScale = std::make_shared<float>(1.0f);
	hudX = std::make_shared<float>(0.016f);
	hudY = std::make_shared<float>(0.165f);
	speedUnits = std::make_shared<int>(0);
	inputDisplay = std::make_shared<int>(2);
	padX = std::make_shared<float>(0.016f);
	padY = std::make_shared<float>(0.80f);
	padScale = std::make_shared<float>(1.0f);
	padJump = std::make_shared<int>(PB_South);
	padBoost = std::make_shared<int>(PB_East);
	padSlide = std::make_shared<int>(PB_West);
	padRollLeft = std::make_shared<int>(PB_None);
	padRollRight = std::make_shared<int>(PB_None);
	targetHitbox = std::make_shared<int>(1);
	padTheme = std::make_shared<int>(0);

	cvarManager->registerCvar("rlcoach_hud_compact", "0", "Panel shows only the essentials", true, true, 0, true, 1).bindTo(hudCompact);
	cvarManager->registerCvar("rlcoach_hud_scale", "1", "Panel size", true, true, 0.6f, true, 2.0f).bindTo(hudScale);
	cvarManager->registerCvar("rlcoach_hud_x", "0.016", "Panel left edge (fraction of the screen width)", true, true, 0, true, 1).bindTo(hudX);
	cvarManager->registerCvar("rlcoach_hud_y", "0.165", "Panel top edge (fraction of the screen height)", true, true, 0, true, 1).bindTo(hudY);
	cvarManager->registerCvar("rlcoach_speed_units", "0", "Speed units: 0 uu/s, 1 km/h, 2 mph", true, true, 0, true, 2).bindTo(speedUnits);
	cvarManager->registerCvar("rlcoach_input_display", "2", "Inputs: 0 none, 1 text on the panel, 2 PS4 controller, 3 Xbox controller", true, true, 0, true, 3).bindTo(inputDisplay);
	cvarManager->registerCvar("rlcoach_pad_x", "0.016", "Controller overlay left edge (fraction of the screen width)", true, true, 0, true, 1).bindTo(padX);
	cvarManager->registerCvar("rlcoach_pad_y", "0.80", "Controller overlay top edge (fraction of the screen height)", true, true, 0, true, 1).bindTo(padY);
	cvarManager->registerCvar("rlcoach_pad_scale", "1", "Controller overlay size", true, true, 0.5f, true, 2.5f).bindTo(padScale);
	cvarManager->registerCvar("rlcoach_pad_theme", "0", "Controller overlay color: 0 dark, 1 light", true, true, 0, true, 1).bindTo(padTheme);
	cvarManager->registerCvar("rlcoach_pad_jump", "1", "Controller button for jump", true, true, 0, true, PB_Count - 1).bindTo(padJump);
	cvarManager->registerCvar("rlcoach_pad_boost", "2", "Controller button for boost", true, true, 0, true, PB_Count - 1).bindTo(padBoost);
	cvarManager->registerCvar("rlcoach_pad_slide", "3", "Controller button for powerslide / air roll", true, true, 0, true, PB_Count - 1).bindTo(padSlide);
	cvarManager->registerCvar("rlcoach_pad_roll_left", "0", "Controller button for air roll left", true, true, 0, true, PB_Count - 1).bindTo(padRollLeft);
	cvarManager->registerCvar("rlcoach_pad_roll_right", "0", "Controller button for air roll right", true, true, 0, true, PB_Count - 1).bindTo(padRollRight);

	cvarManager->registerCvar("rlcoach_target_hitbox", "1", "Target car outline: 0 off, 1 for your touch, 2 for the earliest intercept", true, true, 0, true, 2).bindTo(targetHitbox);

	showUngraded = std::make_shared<bool>(false);
	cvarManager->registerCvar("rlcoach_viewer_show_ungraded", "0", "Rep viewer: also list reps with no grade (bailed or reset)", true, true, 0, true, 1).bindTo(showUngraded);

	viewerRange = std::make_shared<int>(0);
	cvarManager->registerCvar("rlcoach_viewer_range", "0", "Rep viewer list: 0 today, 1 last 7 days, 2 starred, 3 all", true, true, 0, true, 3).bindTo(viewerRange);

	cvarManager->registerNotifier("rlcoach_hud_toggle_compact", [this](std::vector<std::string>) {
		cvarManager->getCvar("rlcoach_hud_compact").setValue(*hudCompact ? 0 : 1);
	}, "Switch the panel between full and compact", PERMISSION_ALL);
}

void RLCoach::RenderLookSettings()
{
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	// Panel
	bool compact = *hudCompact;
	if (ImGui::Checkbox("Compact panel (just the essentials)", &compact)) setCvar("rlcoach_hud_compact", compact ? 1.0f : 0.0f);
	ImGui::SameLine();
	ImGui::TextDisabled("bind rlcoach_hud_toggle_compact to switch in game");
	float sc = *hudScale;
	if (ImGui::SliderFloat("Panel size", &sc, 0.6f, 2.0f, "%.2fx")) setCvar("rlcoach_hud_scale", sc);
	float px = *hudX * 100.0f, py = *hudY * 100.0f;
	if (ImGui::SliderFloat("Panel left/right", &px, 0.0f, 90.0f, "%.0f%%")) setCvar("rlcoach_hud_x", px / 100.0f);
	if (ImGui::SliderFloat("Panel up/down", &py, 0.0f, 90.0f, "%.0f%%")) setCvar("rlcoach_hud_y", py / 100.0f);

	int units = *speedUnits;
	const char* unitNames[3] = { "uu/s (game units)", "km/h", "mph" };
	if (ImGui::Combo("Speed units", &units, unitNames, 3)) setCvar("rlcoach_speed_units", static_cast<float>(units));

	int th = *targetHitbox;
	const char* thNames[3] = { "Off", "Where your car needed to be for your touch", "Where it needed to be at the earliest intercept" };
	if (ImGui::Combo("Target car outline", &th, thNames, 3)) setCvar("rlcoach_target_hitbox", static_cast<float>(th));
	ImGui::TextDisabled("A yellow outline of your car's hitbox, placed and aimed to make the ideal hit. Shows in replays and practice.");
}

void RLCoach::RenderPadSettings()
{
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	int mode = *inputDisplay;
	const char* modes[4] = { "Off", "Text on the panel", "PS4 controller", "Xbox controller" };
	if (ImGui::Combo("Show inputs", &mode, modes, 4)) setCvar("rlcoach_input_display", static_cast<float>(mode));
	if (mode < 2) return;

	int theme = *padTheme;
	const char* themes[2] = { "Dark", "Light" };
	if (ImGui::Combo("Controller color", &theme, themes, 2)) setCvar("rlcoach_pad_theme", static_cast<float>(theme));
	float sc = *padScale;
	if (ImGui::SliderFloat("Controller size", &sc, 0.5f, 2.5f, "%.2fx")) setCvar("rlcoach_pad_scale", sc);
	float px = *padX * 100.0f, py = *padY * 100.0f;
	if (ImGui::SliderFloat("Controller left/right", &px, 0.0f, 95.0f, "%.0f%%")) setCvar("rlcoach_pad_x", px / 100.0f);
	if (ImGui::SliderFloat("Controller up/down", &py, 0.0f, 95.0f, "%.0f%%")) setCvar("rlcoach_pad_y", py / 100.0f);

	ImGui::Spacing();
	ImGui::TextDisabled("Your button layout: match your in-game binds (Settings > Controls)");
	PadStyle style = mode == 3 ? PadStyle::Xbox : PadStyle::PS4;
	const char* names[PB_Count];
	for (int b = 0; b < PB_Count; b++) names[b] = ControllerOverlay::ButtonName(b, style);
	struct Row { const char* label; const char* cvar; std::shared_ptr<int>* value; };
	Row rows[5] = {
		{ "Jump", "rlcoach_pad_jump", &padJump },
		{ "Boost", "rlcoach_pad_boost", &padBoost },
		{ "Powerslide / air roll", "rlcoach_pad_slide", &padSlide },
		{ "Air roll left", "rlcoach_pad_roll_left", &padRollLeft },
		{ "Air roll right", "rlcoach_pad_roll_right", &padRollRight },
	};
	for (Row& r : rows)
	{
		int v = **r.value;
		if (ImGui::Combo(r.label, &v, names, PB_Count)) setCvar(r.cvar, static_cast<float>(v));
	}
}
