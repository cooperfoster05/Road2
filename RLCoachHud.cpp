#include "pch.h"
#include "RLCoach.h"
#include "UiStyle.h"
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
	// Panel colors: style A (Nov 4, the design canvas's panel mock): navy, the team blue and orange, slanted pieces.
	// The panel draws with the game's own text (the canvas can't use the window's fonts), so the look is in the shapes.
	const Rgba kPanelBg{ 11, 15, 26, 235 };
	const Rgba kHeaderBg{ 14, 19, 34, 245 };
	const Rgba kBorder{ 30, 42, 68, 255 };
	const Rgba kTitle{ 238, 242, 248, 255 };
	const Rgba kOrange{ 255, 138, 31, 255 };
	const Rgba kSection{ 133, 146, 171, 255 };
	const Rgba kDivider{ 30, 42, 68, 230 };
	const Rgba kTrack{ 26, 36, 64, 255 };
	const Rgba kCallout{ 18, 26, 44, 245 };

	// The panel's lines come with colors from the old palette (here and in RLCoach.cpp); this maps them onto the new one
	Rgba Restyle(unsigned char r, unsigned char g, unsigned char b)
	{
		struct Map { unsigned char r, g, b; Rgba to; };
		static const Map kMap[] = {
			{ 240, 244, 250, { 238, 242, 248, 255 } }, { 238, 242, 250, { 238, 242, 248, 255 } }, { 225, 230, 238, { 238, 242, 248, 255 } },
			{ 220, 224, 232, { 238, 242, 248, 255 } }, { 200, 205, 215, { 200, 212, 228, 255 } }, { 190, 190, 190, { 167, 179, 200, 255 } },
			{ 150, 160, 178, { 167, 179, 200, 255 } }, { 130, 138, 152, { 133, 146, 171, 255 } }, { 110, 118, 132, { 133, 146, 171, 255 } },
			{ 115, 230, 115, { 61, 220, 132, 255 } }, { 120, 230, 120, { 61, 220, 132, 255 } },
			{ 255, 204, 77, { 255, 176, 32, 255 } }, { 255, 200, 0, { 255, 176, 32, 255 } }, { 255, 200, 60, { 255, 176, 32, 255 } },
			{ 200, 170, 60, { 255, 176, 32, 255 } }, { 255, 160, 80, { 255, 138, 31, 255 } }, { 255, 205, 130, { 253, 186, 116, 255 } },
			{ 255, 115, 102, { 255, 90, 90, 255 } }, { 255, 120, 120, { 255, 90, 90, 255 } }, { 255, 90, 90, { 255, 90, 90, 255 } },
			{ 153, 204, 255, { 94, 200, 229, 255 } }, { 160, 205, 255, { 125, 211, 252, 255 } }, { 120, 160, 255, { 125, 184, 255, 255 } },
			{ 80, 200, 230, { 125, 184, 255, 255 } }, { 120, 200, 255, { 125, 184, 255, 255 } },
			{ 200, 200, 200, { 167, 179, 200, 255 } }, { 160, 160, 160, { 133, 146, 171, 255 } },
		};
		for (const Map& m : kMap) if (m.r == r && m.g == g && m.b == b) return m.to;
		return Rgba{ r, g, b, 255 };
	}

	// The grade color (as in the rep viewer: 5 cyan, 4 green, 3 amber, 2 orange, 1 red)
	Rgba GradeRgba(int o)
	{
		if (HudDraw::gColorBlind)
		{
			static const Rgba kSet[6] = { { 133, 146, 171, 255 }, { 204, 121, 167, 255 }, { 230, 159, 0, 255 }, { 240, 228, 66, 255 }, { 0, 158, 115, 255 }, { 86, 180, 233, 255 } };
			return kSet[std::clamp(o, 0, 5)];
		}
		if (o >= 5) return { 45, 226, 230, 255 };
		if (o == 4) return { 61, 220, 132, 255 };
		if (o == 3) return { 255, 176, 32, 255 };
		if (o == 2) return { 255, 138, 31, 255 };
		if (o == 1) return { 255, 90, 90, 255 };
		return { 133, 146, 171, 255 };
	}
}

void RLCoach::RenderHud(CanvasWrapper canvas)
{
	UpdateRatingKeys();
	Units::SpeedMode() = *speedUnits;

	if (!IsOfflinePractice()) return;
	CarWrapper car = gameWrapper->GetLocalCar();
	if (car.IsNull()) return;
	DrawBotStatus(canvas);
	DrawCombineHud(canvas);
	DrawLearnRecoveryHud(canvas);
	if (combinePhase != CombinePhase::Off) return;   // the Combine shows its own line only

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
		// Practicing a line: its inputs at this moment are the ghost (where you are in the rep's time)
		PadInput ghost;
		bool haveGhost = false, fineAim = false;
		if (practiceState != PracticeState::Off && practiceGhostLine && *ghostOpacity > 0.01f && !practiceRefFrames.empty())
		{
			const int t = practiceStartTick + static_cast<int>(practiceLive.size());
			if (t >= 0 && t < static_cast<int>(practiceRefFrames.size()))
			{
				const Frame& g = practiceRefFrames[t];
				ghost = PadInput{ g.throttle, g.steer, g.pitch, g.yaw, g.roll, g.jump, g.boost, g.handbrake, -1.0f };
				// Nov 1: the stick and triggers lightly smoothed for display (the average of 4 ticks either side, about 0.07 s), so an
				// engine line's frame-to-frame stick changes don't make the ghost shake. Buttons stay exact.
				{
					const int n = static_cast<int>(practiceRefFrames.size()), a = std::max(0, t - 4), b = std::min(n - 1, t + 4);
					PadInput avg{};
					for (int k = a; k <= b; k++)
					{
						const Frame& h = practiceRefFrames[k];
						avg.throttle += h.throttle; avg.steer += h.steer; avg.pitch += h.pitch; avg.yaw += h.yaw;
					}
					const float inv = 1.0f / static_cast<float>(b - a + 1);
					ghost.throttle = avg.throttle * inv; ghost.steer = avg.steer * inv; ghost.pitch = avg.pitch * inv;
					ghost.yaw = avg.yaw * inv;   // (roll is a button on most pads: left exact)
				}
				haveGhost = true;
				for (const auto& f : practiceFineSpans) fineAim = fineAim || (t >= f.first && t <= f.second);
			}
		}
		ControllerOverlay::Draw(canvas, pad, *inputDisplay == 3 ? PadStyle::Xbox : PadStyle::PS4,
			*padTheme == 1 ? PadTheme::Light : PadTheme::Dark, layout,
			*padX * screen.X, *padY * screen.Y, *padScale * (screen.Y / 1080.0f), haveGhost ? &ghost : nullptr, *ghostOpacity, fineAim);
	}

	if (!*hudEnabled) return;
	DrawPanel(canvas, car, pad);
}

void RLCoach::DrawPanel(CanvasWrapper& canvas, CarWrapper& car, const PadInput& pad)
{
	// One line on the panel. Detail lines are hidden in compact mode.
	struct HudLine
	{
		enum Kind { Text, Section, Callout, Keys, RecoveryKeys } kind = Text;   // Callout: the thing to fix (orange edge); Keys: the rating keys as chips
		std::vector<PanelChunk> chunks;
		float scale = 1.0f;
		bool detail = false;
		int grade = 0;      // 1-5: a slanted grade badge leads the line
		int picked = 0;     // Keys / RecoveryKeys: the one you pressed (0 none)
	};
	std::vector<HudLine> lines;
	auto add = [&](std::string text, unsigned char r = 225, unsigned char g = 230, unsigned char b = 238, bool detail = false, float scale = 1.0f) {
		HudLine l;
		l.chunks = { { std::move(text), r, g, b } };
		l.detail = detail;
		l.scale = scale;
		lines.push_back(std::move(l));
	};
	auto addChunks = [&](const std::vector<PanelChunk>& chunks, bool detail = false, int grade = 0) {
		HudLine l;
		l.chunks = chunks;
		l.detail = detail;
		l.grade = grade;
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
	Rgba tagCol{ 133, 146, 171, 255 };

	if (practiceState != PracticeState::Off)
	{
		tag = "PRACTICE";
		tagCol = { 61, 220, 132, 255 };
		int pct = static_cast<int>(practiceSpeed * 100.0f + 0.5f);
		add(practiceLabel, 240, 244, 250);
		add(std::format("{}% speed   {:.2f}s lead-in{}", pct, practiceLeadIn, practiceAutoRamp ? "   auto-ramp" : ""), 150, 160, 178, true);
		if (practiceState == PracticeState::Frozen) add("Move to start the attempt", 255, 204, 77);
		else add("GO", 115, 230, 115);
		if (learnActive)
		{
			tag = "LEARN"; tagCol = { 255, 138, 31, 255 };
			const bool lastStage = learnStage + 1 >= static_cast<int>(learnLesson.stages.size());
			if (lastStage && learnProgress.completed) add("Lesson complete", 74, 222, 128);
			else if (!learnWarmup && learnStage == learnProgress.unlocked)
				add(std::format("{}: {}/3 of your last 5 at 100% scored 4-5", lastStage ? "To complete the lesson" : "Next stage",
					std::min(3, learnProgress.PassedRecent(learnStage))), 150, 160, 178);
			if (learnAutoRamp && practiceSpeed < 0.999f) add(std::format("Auto ramp: {} of 2 at 4-5 to speed up", learnProgress.rampStreak), 150, 160, 178, true);
		}
		else
		{
			std::string bySpeed;
			for (const auto& [p, h] : practiceHitsByPct) bySpeed += std::format("{}{}%: {}", bySpeed.empty() ? "" : "   ", p, h);
			add(std::format("Hits {}/{}   streak {}", practiceHitsTotal, practiceAttempts, practiceStreak));
			if (!bySpeed.empty()) add(bySpeed, 150, 160, 178, true);
		}
		inputText();
		if (!practiceResultLines.empty())
		{
			section("LAST ATTEMPT");
			for (const PanelGradeLine& gl : practiceResultLines) addChunks(gl.chunks, false, gl.grade);
		}
		if (!practiceRefLabel.empty()) add(practiceRefLabel, 130, 138, 152, true);
		add("Retry: road2_practice_retry   Stop: road2_practice_exit", 110, 118, 132, true);
	}
	else if (viewerActive && !viewerFrames.empty())
	{
		tag = "REPLAY";
		tagCol = { 46, 139, 255, 255 };
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
			addChunks({ { Grader::OverallName(viewerGrade.overall), r, g, b } }, false, viewerGrade.overall);
			if (viewerShot.valid && viewerShot.type != ShotType::Aerial)
				add(std::format("Graded as a {}", viewerShot.type == ShotType::Ground ? "ground shot"
					: viewerShot.type == ShotType::Dribble ? "dribble" : viewerShot.type == ShotType::AirDribble ? "air dribble"
					: viewerShot.type == ShotType::FlipReset ? "flip reset" : viewerShot.type == ShotType::CeilingShot ? "ceiling shot" : "double tap"), 150, 160, 178, true);
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
		if (!*recordingEnabled) { tag = "OFF"; add("Recording off  (F2 > Plugins > Road2)", 150, 160, 178); }
		else if (state == RepState::Recording && !pendingEndReason.empty())
		{
			tag = "REC"; tagCol = { 255, 90, 90, 255 };
			add(std::format("Shot over ({}), catching your recovery", pendingEndReason), 240, 244, 250);
		}
		else if (state == RepState::Recording) { tag = "REC"; tagCol = { 255, 90, 90, 255 }; add("Recording rep", 240, 244, 250); }
		else if (state == RepState::Armed) { tag = "ARMED"; tagCol = { 255, 176, 32, 255 }; add("Rep starts when you move", 240, 244, 250); }
		else { tag = "READY"; add("Reset the shot or ball to start", 200, 205, 215); }

		if (*recordingEnabled) add(std::format("{} rep{} this session", repsThisSession, repsThisSession == 1 ? "" : "s"), 150, 160, 178, true);
		add(std::format("{}{}   boost {:.0f}   {}", Units::Speed(speed), speed >= 2200.0f ? " (supersonic)" : "",
			pad.boostAmount < 0 ? 0.0f : pad.boostAmount, car.IsOnGround() ? "grounded" : "airborne"), 150, 160, 178, true);
		inputText();

		if (*recordingEnabled && state == RepState::Recording && queuedRating > 0)
			add(std::format("This rep will be rated {}: {}", queuedRating, RatingName(queuedRating)), 120, 230, 120);
		if (*recordingEnabled && state == RepState::Recording && queuedRecovery > 0)
			add(std::format("Recovery will be rated {}", RecoveryName(queuedRecovery)), 120, 230, 120);

		if (*recordingEnabled && haveLastRep)
		{
			section("LAST REP");
			add(std::format("{}   {:.2f}s   {} touch{}", lastRep.endReason, lastRep.durationS,
				lastRep.touches, lastRep.touches == 1 ? "" : "es"), 150, 160, 178, true);
			for (const PanelGradeLine& gl : lastRepGradeLines) addChunks(gl.chunks, false, gl.grade);
			if (*manualGrading)
			{
				// (style A) the keys as chips, the one you pressed lit up
				section(lastRating == 0 ? "RATE IT" : "YOU RATED IT");
				HudLine k; k.kind = HudLine::Keys; k.picked = lastRating; lines.push_back(k);
				HudLine rk; rk.kind = HudLine::RecoveryKeys; rk.picked = lastRecovery; rk.detail = true; lines.push_back(rk);
			}
			else if (lastRating > 0) add(std::format("You rated it {}: {}", lastRating, RatingName(lastRating)), 120, 230, 120);
		}
		if (!statusMessage.empty()) add(statusMessage, 130, 138, 152, true);
	}

	if (*showTiming && *devTools)
		add(std::format("Plugin cost  tick {:.3f} ms (peak {:.2f})   draw {:.2f} ms (peak {:.2f})",
			perfTick.avgMs + perfInput.avgMs, perfTick.peakMs + perfInput.peakMs, perfDraw.avgMs, perfDraw.peakMs), 130, 200, 160);

	// The thing to fix (the replay's Fix line, the last rep's Fix and coaching cue) as an orange-edged callout
	for (HudLine& l : lines)
		if (l.kind == HudLine::Text && !l.chunks.empty() && l.grade == 0 &&
			(l.chunks[0].text.rfind("Fix", 0) == 0 || (l.chunks[0].r == 255 && l.chunks[0].g == 205 && l.chunks[0].b == 130)))
			l.kind = HudLine::Callout;

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
	const float padding = 12.0f * S, lineH = 17.0f * S, headerH = 28.0f * S, radius = 9.0f * S;
	const float textScale = S;
	const float gaugeR = 10.0f * S, gaugeW = gaugeR * 2.0f + 10.0f * S;   // the small grade gauge and the room it takes

	auto lineHeight = [&](const HudLine& l) {
		if (l.kind == HudLine::Section) return lineH * 0.8f + 11.0f * S;
		if (l.kind == HudLine::Callout) return lineH + 10.0f * S;
		if (l.kind == HudLine::Keys || l.kind == HudLine::RecoveryKeys) return lineH + 9.0f * S;
		return l.grade > 0 ? std::max(lineH * l.scale, gaugeR * 2.0f + 8.0f * S) : lineH * l.scale;
	};
	float bodyH = 0.0f, width = 260.0f * S;
	for (const HudLine& l : lines)
	{
		bodyH += lineHeight(l);
		float w = l.grade > 0 ? gaugeW : l.kind == HudLine::Callout ? 14.0f * S : 0.0f;
		if (l.kind == HudLine::Keys) w = 330.0f * S;
		for (const PanelChunk& c : l.chunks) w += TextSize(canvas, c.text, l.scale * textScale).X;
		width = std::max(width, w + padding * 2);
	}
	float tagW = TextSize(canvas, tag, 0.75f * textScale).X + (tag == "REC" ? 30.0f : 22.0f) * S;
	width = std::max(width, TextSize(canvas, "ROAD2", 1.1f * textScale).X + tagW + padding * 3);
	const float height = headerH + padding * 0.7f + bodyH + padding * 0.7f;

	// ---- Draw ----
	const float rr = 5.0f * S;   // style A: nearly square corners
	RoundBox(canvas, x, y, width, height, rr, kPanelBg);
	RoundBox(canvas, x, y, width, headerH, rr, kHeaderBg);
	Box(canvas, x, y + headerH - rr, width, rr, kHeaderBg);   // square off the header's bottom corners
	Box(canvas, x + 1.0f, y + headerH, width - 2.0f, 1.0f, kDivider);
	RoundOutline(canvas, x, y, width, height, rr, kBorder);
	(void)radius;

	// The wordmark: ROAD in white, the 2 in team orange
	const float ts = 1.1f * textScale;
	float titleH = TextSize(canvas, "ROAD", ts).Y;
	Text(canvas, x + padding, y + (headerH - titleH) * 0.5f, "ROAD", ts, kTitle);
	Text(canvas, x + padding + TextSize(canvas, "ROAD", ts).X, y + (headerH - titleH) * 0.5f, "2", ts, kOrange);

	// The mode tag: a slanted chip filled with its color, dark capitals (a quiet dark chip when off)
	float pillH = headerH - 10.0f * S;
	float pillX = x + width - padding - tagW, pillY = y + (headerH - pillH) * 0.5f;
	const bool quietTag = tag == "OFF" || tag == "READY";
	SlantFill(canvas, pillX, pillY, tagW, pillH, quietTag ? kTrack : tagCol);
	const Rgba tagText = quietTag ? Rgba{ 167, 179, 200, 255 } : Rgba{ 10, 14, 24, 255 };
	if (tag == "REC") Disc(canvas, pillX + 10.0f * S, pillY + pillH * 0.5f, 3.0f * S, tagText);
	TextCentered(canvas, pillX + tagW * 0.5f + (tag == "REC" ? 5.0f * S : 0.0f), pillY + pillH * 0.5f, tag, 0.75f * textScale, tagText);

	float cy = y + headerH + padding * 0.7f;
	bool first = true;
	for (const HudLine& l : lines)
	{
		float cx = x + padding;
		const float lh = lineHeight(l);
		if (l.kind == HudLine::Section)
		{
			// the name in small capitals with a rule after it
			const float sy = cy + 7.0f * S;
			const Vector2F tsz = TextSize(canvas, l.chunks[0].text, l.scale * textScale);
			Text(canvas, cx, sy, l.chunks[0].text, l.scale * textScale, kSection, false);
			Box(canvas, cx + tsz.X + 8.0f * S, sy + tsz.Y * 0.5f, width - padding * 2 - tsz.X - 8.0f * S, 1.0f, kDivider);
		}
		else if (l.kind == HudLine::Callout)
		{
			// the thing to fix: a dark row with an orange edge
			Box(canvas, cx, cy + 3.0f * S, width - padding * 2, lh - 6.0f * S, kCallout);
			Box(canvas, cx, cy + 3.0f * S, 3.0f * S, lh - 6.0f * S, kOrange);
			float tx = cx + 10.0f * S;
			const float ty = cy + (lh - lineH) * 0.5f;
			for (size_t k = 0; k < l.chunks.size(); k++)
			{
				const PanelChunk& c = l.chunks[k];
				const Rgba col = k == 0 ? Rgba{ 255, 170, 90, 255 } : Restyle(c.r, c.g, c.b);
				Text(canvas, tx, ty, c.text, l.scale * textScale, col);
				tx += TextSize(canvas, c.text, l.scale * textScale).X;
			}
		}
		else if (l.kind == HudLine::Keys || l.kind == HudLine::RecoveryKeys)
		{
			// the rating keys as slanted chips: each with its grade color along the bottom, the one you pressed filled
			const bool rec = l.kind == HudLine::RecoveryKeys;
			const char* labels5[5] = { "1 Missed", "2 Close", "3 Bad-in", "4 Good-in", "5 Great-in" };
			const char* labels3[3] = { "6 Poor", "7 Eh", "8 Good" };
			const int n = rec ? 3 : 5;
			float kx = cx;
			const float ch = lh - 7.0f * S, cyy = cy + 3.0f * S, sc = 0.72f * textScale;
			if (rec)
			{
				Text(canvas, kx, cyy + (ch - TextSize(canvas, "RECOVERY", sc).Y) * 0.5f, "RECOVERY", sc, kSection, false);
				kx += TextSize(canvas, "RECOVERY", sc).X + 10.0f * S;
			}
			const float gap = 4.0f * S, cw = (x + width - padding - kx - gap * (n - 1)) / n;
			for (int k = 0; k < n; k++)
			{
				const int v = k + 1;
				const Rgba gc = rec ? GradeRgba(v == 1 ? 1 : v == 2 ? 3 : 4) : GradeRgba(v);
				const bool on = l.picked == v;
				const float bx = kx + k * (cw + gap);
				SlantFill(canvas, bx, cyy, cw, ch, on ? gc : kTrack);
				if (!on) Box(canvas, bx - ch * 0.106f + 1.0f, cyy + ch - 2.0f * S, cw, 2.0f * S, gc);
				TextCentered(canvas, bx + cw * 0.5f, cyy + ch * 0.5f, rec ? labels3[k] : labels5[k], sc, on ? Rgba{ 10, 14, 24, 255 } : Rgba{ 200, 210, 226, 255 });
			}
		}
		else
		{
			float ty = cy;
			if (l.grade > 0)
			{
				// The grade as a slanted badge in its color with a hard shadow (style A, like the viewer's)
				const float bs = gaugeR * 2.0f + 2.0f * S, bx = cx + 1.0f * S, by = cy + (lh - bs) * 0.5f;
				const Rgba gc = GradeRgba(l.grade);
				SlantFill(canvas, bx + 3.0f * S, by + 3.0f * S, bs, bs, Rgba{ static_cast<unsigned char>(gc.r / 4), static_cast<unsigned char>(gc.g / 4), static_cast<unsigned char>(gc.b / 4), 255 });
				SlantFill(canvas, bx, by, bs, bs, gc);
				TextCentered(canvas, bx + bs * 0.5f, by + bs * 0.5f, std::to_string(l.grade), 1.05f * textScale, Rgba{ 10, 14, 24, 255 });
				cx += gaugeW;
				ty = cy + (lh - lineH * l.scale) * 0.5f - 1.0f * S;
			}
			for (size_t k = 0; k < l.chunks.size(); k++)
			{
				const PanelChunk& c = l.chunks[k];
				const Rgba col = l.grade > 0 && k == 0 ? GradeRgba(l.grade) : Restyle(c.r, c.g, c.b);
				Text(canvas, cx, ty, c.text, l.scale * textScale, col);
				cx += TextSize(canvas, c.text, l.scale * textScale).X;
			}
		}
		cy += lh;
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
	ghostOpacity = std::make_shared<float>(0.7f);
	windowSize = std::make_shared<float>(1.0f);

	cvarManager->registerCvar("road2_hud_compact", "0", "Panel shows only the essentials", true, true, 0, true, 1).bindTo(hudCompact);
	cvarManager->registerCvar("road2_hud_scale", "1", "Panel size", true, true, 0.6f, true, 2.0f).bindTo(hudScale);
	cvarManager->registerCvar("road2_hud_x", "0.016", "Panel left edge (fraction of the screen width)", true, true, 0, true, 1).bindTo(hudX);
	cvarManager->registerCvar("road2_hud_y", "0.165", "Panel top edge (fraction of the screen height)", true, true, 0, true, 1).bindTo(hudY);
	cvarManager->registerCvar("road2_speed_units", "0", "Speed units: 0 uu/s, 1 km/h, 2 mph", true, true, 0, true, 2).bindTo(speedUnits);
	cvarManager->registerCvar("road2_input_display", "2", "Inputs: 0 none, 1 text on the panel, 2 PS4 controller, 3 Xbox controller", true, true, 0, true, 3).bindTo(inputDisplay);
	cvarManager->registerCvar("road2_pad_x", "0.016", "Controller overlay left edge (fraction of the screen width)", true, true, 0, true, 1).bindTo(padX);
	cvarManager->registerCvar("road2_pad_y", "0.80", "Controller overlay top edge (fraction of the screen height)", true, true, 0, true, 1).bindTo(padY);
	cvarManager->registerCvar("road2_pad_scale", "1", "Controller overlay size", true, true, 0.5f, true, 2.5f).bindTo(padScale);
	cvarManager->registerCvar("road2_pad_theme", "0", "Controller overlay color: 0 dark, 1 light", true, true, 0, true, 1).bindTo(padTheme);
	cvarManager->registerCvar("road2_pad_jump", "1", "Controller button for jump", true, true, 0, true, PB_Count - 1).bindTo(padJump);
	cvarManager->registerCvar("road2_pad_boost", "2", "Controller button for boost", true, true, 0, true, PB_Count - 1).bindTo(padBoost);
	cvarManager->registerCvar("road2_pad_slide", "3", "Controller button for powerslide / air roll", true, true, 0, true, PB_Count - 1).bindTo(padSlide);
	cvarManager->registerCvar("road2_pad_roll_left", "0", "Controller button for air roll left", true, true, 0, true, PB_Count - 1).bindTo(padRollLeft);
	cvarManager->registerCvar("road2_pad_roll_right", "0", "Controller button for air roll right", true, true, 0, true, PB_Count - 1).bindTo(padRollRight);

	cvarManager->registerCvar("road2_ghost_opacity", "0.7", "Learn: the controller ghost when practicing a line (0 = off, 1 = solid)", true, true, 0, true, 1).bindTo(ghostOpacity);
	cvarManager->registerCvar("road2_window_size", "1", "Size of the Road2 window (rep viewer, Learn, the Combine)", true, true, 0.6f, true, 1.6f).bindTo(windowSize);
	cvarManager->registerCvar("road2_line_ghost", "1", "Show the better line's car as a ghost while your rep plays", true, true, 0, true, 1).bindTo(lineGhost);
	cvarManager->registerCvar("road2_target_hitbox", "1", "Target car outline: 0 off, 1 for your touch, 2 for the earliest intercept", true, true, 0, true, 2).bindTo(targetHitbox);

	showUngraded = std::make_shared<bool>(false);
	cvarManager->registerCvar("road2_viewer_show_ungraded", "0", "Rep viewer: also list reps with no grade (bailed or reset)", true, true, 0, true, 1).bindTo(showUngraded);

	viewerRange = std::make_shared<int>(0);
	cvarManager->registerCvar("road2_viewer_range", "0", "Rep viewer list: 0 today, 1 last 7 days, 2 starred, 3 all", true, true, 0, true, 3).bindTo(viewerRange);

	// The rep viewer's Filter button (Nov 1)
	viewerGradeFrom = std::make_shared<int>(0);
	cvarManager->registerCvar("road2_viewer_grade_from", "0", "Rep viewer filter: whose grade counts: 0 yours or auto, 1 yours, 2 auto", true, true, 0, true, 2).bindTo(viewerGradeFrom);
	viewerGrades = std::make_shared<int>(31);
	cvarManager->registerCvar("road2_viewer_grades", "31", "Rep viewer filter: which grades show (1 = grade 1, 2 = grade 2, 4 = grade 3, 8 = grade 4, 16 = grade 5; add them up)", true, true, 1, true, 31).bindTo(viewerGrades);
	viewerShotFilter = std::make_shared<int>(0);
	cvarManager->registerCvar("road2_viewer_shot", "0", "Rep viewer filter: 0 all shots, 1 aerials, 2 ground, 3 double taps, 4 dribbles, 5 air dribbles, 6 flip resets, 7 ceiling shots", true, true, 0, true, 7).bindTo(viewerShotFilter);

	cvarManager->registerNotifier("road2_hud_toggle_compact", [this](std::vector<std::string>) {
		cvarManager->getCvar("road2_hud_compact").setValue(*hudCompact ? 0 : 1);
	}, "Switch the panel between full and compact", PERMISSION_ALL);
}

void RLCoach::RenderLookSettings()
{
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	{
		bool cb = *colorBlind;
		if (UiStyle::Checkbox("Colour-blind friendly colours (grades without red against green)", &cb)) setCvar("road2_colorblind", cb ? 1.0f : 0.0f);
	}
	// Panel
	UiStyle::Heading("The panel");
	bool compact = *hudCompact;
	if (UiStyle::Checkbox("Compact panel (just the essentials)", &compact)) setCvar("road2_hud_compact", compact ? 1.0f : 0.0f);
	ImGui::SameLine();
	ImGui::TextDisabled("bind road2_hud_toggle_compact to switch in game");
	float sc = *hudScale;
	if (UiStyle::SliderFloat("Panel size", &sc, 0.6f, 2.0f, "%.2fx")) setCvar("road2_hud_scale", sc);
	float px = *hudX * 100.0f, py = *hudY * 100.0f;
	if (UiStyle::SliderFloat("Panel left/right", &px, 0.0f, 90.0f, "%.0f%%")) setCvar("road2_hud_x", px / 100.0f);
	if (UiStyle::SliderFloat("Panel up/down", &py, 0.0f, 90.0f, "%.0f%%")) setCvar("road2_hud_y", py / 100.0f);

	// The Road2 window (Nov 1): its height always fits what it shows; this sets the size of everything in it. Dragging
	// the window narrower also shrinks it, so you can make it small without losing anything.
	UiStyle::Heading("The Road2 window");
	float ws = *windowSize;
	if (UiStyle::SliderFloat("Road2 window size", &ws, 0.6f, 1.6f, "%.2fx")) setCvar("road2_window_size", std::round(ws * 20.0f) / 20.0f);
	ImGui::TextDisabled("The window always fits what it shows. Drag it narrower to shrink everything in it too.");

	UiStyle::Heading("Units and outlines");
	int units = *speedUnits;
	const char* unitNames[3] = { "uu/s (game units)", "km/h", "mph" };
	if (UiStyle::Combo("Speed units", &units, unitNames, 3)) setCvar("road2_speed_units", static_cast<float>(units));

	int th = *targetHitbox;
	const char* thNames[3] = { "Off", "Where your car needed to be for your touch", "Where it needed to be at the earliest intercept" };
	if (UiStyle::Combo("Target car outline", &th, thNames, 3)) setCvar("road2_target_hitbox", static_cast<float>(th));
	{
		bool lg = *lineGhost;
		if (UiStyle::Checkbox("Ghost car on the better line (while your rep plays)", &lg)) setCvar("road2_line_ghost", lg ? 1.0f : 0.0f);
	}
	ImGui::TextDisabled("A yellow outline of your car's hitbox, placed and aimed to make the ideal hit. Shows in replays and practice.");
}

void RLCoach::RenderPadSettings()
{
	auto setCvar = [this](const std::string& name, float value) {
		gameWrapper->Execute([this, name, value](GameWrapper*) { cvarManager->getCvar(name).setValue(value); });
	};

	int mode = *inputDisplay;
	const char* modes[4] = { "Off", "Text on the panel", "PS4 controller", "Xbox controller" };
	if (UiStyle::Combo("Show inputs", &mode, modes, 4)) setCvar("road2_input_display", static_cast<float>(mode));
	if (mode < 2) return;

	int theme = *padTheme;
	const char* themes[2] = { "Dark", "Light" };
	if (UiStyle::Combo("Controller color", &theme, themes, 2)) setCvar("road2_pad_theme", static_cast<float>(theme));
	float sc = *padScale;
	if (UiStyle::SliderFloat("Controller size", &sc, 0.5f, 2.5f, "%.2fx")) setCvar("road2_pad_scale", sc);
	float px = *padX * 100.0f, py = *padY * 100.0f;
	if (UiStyle::SliderFloat("Controller left/right", &px, 0.0f, 95.0f, "%.0f%%")) setCvar("road2_pad_x", px / 100.0f);
	if (UiStyle::SliderFloat("Controller up/down", &py, 0.0f, 95.0f, "%.0f%%")) setCvar("road2_pad_y", py / 100.0f);

	// Learn: the line you're practicing, shown on the controller (a cyan ring where its stick is, rings on its buttons)
	ImGui::Spacing();
	UiStyle::Heading("The ghost");
	ImGui::TextDisabled("When you practice a line or a lesson, its inputs show on the controller");
	int ghost = static_cast<int>(std::lround(*ghostOpacity * 100.0f));
	if (UiStyle::SliderInt("Controller ghost", &ghost, 0, 100, ghost == 0 ? "off" : "%d%%")) setCvar("road2_ghost_opacity", ghost / 100.0f);

	ImGui::Spacing();
	UiStyle::Heading("Your button layout");
	ImGui::TextDisabled("Match your in-game binds (Settings > Controls)");
	PadStyle style = mode == 3 ? PadStyle::Xbox : PadStyle::PS4;
	const char* names[PB_Count];
	for (int b = 0; b < PB_Count; b++) names[b] = ControllerOverlay::ButtonName(b, style);
	struct Row { const char* label; const char* cvar; std::shared_ptr<int>* value; };
	Row rows[5] = {
		{ "Jump", "road2_pad_jump", &padJump },
		{ "Boost", "road2_pad_boost", &padBoost },
		{ "Powerslide / air roll", "road2_pad_slide", &padSlide },
		{ "Air roll left", "road2_pad_roll_left", &padRollLeft },
		{ "Air roll right", "road2_pad_roll_right", &padRollRight },
	};
	for (Row& r : rows)
	{
		int v = **r.value;
		if (UiStyle::Combo(r.label, &v, names, PB_Count)) setCvar(r.cvar, static_cast<float>(v));
	}
}
