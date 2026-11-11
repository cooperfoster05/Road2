// No pch on purpose: plain C++ (see Lesson.h)
#include "Lesson.h"
#include "Grader.h"
#include "Segmenter.h"
#include "Mechanics.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <format>
#include <sstream>

namespace
{
	float Len(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }
	float CarBall(const Frame& f) { return Len(f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ); }
	bool Surface(const Frame& f) { return ShotGeometry::OnSurface(f); }
	bool TouchAt(const std::vector<Frame>& fr, int t) { return t > 0 && fr[t].touch && !fr[t - 1].touch; }

	// The pieces of an air dribble attempt, found by what happened (not by the clock, so a slower or faster attempt
	// still lines up)
	struct Events
	{
		int pop = -1;          // the touch that popped it off the wall (or ground)
		bool popOnWall = false;
		float popZ = 0;
		int leave = -1;        // you left the surface after the pop
		int firstAir = -1;     // your first touch in the air
		std::vector<int> air;  // air touches (firstAir included), a few ticks apart count once
	};
	Events FindAd(const std::vector<Frame>& fr, int from, int to)
	{
		Events e;
		const int n = static_cast<int>(fr.size());
		to = std::min(to, n - 1);
		for (int t = std::max(1, from); t <= to; t++)
		{
			if (e.pop < 0)
			{
				if (TouchAt(fr, t) && fr[t].ballZ > 250.0f && Surface(fr[t - 1]))
				{
					e.pop = t; e.popZ = fr[t].ballZ;
					e.popOnWall = fr[t - 1].onWall || fr[t - 1].carZ > 120.0f;
				}
				continue;
			}
			if (e.leave < 0) { if (!Surface(fr[t])) e.leave = t; continue; }
			if (TouchAt(fr, t) && !Surface(fr[t]) && (e.air.empty() || t - e.air.back() > 8))
			{
				e.air.push_back(t);
				if (e.firstAir < 0) e.firstAir = t;
			}
		}
		return e;
	}
	float Push(const std::vector<Frame>& fr, int t)
	{
		const int n = static_cast<int>(fr.size());
		const Frame& a = fr[std::max(0, t - 1)];
		const Frame& b = fr[std::min(n - 1, t + 3)];
		return Len(b.ballVX - a.ballVX, b.ballVY - a.ballVY, b.ballVZ - a.ballVZ);
	}
	float CloseShare(const std::vector<Frame>& fr, int from, int to)
	{
		int n = 0, close = 0;
		for (int t = std::max(0, from); t <= to && t < static_cast<int>(fr.size()); t++) { n++; close += CarBall(fr[t]) <= 230.0f; }
		return n ? static_cast<float>(close) / n : 0.0f;
	}

	// The pop's height: a third to under half way up the wall is right (about 450-950 high)
	int PopLevel(const Events& e, std::string& why)
	{
		if (e.pop < 0) { why = "Never popped it off the wall"; return 1; }
		if (!e.popOnWall) { why = std::format("Popped it off the ground ({:.0f} high), not the wall", e.popZ); return 2; }
		if (e.popZ > 1230.0f) { why = std::format("Popped it {:.0f} high: too far up the wall, you were behind it", e.popZ); return 2; }
		if (e.popZ < 300.0f) { why = std::format("Popped it {:.0f} high: still on the curve, let it roll up a bit more", e.popZ); return 3; }
		if (e.popZ < 450.0f || e.popZ > 950.0f) { why = std::format("Popped it {:.0f} high (450-950 is the sweet spot)", e.popZ); return 4; }
		why = std::format("Popped it {:.0f} high", e.popZ);
		return 5;
	}

	// Air touches before the shot (the shot itself is the finish's)
	std::vector<int> CarryTouches(const Events& e, int shotTick)
	{
		std::vector<int> c;
		for (int t : e.air) if (shotTick < 0 || t < shotTick - 8) c.push_back(t);
		return c;
	}
}

static int AdWatchEnd(const Lesson& L, int stage)
{
	const int popTick = L.popTick, firstAirTick = L.firstAirTick, carryEndTick = L.carryEndTick, shotTick = L.shotTick;
	const auto& model = L.model;
	switch (L.stages[stage].last)
	{
	case 0: return popTick + 30;
	case 1: return firstAirTick + 60;
	case 2: return carryEndTick;
	default: return std::min(static_cast<int>(model.size()) - 1, shotTick + 90);
	}
}

Lesson Lessons::AirDribble(const std::vector<Frame>& model)
{
	Lesson L;
	L.kind = LessonKind::AirDribble;
	L.id = "air_dribble";
	L.name = "Air dribble";
	L.model = model;
	if (model.size() < 60) return Lesson{};
	L.modelShot = ShotGeometry::Analyze(model);
	const ShotAnalysis& s = L.modelShot;
	Events e = FindAd(model, 0, s.touchTick - 1);
	if (!s.valid || e.pop < 0 || e.firstAir < 0) return Lesson{};
	L.popTick = e.pop;
	L.firstAirTick = e.firstAir;
	L.shotTick = s.touchTick;
	L.carryEndTick = s.touchTick - 45;

	LessonStep roll;
	roll.name = "Follow it up the wall";
	roll.what = "The ball rolls up the side wall on its own. Drive after it, match its speed as it climbs, and get under it.";
	roll.why = "Everything after depends on being right behind the ball when it starts up the wall. Late and you're chasing it; too fast and you run past it.";
	roll.tips = { "Head for the wall right away, on a line toward where the ball is going up.",
		"Close in before it climbs: the model is about 400 behind it as it starts up.",
		"Once you're on the wall, ease off and match its speed instead of boosting past it." };
	roll.cue = "Be right behind it as it starts up the wall.";
	roll.start = std::max(1, s.startTick);   // the pack's launch: start once the ball has its speed (before that it sits still)

	LessonStep pop;
	pop.name = "Pop and jump";
	pop.what = "Pop the ball off the wall about a third of the way up, then jump straight after it.";
	pop.why = "A low pop out toward the middle gives you room to carry it. Jump late and you're behind the ball from the start.";
	pop.tips = { std::format("Pop it right after it rolls up the curve (the model pops it {:.0f} high).", e.popZ),
		"Hit it from underneath so it goes up and out, toward the middle of the field.",
		std::format("Jump right after the touch (the model leaves the wall {:.2f} s later).", (s.airStart - e.pop) / 120.0f) };
	pop.cue = "Pop, then jump right away.";
	pop.start = std::max(1, e.pop - 36);

	LessonStep carry;
	carry.name = "The carry";
	carry.what = "Soft touches from under the ball, steering it toward the net.";
	carry.why = "Control comes from staying under the ball. Every hard touch sends it away and you have to chase it.";
	carry.tips = { "Meet the bottom of the ball on its way up, car tilted back.",
		"Feather your boost: the least that keeps you under it.",
		"Keep the ball above your car and the carry pointed at the net." };
	carry.cue = "Stay under it: tap, don't push.";
	carry.start = std::max(1, e.firstAir - 20);

	LessonStep finish;
	finish.name = "The finish";
	finish.what = "Set it up with your last carry touch, then shoot: hard, to a corner.";
	finish.why = "An air dribble that never becomes a shot is just a turnover. The carry earns you a clean look; the finish cashes it in.";
	finish.tips = { "Use the last carry touch to put the ball where you want to shoot it from.",
		"Hit through the ball harder (or flip into it) for the shot.",
		"Aim for a top corner." };
	finish.cue = "Close in, then hit it to a corner.";
	// From just before the last carry touch (Oct 26: from 0.4 s out it was one boost into a crossbar-high ball):
	// set it up with the last touch, then shoot
	{
		int lastCarry = -1;
		for (int t : e.air) if (t < s.touchTick - 8) lastCarry = t;
		finish.start = std::max(1, (lastCarry >= 0 ? lastCarry : s.touchTick - 120) - 20);
	}

	L.steps = { roll, pop, carry, finish };
	L.stages = {
		{ 0, 0, "Step 1: Follow it up the wall" },
		{ 1, 1, "Step 2: Pop and jump" },
		{ 0, 1, "Steps 1-2 together" },
		{ 2, 2, "Step 3: The carry" },
		{ 0, 2, "Steps 1-3 together" },
		{ 3, 3, "Step 4: The finish" },
		{ 0, 3, "The whole air dribble", true },
	};
	return L;
}

static bool AdStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	if (L.StageHasFinish(stage)) return false;   // those end on a goal or a miss
	const int t = static_cast<int>(full.size()) - 1;
	if (t <= startTick + 5) return false;
	Events e = FindAd(full, 1, t);   // from the rep's start: a stage that starts mid-air takes the model's pop as given
	const int last = L.stages[stage].last;
	const Frame& f = full[t];
	if (last == 0)
		return (e.pop >= 0 && t >= e.pop + 20) || t > L.popTick + 120;
	// Ball on the floor once it's been popped: over
	if (e.pop >= 0 && t > e.pop + 10 && f.ballZ < 110.0f) return true;
	if (last == 1)
		return (e.firstAir >= 0 && t >= e.firstAir + 60) || (e.leave >= 0 && t > e.leave + 200) || t > L.firstAirTick + 180;
	// The carry: up when the model's carry is (plus a beat), or once you've lost the ball
	if (t >= L.carryEndTick + 30) return true;
	if (e.firstAir >= 0 && CarBall(f) > 900.0f) return true;
	if (e.pop < 0 && t > L.popTick + 150) return true;
	return false;
}

static StageScore AdScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst,
	const ShotReference& ref, bool compareToModel)
{
	StageScore out;
	const LessonStage& st = L.stages[stage];
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const int end = n - 1;
	Events e = FindAd(full, 1, end);
	ShotAnalysis shot = ShotGeometry::Analyze(full);
	const int shotTick = L.StageHasFinish(stage) && shot.valid ? shot.touchTick : -1;
	struct Why { int score; std::string text; };
	std::vector<Why> whys;

	for (int step = st.first; step <= st.last; step++)
	{
		int sc = 5;
		std::string why;
		switch (step)
		{
		case 0:
		{
			// Following it up: how close behind it you were when it started up the wall (Coop's 5s: about 380 away,
			// his 1s about 500), then the pop
			sc = PopLevel(e, why);
			int k = -1;
			for (int t = std::max(1, startTick); t < (e.pop >= 0 ? e.pop : end); t++) if (full[t].ballZ > 300.0f) { k = t; break; }
			if (k >= 0)
			{
				const float d = CarBall(full[k]);
				const int cap = d <= 450.0f ? 5 : d <= 550.0f ? 4 : 3;
				if (cap < sc) { sc = cap; why = std::format("{:.0f} behind it as it started up the wall (catch up before it climbs); {}", d, why); }
				else why = std::format("{:.0f} behind it as it started up the wall; {}", d, why);
			}
			break;
		}
		case 1:
		{
			// When this stage starts at the pop step, the pop has to happen in it; a chain already scored it in step 1
			std::string popWhy;
			int pl = PopLevel(e, popWhy);
			if (st.first == 1) { sc = pl; why = popWhy; }
			if (e.pop < 0) { sc = 1; why = "Never popped it off the wall"; break; }
			if (e.leave < 0) { sc = 1; why = "Never left the wall"; break; }
			const float j = (e.leave - e.pop) / 120.0f;
			int jl = j <= 0.15f ? 5 : j <= 0.25f ? 4 : 2;
			std::string jw = std::format("left the wall {:.2f} s after the pop", j);
			if (jl < sc) { sc = jl; why = jl <= 2 ? std::format("Jumped late: {} (jump right after it)", jw) : "Jump: " + jw; }
			else if (st.first == 1) why += ", " + jw;
			else why = "Jump: " + jw;
			if (e.firstAir < 0) { sc = std::min(sc, 2); why = "Never got the first touch in the air (get under it right away)"; break; }
			// How good a setup it left you, whatever line you took (Oct 26, Coop: a good setup that isn't exactly the
			// model's still earns its grade): the ball went up off the wall, you met it from underneath, and it's
			// heading toward the net
			{
				float apex = 0;
				for (int t = e.pop; t < e.firstAir; t++) apex = std::max(apex, full[t].ballZ);
				const Frame& ft = full[e.firstAir];
				const bool under = ft.touchZ < ft.ballZ - 20.0f;
				const Frame& after = full[std::min(end, e.firstAir + 6)];
				const float toward = after.ballVY * L.modelShot.goalSign;
				if (apex < 800.0f && sc > 4) { sc = 4; why += std::format(", but it only got {:.0f} high off the wall (pop it up and out)", apex); }
				if (!under && sc > 4) { sc = 4; why += ", first touch wasn't from under the ball"; }
				if (toward < 0.0f && sc > 3) { sc = 3; why += ", and it's heading away from the net"; }
			}
			break;
		}
		case 2:
		{
			if (e.firstAir < 0) { sc = 1; why = "Never got a touch in the air"; break; }
			std::vector<int> c = CarryTouches(e, shotTick);
			const int windowEnd = shotTick >= 0 ? shotTick - 1 : end;
			float push = 0;
			for (int t : c) push += Push(full, t);
			push = c.empty() ? 0.0f : push / c.size();
			const float close = CloseShare(full, e.firstAir, windowEnd);
			const int nt = static_cast<int>(c.size());
			// Before the shot: 3+ soft touches kept close is a 5; 2 soft ones a 4 (the review's air dribble 4 is 3 touches
			// counting the shot)
			if (nt >= 3 && push <= 500.0f && close >= 0.30f) sc = 5;
			else if (nt >= 2 && push <= 700.0f) sc = 4;
			else if (nt >= 2 || (nt == 1 && push <= 700.0f)) sc = 3;
			else if (nt == 1) sc = 2;
			else sc = 1;
			why = std::format("{} touch{} in the air, {:.0f} push each, ball close {:.0f}% of the time", nt, nt == 1 ? "" : "es", push, close * 100.0f);
			if (sc <= 3 && push > 700.0f) why += " (hitting it, not carrying it)";
			else if (sc <= 3 && nt < 2) why += " (keep it going: more soft touches)";
			// What makes it a good carry, whichever way it went (Oct 26): the ball kept up high, never dropped, and
			// carried toward the net (left or right of the model's line doesn't matter)
			{
				float minZ = 1e9f;
				bool dropped = false;
				// (bringing it down into the shot is fine, so the last half second before a shot doesn't count)
				const int lowEnd = shotTick >= 0 ? std::max(e.firstAir + 30, shotTick - 60) : windowEnd;
				for (int t = std::min(lowEnd, e.firstAir + 30); t <= lowEnd; t++)
				{
					minZ = std::min(minZ, full[t].ballZ);
					dropped = dropped || full[t].ballZ < 110.0f;
				}
				const float dur = std::max(0.25f, (windowEnd - e.firstAir) / 120.0f);
				const float gain = (full[windowEnd].ballY - full[e.firstAir].ballY) * L.modelShot.goalSign / dur;
				const int mEnd = std::min(L.carryEndTick, static_cast<int>(L.model.size()) - 1);
				const float mDur = std::max(0.25f, (mEnd - L.firstAirTick) / 120.0f);
				const float mGain = (L.model[mEnd].ballY - L.model[L.firstAirTick].ballY) * L.modelShot.goalSign / mDur;
				if (dropped && shotTick < 0) { sc = 1; why += ", then it hit the floor"; }
				else if (minZ < 350.0f && sc > 2) { sc = 2; why += std::format(", but it dropped to {:.0f} (keep it up)", minZ); }
				else if (minZ < 550.0f && sc > 3) { sc = 3; why += std::format(", but it got low ({:.0f}): stay under it and keep it up", minZ); }
				if (mGain > 0.0f && gain < 0.4f * mGain && sc > 3) { sc = 3; why += ", and it barely moved toward the net"; }
				else if (mGain > 0.0f && gain < 0.65f * mGain && sc > 4) { sc = 4; why += ", a bit slow toward the net"; }
				if (shotTick < 0 && !dropped && CarBall(full[end]) > 900.0f && end < L.carryEndTick && sc > 2) { sc = 2; why += ", then lost it"; }
			}
			break;
		}
		case 3:
		{
			if (!shot.valid) { sc = 1; why = "No shot"; break; }
			const bool high = shot.reachesGoalLine && shot.crossZ >= 460.0f;
			const bool corner = high && std::fabs(shot.crossX) >= 580.0f;
			if (wentIn && corner && shot.shotSpeed >= 2000.0f && !floorFirst) { sc = 5; why = std::format("Top corner at {:.0f}", shot.shotSpeed); }
			else if (wentIn && high && !floorFirst) { sc = 4; why = std::format("Goal, high in the net ({:.0f})", shot.crossZ); }
			else if (wentIn) { sc = 3; why = floorFirst ? "Goal, but it bounced in" : std::format("Goal, low in the net ({:.0f})", shot.crossZ); }
			else if (shot.reachesGoalLine && shot.missDist <= 150.0f) { sc = 2; why = std::format("Just missed ({} by {:.0f})", shot.missDir, shot.missDist); }
			else { sc = 1; why = shot.reachesGoalLine ? std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist) : "Never reached the goal"; }
			break;
		}
		}
		out.stepScores.push_back(sc);
		whys.push_back({ sc, std::format("{}: {}", L.steps[step].name, why) });
	}

	int score = 5;
	for (int s : out.stepScores) score = std::min(score, s);

	// The whole thing is graded like any air dribble rep, by the same grader as the review
	if (st.whole)
	{
		RepSegments segs = Segmenter::Split(full, end);
		RepGrade g = Grader::GradeRep(full, segs, shot, wentIn, floorFirst, ref);
		int overall = g.overall;
		std::string why = g.overallWhy;
		if (shot.valid && shot.type != ShotType::AirDribble) { overall = std::min(overall, wentIn ? 2 : 1); why = "Not an air dribble: " + why; }
		// Graded on the result like any air dribble rep (Oct 26: the weakest step used to drag great runs to a 1-3);
		// the steps are still listed so you see which one to work on
		score = overall;
		whys.insert(whys.begin(), { 0, std::format("Overall: {} ({})", overall, why) });
	}

	// How close you stayed to the model's path (info only, Oct 26): for each moment, the nearest point of the model's
	// path in this stage, so a slightly faster or slower attempt on the same line still reads as close
	if (compareToModel && !(st.first == 3 && st.last == 3))
	{
		const int mFrom = startTick, mTo = std::min(L.StageWatchEnd(stage), static_cast<int>(L.model.size()) - 1);
		const int to = std::min(end, startTick + (mTo - mFrom) + 60);
		float sum = 0; int cnt = 0;
		for (int t = startTick; t <= to; t += 3)
		{
			const Frame& a = full[t];
			float best = 1e9f;
			for (int m = mFrom; m <= mTo; m += 3)
				best = std::min(best, Len(a.carX - L.model[m].carX, a.carY - L.model[m].carY, a.carZ - L.model[m].carZ));
			sum += best; cnt++;
		}
		const float dev = cnt ? sum / cnt : 0.0f;
		out.lineMatch = dev;
		whys.push_back({ 6, std::format("Line match: {:.0f} from the model's path on average{}", dev,
			dev <= 150.0f ? " (right on it)" : dev <= 400.0f ? " (close)" : " (your own line)") });
	}

	std::stable_sort(whys.begin(), whys.end(), [](const Why& a, const Why& b) { return a.score < b.score; });
	for (const Why& w : whys) out.lines.push_back(w.text);
	out.score = std::max(1, score);
	return out;
}

// ============================================================
// Double tap (Oct 28): off the wall, the setup touch into the backboard, then the read and the second touch
// ============================================================

namespace
{
	struct DtEvents
	{
		int pop = -1, leave = -1, setup = -1, board = -1, boardSurface = 0, second = -1;
		float popZ = 0;
		bool popOnWall = false, floorBeforeBoard = false;
	};
	DtEvents FindDt(const std::vector<Frame>& fr, int to)
	{
		DtEvents e;
		const int n = static_cast<int>(fr.size());
		to = std::min(to, n - 2);
		for (int t = 1; t <= to; t++)
		{
			if (e.pop < 0)
			{
				if (TouchAt(fr, t) && fr[t].ballZ > 250.0f && Surface(fr[t - 1]))
				{
					e.pop = t; e.popZ = fr[t].ballZ;
					e.popOnWall = fr[t - 1].onWall || fr[t - 1].carZ > 120.0f;
				}
				continue;
			}
			if (e.leave < 0) { if (!Surface(fr[t])) e.leave = t; continue; }
			if (e.setup < 0) { if (TouchAt(fr, t) && !Surface(fr[t])) e.setup = t; continue; }
			if (e.board < 0)
			{
				if (fr[t].ballZ < 110.0f) e.floorBeforeBoard = true;
				if (t > e.setup + 4 && ShotGeometry::IsWallTap(fr[t - 1], fr[t + 1])) { e.board = t; e.boardSurface = ShotGeometry::WallTapSurface(fr[t - 1], fr[t + 1]); }
				continue;
			}
			if (e.second < 0 && TouchAt(fr, t)) e.second = t;
		}
		return e;
	}

	// Where it hit the backboard: high, not past the middle toward the far side, not outside the goal (the review's
	// setup grade, Oct 24)
	int BoardLevel(const std::vector<Frame>& fr, const DtEvents& e, std::string& why)
	{
		const Frame& b = fr[e.board];
		const float z = b.ballZ, ax = std::fabs(b.ballX);
		const float carX = fr[e.setup].carX;
		const bool fromSide = std::fabs(carX) >= 800.0f;
		const float nearX = fromSide ? b.ballX * (carX > 0.0f ? 1.0f : -1.0f) : ax;
		if (e.boardSurface == 2 || e.boardSurface == 3) { why = std::format("Sent it off the {} ({:.0f} high): aim for the backboard", e.boardSurface == 2 ? "ceiling" : "top of the wall", z); return 3; }
		const std::string where = std::format("Hit the backboard {:.0f} high, {:.0f} {}", z, ax, !fromSide || ax < 150.0f ? "from the middle" : nearX >= 0.0f ? "toward your side" : "toward the far side");
		if (ax > 1800.0f) { why = where + " (too wide: outside the goal)"; return 2; }
		if (fromSide && nearX < -1200.0f) { why = where + " (crossed way over to the far side)"; return 2; }
		if (z >= 500.0f && z <= 1300.0f && ax <= 1600.0f && nearX >= -600.0f) { why = where; return 5; }
		if (z >= 300.0f && z <= 1700.0f)
		{
			why = where + (fromSide && nearX < -600.0f ? " (past the middle, toward the far side)" : z < 500.0f ? " (low on the board)" : z > 1300.0f ? " (high on the board)" : "");
			return 4;
		}
		why = where + (z < 300.0f ? " (too low)" : " (too high)");
		return 3;
	}

	float LineMatch(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, int end)
	{
		const int mFrom = startTick, mTo = std::min(L.StageWatchEnd(stage), static_cast<int>(L.model.size()) - 1);
		const int to = std::min(end, startTick + (mTo - mFrom) + 60);
		float sum = 0; int cnt = 0;
		for (int t = startTick; t <= to; t += 3)
		{
			const Frame& a = full[t];
			float best = 1e9f;
			for (int m = mFrom; m <= mTo; m += 3)
				best = std::min(best, Len(a.carX - L.model[m].carX, a.carY - L.model[m].carY, a.carZ - L.model[m].carZ));
			sum += best; cnt++;
		}
		return cnt ? sum / cnt : 0.0f;
	}
}

Lesson Lessons::DoubleTap(const std::vector<Frame>& model)
{
	Lesson L;
	L.kind = LessonKind::DoubleTap;
	L.id = "double_tap";
	L.name = "Double tap";
	L.model = model;
	if (model.size() < 60) return Lesson{};
	L.modelShot = ShotGeometry::Analyze(model);
	const ShotAnalysis& s = L.modelShot;
	DtEvents e = FindDt(model, s.touchTick);
	if (!s.valid || s.type != ShotType::DoubleTap || e.pop < 0 || e.leave < 0 || e.setup < 0 || e.board < 0) return Lesson{};
	L.popTick = e.pop;
	L.leaveTick = e.leave;
	L.setupTick = e.setup;
	L.boardTick = e.board;
	L.shotTick = s.touchTick;

	LessonStep wall;
	wall.name = "Off the wall";
	wall.what = "Follow the ball to the wall, pop it up off the wall, and jump straight after it.";
	wall.why = "A quick, clean pop gets you in the air with the ball out in front of you. That head start is what makes the double tap fast enough to beat a defender.";
	wall.tips = { "Head for the wall on the ball's line and arrive right behind it.",
		"Pop it up and out off the wall, not along it.",
		std::format("Jump right after the pop (the model leaves the wall {:.2f} s later).", (e.leave - e.pop) / 120.0f) };
	wall.cue = "Pop it, then get off the wall fast.";
	wall.start = std::max(1, s.startTick);

	LessonStep setup;
	setup.name = "The setup touch";
	setup.what = "Fly to the ball and send it high into the backboard, a little toward your side.";
	setup.why = "Where it hits the backboard decides the rebound. High and toward your side gives you the widest angle for the second touch; a hard hit dead center bounces out too far to follow.";
	setup.tips = { std::format("Aim high on the backboard, toward the side you came from (the model hits it {:.0f} high).", model[e.board].ballZ),
		"Don't hit it as hard as you can: you want a rebound you can follow.",
		"Keep boosting after the touch so you stay with it." };
	setup.cue = "High on the board, toward your side.";
	setup.start = std::max(1, e.leave + 8);

	LessonStep read;
	read.name = "Read it and finish";
	read.what = "Follow it in, read the bounce off the backboard, and put the second touch in the net.";
	read.why = "Defenders go for the backboard. Being there before it comes off the wall is what turns it into a goal.";
	read.tips = { "Stay under it, or a little behind where it'll come off.",
		"Meet it once it's come off the wall a bit, not pinched against it.",
		"Place it: a high corner or the far post beats hitting it as hard as you can." };
	read.cue = "Be there before it comes off the wall.";
	read.start = std::max(1, e.setup + 15);

	L.steps = { wall, setup, read };
	L.stages = {
		{ 0, 0, "Step 1: Off the wall" },
		{ 1, 1, "Step 2: The setup touch" },
		{ 0, 1, "Steps 1-2 together" },
		{ 2, 2, "Step 3: Read it and finish" },
		{ 0, 2, "The whole double tap", true },
	};
	return L;
}

static int DtWatchEnd(const Lesson& L, int stage)
{
	switch (L.stages[stage].last)
	{
	case 0: return L.leaveTick + 30;
	case 1: return L.boardTick + 30;
	default: return std::min(static_cast<int>(L.model.size()) - 1, L.shotTick + 90);
	}
}

static bool DtStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	if (L.StageHasFinish(stage)) return false;
	const int t = static_cast<int>(full.size()) - 1;
	if (t <= startTick + 5) return false;
	DtEvents e = FindDt(full, t);
	const int last = L.stages[stage].last;
	if (last == 0)
		return (e.leave >= 0 && t >= e.leave + 20) || t > L.leaveTick + 150;
	// The setup touch: over once it's hit the backboard (or can't any more)
	if (e.board >= 0 && t >= e.board + 10) return true;
	if (e.setup >= 0 && full[t].ballZ < 110.0f) return true;
	if (e.setup < 0 && t > L.setupTick + 150) return true;
	return t > L.boardTick + 150;
}

static StageScore DtScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst,
	const ShotReference& ref, bool compareToModel)
{
	StageScore out;
	const LessonStage& st = L.stages[stage];
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const int end = n - 1;
	DtEvents e = FindDt(full, end);
	ShotAnalysis shot = ShotGeometry::Analyze(full);
	struct Why { int score; std::string text; };
	std::vector<Why> whys;

	for (int step = st.first; step <= st.last; step++)
	{
		int sc = 5;
		std::string why;
		switch (step)
		{
		case 0:
		{
			if (e.pop < 0) { sc = 1; why = "Never popped it off the wall"; break; }
			if (!e.popOnWall) { sc = 2; why = "Popped it off the ground, not the wall"; break; }
			if (e.leave < 0) { sc = 1; why = "Never left the wall"; break; }
			const float j = (e.leave - e.pop) / 120.0f;
			sc = j <= 0.25f ? 5 : j <= 0.35f ? 4 : 2;   // wheels leave the wall a few ticks after the jump press (the model: 0.22 s)
			why = std::format("Popped it {:.0f} high, left the wall {:.2f} s later", e.popZ, j);
			if (sc == 2) why += " (jump right after the pop)";
			// Right behind it as it started up the wall
			int k = -1;
			for (int t = std::max(1, startTick); t < e.pop; t++) if (full[t].ballZ > 300.0f) { k = t; break; }
			if (k >= 0)
			{
				const float d = CarBall(full[k]);
				if (d > 550.0f && sc > 3) { sc = 3; why += std::format(", {:.0f} behind it as it went up the wall (catch up before it climbs)", d); }
				else if (d > 450.0f && sc > 4) { sc = 4; why += std::format(", {:.0f} behind it as it went up the wall", d); }
			}
			break;
		}
		case 1:
		{
			if (e.setup < 0) { sc = 1; why = "Never touched it in the air"; break; }
			if (e.board < 0) { sc = e.floorBeforeBoard ? 1 : 2; why = "The setup touch didn't reach the backboard"; break; }
			sc = BoardLevel(full, e, why);
			break;
		}
		case 2:
		{
			const bool dt = shot.valid && shot.type == ShotType::DoubleTap;
			if (!dt)
			{
				sc = wentIn ? 2 : 1;
				why = wentIn ? "Scored, but not off the backboard (no double tap)" : "No second touch off the backboard";
				break;
			}
			const float readSec = (shot.touchTick - shot.backboardTick) / 120.0f;
			int rl = readSec <= 0.50f ? 5 : readSec <= 0.80f ? 4 : readSec <= 1.10f ? 3 : 2;
			std::string rw = std::format("hit it {:.2f} s after it came off the wall", readSec);
			if (shot.reboundDist < 150.0f && rl > 3) { rl = 3; rw += " (pinched it on the wall: let it come off a bit)"; }
			int res;
			std::string resw;
			if (wentIn && shot.reachesGoalLine && shot.crossZ >= 360.0f && !floorFirst) { res = 5; resw = std::format("Goal, high in the net ({:.0f})", shot.crossZ); }
			else if (wentIn) { res = 3; resw = floorFirst ? "Goal, but it bounced in" : std::format("Goal, low in the net ({:.0f})", shot.crossZ); }
			else if (shot.reachesGoalLine && shot.missDist > 0.0f && shot.missDist <= 245.0f) { res = 2; resw = std::format("Just missed ({} by {:.0f})", shot.missDir, shot.missDist); }
			else { res = 1; resw = shot.reachesGoalLine && shot.missDist > 0.0f ? std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist) : "Didn't go in"; }
			sc = std::min(rl, res);
			why = resw + ", " + rw;
			break;
		}
		}
		out.stepScores.push_back(sc);
		whys.push_back({ sc, std::format("{}: {}", L.steps[step].name, why) });
	}

	int score = 5;
	for (int s : out.stepScores) score = std::min(score, s);
	if (st.whole)
	{
		RepSegments segs = Segmenter::Split(full, end);
		RepGrade g = Grader::GradeRep(full, segs, shot, wentIn, floorFirst, ref);
		int overall = g.overall;
		std::string why = g.overallWhy;
		if (shot.valid && shot.type != ShotType::DoubleTap) { overall = std::min(overall, wentIn ? 2 : 1); why = "Not a double tap: " + why; }
		score = overall;
		whys.insert(whys.begin(), { 0, std::format("Overall: {} ({})", overall, why) });
	}
	if (compareToModel && st.first != st.last + 1)
	{
		const float dev = LineMatch(L, stage, full, startTick, end);
		out.lineMatch = dev;
		whys.push_back({ 6, std::format("Line match: {:.0f} from the model's path on average{}", dev,
			dev <= 150.0f ? " (right on it)" : dev <= 400.0f ? " (close)" : " (your own line)") });
	}
	std::stable_sort(whys.begin(), whys.end(), [](const Why& a, const Why& b) { return a.score < b.score; });
	for (const Why& w : whys) out.lines.push_back(w.text);
	out.score = std::max(1, score);
	return out;
}

// ============================================================
// Ceiling shot (Oct 28): up the wall and onto the ceiling, the drop, the flip into the shot
// ============================================================

namespace
{
	bool OnCeiling(const Frame& f) { return f.onGround && f.carZ > 1900.0f; }

	struct CsEvents
	{
		int wall = -1, land = -1, leave = -1, flip = -1, hit = -1;
		bool wallFromWall = false, jumpedOff = false, ballOnCeiling = false;
		float wallSpeed = 0, predApex = 0;
	};
	CsEvents FindCs(const std::vector<Frame>& fr, int to)
	{
		CsEvents e;
		const int n = static_cast<int>(fr.size());
		to = std::min(to, n - 1);
		for (int t = 1; t <= to; t++)
		{
			if (e.wall < 0)
			{
				if (TouchAt(fr, t) && fr[t].ballZ > 200.0f && Surface(fr[t - 1]))
				{
					e.wall = t;
					const Frame& w = fr[t - 1];
					e.wallFromWall = w.onWall || w.carZ > 100.0f;
					e.wallSpeed = Len(w.carVX, w.carVY, w.carVZ);
					const Frame& a = fr[std::min(n - 1, t + 8)];
					e.predApex = a.ballZ + (a.ballVZ > 0.0f ? a.ballVZ * a.ballVZ / (2.0f * 650.0f) : 0.0f);
				}
				continue;
			}
			if (fr[t].ballZ > 1945.0f && e.hit < 0) e.ballOnCeiling = true;
			if (e.land < 0) { if (OnCeiling(fr[t])) e.land = t; continue; }
			if (e.leave < 0)
			{
				if (fr[t].jump && !fr[t - 1].jump) e.jumpedOff = true;
				if (!OnCeiling(fr[t])) e.leave = t;
				continue;
			}
			if (e.flip < 0 && fr[t].jump && !fr[t - 1].jump && !Surface(fr[t]) && !(e.jumpedOff && t <= e.leave + 6)) e.flip = t;
			if (e.hit < 0 && TouchAt(fr, t)) e.hit = t;
		}
		return e;
	}
}

Lesson Lessons::CeilingShot(const std::vector<Frame>& model)
{
	Lesson L;
	L.kind = LessonKind::CeilingShot;
	L.id = "ceiling_shot";
	L.name = "Ceiling shot";
	L.model = model;
	if (model.size() < 60) return Lesson{};
	L.modelShot = ShotGeometry::Analyze(model);
	const ShotAnalysis& s = L.modelShot;
	CsEvents e = FindCs(model, s.touchTick);
	if (!s.valid || s.type != ShotType::CeilingShot || e.wall < 0 || e.land < 0 || e.leave < 0 || e.flip < 0) return Lesson{};
	L.popTick = e.wall;
	L.landTick = e.land;
	L.leaveTick = e.leave;
	L.flipTick = e.flip;
	L.shotTick = s.touchTick;

	LessonStep up;
	up.name = "Up the wall and onto the ceiling";
	up.what = "Hit the ball up the side wall toward the middle, then follow it up onto the ceiling.";
	up.why = "This touch decides where the shot goes: the flip later only adjusts it a little. It should peak just under the ceiling; if it hits the ceiling it drops back before you get there.";
	up.tips = { std::format("Come in fast (the model hits it at about {:.0f}).", e.wallSpeed),
		std::format("Send it up and out so it peaks just under the ceiling (the model's peaks {:.0f} high).", s.ballApexZ),
		"Jump off the wall and land on the ceiling, or drive up onto it." };
	up.cue = "Hard and high, but not into the ceiling.";
	up.start = std::max(1, s.startTick + 1);

	LessonStep drop;
	drop.name = "The drop";
	drop.what = "Fall off the ceiling without jumping, line up with the ball on the way down, and get right to it.";
	drop.why = "Falling off keeps your flip for as long as you need it. Jumping off burns it, and you have to rush the shot.";
	drop.tips = { "Let go: don't press jump to leave the ceiling.",
		"Nose down and boost down toward the ball; don't air roll upright.",
		"Line up behind the ball's path while you're still on the ceiling." };
	drop.cue = "Fall, don't jump.";
	drop.start = std::max(1, e.land + 6);

	LessonStep flip;
	flip.name = "Flip into the shot";
	flip.what = "Flip into the ball for the shot: hard, to a corner.";
	flip.why = "The stored flip is the whole point of a ceiling shot: a powerful flip exactly when and where you choose.";
	flip.tips = { "Wait until you're right at the ball, then flip.",
		"Flip with the ball's direction, not across it.",
		"Near the net, a side flip to the far post beats a front flip into the crossbar." };
	flip.cue = "Get right to it, then flip.";
	flip.start = std::max(1, e.flip - 40);

	L.steps = { up, drop, flip };
	L.stages = {
		{ 0, 0, "Step 1: Up the wall and onto the ceiling" },
		{ 1, 1, "Step 2: The drop" },
		{ 0, 1, "Steps 1-2 together" },
		{ 2, 2, "Step 3: Flip into the shot" },
		{ 0, 2, "The whole ceiling shot", true },
	};
	return L;
}

static int CsWatchEnd(const Lesson& L, int stage)
{
	switch (L.stages[stage].last)
	{
	case 0: return L.landTick + 30;
	case 1: return L.flipTick + 10;
	default: return std::min(static_cast<int>(L.model.size()) - 1, L.shotTick + 90);
	}
}

static bool CsStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	if (L.StageHasFinish(stage)) return false;
	const int t = static_cast<int>(full.size()) - 1;
	if (t <= startTick + 5) return false;
	CsEvents e = FindCs(full, t);
	if (L.stages[stage].last == 0)
		return (e.land >= 0 && t >= e.land + 20) || t > L.landTick + 180;
	if (e.flip >= 0 && t >= e.flip + 5) return true;
	if (e.leave >= 0 && (Surface(full[t]) || full[t].ballZ < 110.0f)) return true;   // landed, or the ball's down
	return t > L.flipTick + 150;
}

static StageScore CsScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst,
	const ShotReference& ref, bool compareToModel)
{
	StageScore out;
	const LessonStage& st = L.stages[stage];
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const int end = n - 1;
	CsEvents e = FindCs(full, end);
	ShotAnalysis shot = ShotGeometry::Analyze(full);
	struct Why { int score; std::string text; };
	std::vector<Why> whys;

	for (int step = st.first; step <= st.last; step++)
	{
		int sc = 5;
		std::string why;
		switch (step)
		{
		case 0:
		{
			if (e.wall < 0) { sc = 1; why = "Never hit it up the wall"; break; }
			if (!e.wallFromWall) { sc = 2; why = "Hit it from the ground, not the wall"; break; }
			sc = e.wallSpeed >= 1400.0f ? 5 : e.wallSpeed >= 900.0f ? 4 : 3;
			why = std::format("Hit it up the wall at {:.0f}", e.wallSpeed);
			if (sc <= 3) why += " (come in faster)";
			if (e.predApex > 1945.0f && sc > 3) { sc = 3; why += ", but it's going into the ceiling (a bit less height)"; }
			else if (e.predApex < 1400.0f && sc > 3) { sc = 3; why += std::format(", peaking only {:.0f} high (hit it harder up the wall)", e.predApex); }
			else if (e.predApex < 1700.0f && sc > 4) { sc = 4; why += std::format(", peaking {:.0f} high", e.predApex); }
			if (e.land < 0) { sc = std::min(sc, 2); why += ", and never got onto the ceiling"; }
			break;
		}
		case 1:
		{
			if (e.land < 0) { sc = 1; why = "Never got onto the ceiling"; break; }
			if (e.leave < 0) { sc = 1; why = "Never left the ceiling"; break; }
			// How close you got to it: where you flipped, or your closest pass if you didn't
			float d = 1e9f;
			if (e.flip >= 0) d = CarBall(full[e.flip]);
			else for (int t = e.leave; t <= end; t++) d = std::min(d, CarBall(full[t]));
			sc = d <= 300.0f ? 5 : d <= 450.0f ? 4 : d <= 650.0f ? 3 : 2;
			why = e.flip >= 0 ? std::format("Flipped {:.0f} from the ball", d) : std::format("Got within {:.0f} of it (no flip)", d);
			if (sc <= 3) why += " (line up with it on the ceiling and get right to it)";
			if (e.jumpedOff)
			{
				const int cap = e.ballOnCeiling ? 4 : 3;
				if (sc > cap) sc = cap;
				why = std::string(e.ballOnCeiling ? "Jumped down off the ceiling (fine: the ball was coming off it), " : "Jumped off the ceiling (fall off to keep your flip), ") + why;
			}
			else why = "Fell off the ceiling, " + why;
			break;
		}
		case 2:
		{
			const bool flipShot = e.flip >= 0 && e.hit >= 0 && e.hit - e.flip >= 0 && e.hit - e.flip <= 40;
			int res;
			std::string resw;
			const bool high = shot.reachesGoalLine && shot.crossZ >= 360.0f;
			if (wentIn && high && std::fabs(shot.crossX) >= 580.0f && shot.shotSpeed >= 2000.0f && !floorFirst) { res = 5; resw = std::format("Top corner at {:.0f}", shot.shotSpeed); }
			else if (wentIn && shot.reachesGoalLine && shot.crossZ >= 300.0f && !floorFirst) { res = 4; resw = std::format("Goal, {:.0f} high", shot.crossZ); }
			else if (wentIn) { res = 3; resw = floorFirst ? "Goal, but it bounced in" : std::format("Goal, low in the net ({:.0f})", shot.crossZ); }
			else if (shot.reachesGoalLine && shot.missDist > 0.0f && shot.missDist <= 250.0f) { res = 2; resw = std::format("Just missed ({} by {:.0f})", shot.missDir, shot.missDist); }
			else { res = 1; resw = shot.reachesGoalLine && shot.missDist > 0.0f ? std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist) : "Didn't go in"; }
			sc = res;
			why = resw;
			if (!flipShot && sc > 3) { sc = 3; why += ", but not off the flip"; }
			else if (flipShot) why += ", off the flip";
			break;
		}
		}
		out.stepScores.push_back(sc);
		whys.push_back({ sc, std::format("{}: {}", L.steps[step].name, why) });
	}

	int score = 5;
	for (int s : out.stepScores) score = std::min(score, s);
	if (st.whole)
	{
		RepSegments segs = Segmenter::Split(full, end);
		RepGrade g = Grader::GradeRep(full, segs, shot, wentIn, floorFirst, ref);
		int overall = g.overall;
		std::string why = g.overallWhy;
		if (shot.valid && shot.type != ShotType::CeilingShot) { overall = std::min(overall, wentIn ? 2 : 1); why = "Not a ceiling shot: " + why; }
		score = overall;
		whys.insert(whys.begin(), { 0, std::format("Overall: {} ({})", overall, why) });
	}
	if (compareToModel)
	{
		const float dev = LineMatch(L, stage, full, startTick, end);
		out.lineMatch = dev;
		whys.push_back({ 6, std::format("Line match: {:.0f} from the model's path on average{}", dev,
			dev <= 150.0f ? " (right on it)" : dev <= 400.0f ? " (close)" : " (your own line)") });
	}
	std::stable_sort(whys.begin(), whys.end(), [](const Why& a, const Why& b) { return a.score < b.score; });
	for (const Why& w : whys) out.lines.push_back(w.text);
	out.score = std::max(1, score);
	return out;
}

// ============================================================
// Flip reset (Oct 28): pop and get up, get the reset, use the flip
// ============================================================

namespace
{
	struct FrEvents
	{
		Events ad;          // the pop, leaving the wall, the first air touch
		int reset = -1, dodge = -1, hit = -1;
		float closing = 0, keptAfter = 0;
	};
	FrEvents FindFr(const std::vector<Frame>& fr, int to)
	{
		FrEvents e;
		const int n = static_cast<int>(fr.size());
		to = std::min(to, n - 1);
		e.ad = FindAd(fr, 1, to);
		if (e.ad.leave < 0) return e;
		for (int t = e.ad.leave + 1; t <= to; t++)
			if (ShotGeometry::IsFlipReset(fr[t - 1], fr[t])) { e.reset = t; break; }
		if (e.reset < 0) return e;
		const Frame& a = fr[std::max(0, e.reset - 3)];
		const float dx = a.ballX - a.carX, dy = a.ballY - a.carY, dz = a.ballZ - a.carZ, d = std::max(1.0f, Len(dx, dy, dz));
		e.closing = ((a.carVX - a.ballVX) * dx + (a.carVY - a.ballVY) * dy + (a.carVZ - a.ballVZ) * dz) / d;
		for (int t = e.reset; t <= std::min(to, e.reset + 48); t++) e.keptAfter = std::max(e.keptAfter, CarBall(fr[t]));
		for (int t = e.reset + 1; t <= to; t++)
		{
			if (e.dodge < 0 && fr[t].jump && !fr[t - 1].jump && !Surface(fr[t])) e.dodge = t;
			if (e.hit < 0 && TouchAt(fr, t) && t > e.reset + 4) e.hit = t;
		}
		return e;
	}
}

Lesson Lessons::FlipReset(const std::vector<Frame>& model)
{
	Lesson L;
	L.kind = LessonKind::FlipReset;
	L.id = "flip_reset";
	L.name = "Flip reset";
	L.model = model;
	if (model.size() < 60) return Lesson{};
	L.modelShot = ShotGeometry::Analyze(model);
	const ShotAnalysis& s = L.modelShot;
	FrEvents e = FindFr(model, s.touchTick);
	if (!s.valid || s.type != ShotType::FlipReset || e.ad.pop < 0 || e.ad.firstAir < 0 || e.reset < 0) return Lesson{};
	L.popTick = e.ad.pop;
	L.firstAirTick = e.ad.firstAir;
	L.resetTick = e.reset;
	L.flipTick = e.dodge;
	L.shotTick = s.touchTick;

	LessonStep pop;
	pop.name = "Pop and get up";
	pop.what = "Pop the ball off the wall, jump straight after it, and get your first touch in the air from underneath.";
	pop.why = "The reset needs the ball above you and close. A clean pop and a quick jump put you there.";
	pop.tips = { std::format("Pop it right after it rolls up the curve (the model pops it {:.0f} high).", e.ad.popZ),
		std::format("Jump right after the touch (the model leaves the wall {:.2f} s later).", (e.ad.leave - e.ad.pop) / 120.0f),
		"Touch it from underneath so it stays above you." };
	pop.cue = "Pop, jump, get under it.";
	pop.start = std::max(1, s.startTick + 1);

	LessonStep reset;
	reset.name = "Get the reset";
	reset.what = "Fly into the ball and touch it with all four wheels to get your flip back.";
	reset.why = "The reset is a soft, controlled touch. Fly into it rather than letting it land on you, or you'll knock it away or miss with a wheel.";
	reset.tips = { "Get under it and turn your wheels toward the ball.",
		std::format("Move into the ball as you touch it (the model is moving into it at about {:.0f}).", e.closing),
		"Touch it softly so it stays with you." };
	reset.cue = "Wheels to the ball, softly.";
	reset.start = std::max(1, e.ad.firstAir + 10);

	LessonStep use;
	use.name = "Use the flip";
	use.what = "Stay with the ball after the reset, then flip into it for the shot.";
	use.why = "Holding the flip until the right moment is what makes a reset hard to read. Let the ball get away and the flip's wasted.";
	use.tips = { "Stay with the ball after the reset; don't fly past it.",
		std::format("Flip into it when it's right in front of you (the model held the flip {:.2f} s).", e.dodge >= 0 ? (e.dodge - e.reset) / 120.0f : 0.0f),
		"Aim high: a corner beats a flat hit." };
	use.cue = "Stay with it, then flip through it.";
	use.start = std::max(1, e.reset + 15);

	L.steps = { pop, reset, use };
	L.stages = {
		{ 0, 0, "Step 1: Pop and get up" },
		{ 1, 1, "Step 2: Get the reset" },
		{ 0, 1, "Steps 1-2 together" },
		{ 2, 2, "Step 3: Use the flip" },
		{ 0, 2, "The whole flip reset", true },
	};
	return L;
}

static int FrWatchEnd(const Lesson& L, int stage)
{
	switch (L.stages[stage].last)
	{
	case 0: return L.firstAirTick + 30;
	case 1: return L.resetTick + 30;
	default: return std::min(static_cast<int>(L.model.size()) - 1, L.shotTick + 90);
	}
}

static bool FrStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	if (L.StageHasFinish(stage)) return false;
	const int t = static_cast<int>(full.size()) - 1;
	if (t <= startTick + 5) return false;
	FrEvents e = FindFr(full, t);
	const Frame& f = full[t];
	if (e.ad.pop >= 0 && t > e.ad.pop + 10 && f.ballZ < 110.0f) return true;
	if (L.stages[stage].last == 0)
		return (e.ad.firstAir >= 0 && t >= e.ad.firstAir + 30) || (e.ad.leave >= 0 && t > e.ad.leave + 200) || t > L.firstAirTick + 180;
	if (e.reset >= 0 && t >= e.reset + 30) return true;
	if (e.ad.firstAir >= 0 && CarBall(f) > 900.0f) return true;
	return t > L.resetTick + 180;
}

static StageScore FrScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst,
	const ShotReference& ref, bool compareToModel)
{
	StageScore out;
	const LessonStage& st = L.stages[stage];
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const int end = n - 1;
	FrEvents e = FindFr(full, end);
	ShotAnalysis shot = ShotGeometry::Analyze(full);
	struct Why { int score; std::string text; };
	std::vector<Why> whys;

	for (int step = st.first; step <= st.last; step++)
	{
		int sc = 5;
		std::string why;
		switch (step)
		{
		case 0:
		{
			sc = PopLevel(e.ad, why);
			if (e.ad.pop < 0 || e.ad.leave < 0) { sc = 1; if (e.ad.pop >= 0) why = "Never left the wall"; break; }
			const float j = (e.ad.leave - e.ad.pop) / 120.0f;
			const int jl = j <= 0.15f ? 5 : j <= 0.25f ? 4 : 2;
			why += std::format(", left the wall {:.2f} s later", j);
			if (jl < sc) { sc = jl; if (jl <= 2) why += " (jump right after the pop)"; }
			if (e.ad.firstAir < 0) { sc = std::min(sc, 2); why += ", but never got a touch in the air"; }
			else if (full[e.ad.firstAir].touchZ >= full[e.ad.firstAir].ballZ - 20.0f && sc > 4) { sc = 4; why += ", first touch wasn't from under the ball"; }
			break;
		}
		case 1:
		{
			if (e.reset < 0) { sc = 1; why = "No reset (all four wheels on the ball)"; break; }
			sc = e.closing >= 150.0f ? 5 : e.closing >= 0.0f ? 4 : 3;
			why = std::format("Got the reset, moving into the ball at {:.0f}", std::max(0.0f, e.closing));
			if (e.closing < 0.0f) why += " (it fell onto you: fly into it)";
			else if (sc == 4) why += " (barely: fly into it more)";
			if (e.keptAfter > 400.0f && sc > 4) { sc = 4; why += std::format(", then it got {:.0f} away (softer touch)", e.keptAfter); }
			break;
		}
		case 2:
		{
			if (e.reset < 0) { sc = 1; why = "No reset to use"; break; }
			const bool flipShot = e.dodge >= 0 && shot.valid && shot.touchTick - e.dodge >= 0 && shot.touchTick - e.dodge <= 40;
			int res;
			std::string resw;
			if (wentIn && shot.reachesGoalLine && shot.crossZ >= 450.0f && !floorFirst) { res = 5; resw = std::format("Goal, high in the net ({:.0f})", shot.crossZ); }
			else if (wentIn && !floorFirst) { res = 4; resw = std::format("Goal ({:.0f} high)", shot.crossZ); }
			else if (wentIn) { res = 3; resw = "Goal, but it bounced in"; }
			else if (shot.reachesGoalLine && shot.missDist > 0.0f && shot.missDist <= 250.0f) { res = 2; resw = std::format("Just missed ({} by {:.0f})", shot.missDir, shot.missDist); }
			else { res = 1; resw = shot.reachesGoalLine && shot.missDist > 0.0f ? std::format("Missed {} by {:.0f}", shot.missDir, shot.missDist) : "Didn't go in"; }
			sc = res;
			why = resw;
			if (e.dodge < 0 && sc > 2) { sc = 2; why += ", but you never used the flip"; }
			else if (!flipShot && sc > 3) { sc = 3; why += ", but the flip wasn't into the shot"; }
			else if (flipShot) why += std::format(", off the flip ({:.2f} s after the reset)", (e.dodge - e.reset) / 120.0f);
			float kept = 0;
			for (int t = e.reset; t < (shot.valid ? shot.touchTick : end) && t <= end; t++) kept = std::max(kept, CarBall(full[t]));
			if (kept > 700.0f && sc > 3) { sc = 3; why += std::format(", and the ball got {:.0f} away first", kept); }
			break;
		}
		}
		out.stepScores.push_back(sc);
		whys.push_back({ sc, std::format("{}: {}", L.steps[step].name, why) });
	}

	int score = 5;
	for (int s : out.stepScores) score = std::min(score, s);
	if (st.whole)
	{
		RepSegments segs = Segmenter::Split(full, end);
		RepGrade g = Grader::GradeRep(full, segs, shot, wentIn, floorFirst, ref);
		int overall = g.overall;
		std::string why = g.overallWhy;
		if (shot.valid && shot.type != ShotType::FlipReset) { overall = std::min(overall, wentIn ? 2 : 1); why = "Not a flip reset: " + why; }
		score = overall;
		whys.insert(whys.begin(), { 0, std::format("Overall: {} ({})", overall, why) });
	}
	if (compareToModel)
	{
		const float dev = LineMatch(L, stage, full, startTick, end);
		out.lineMatch = dev;
		whys.push_back({ 6, std::format("Line match: {:.0f} from the model's path on average{}", dev,
			dev <= 150.0f ? " (right on it)" : dev <= 400.0f ? " (close)" : " (your own line)") });
	}
	std::stable_sort(whys.begin(), whys.end(), [](const Why& a, const Why& b) { return a.score < b.score; });
	for (const Why& w : whys) out.lines.push_back(w.text);
	out.score = std::max(1, score);
	return out;
}

// ============================================================
// Dispatch by lesson kind, and the skill tree
// ============================================================

// ============================================================
// Kickoffs (Nov 1). The model rep is made in RocketSim (tools/combine/kickoff_model.cpp): the fastest clean kickoff
// from each spawn with the inputs good players use (an SSL speedflip's jump and dodge timing), checked against 2,600+
// replay kickoffs. Nov 2 (Coop: the model looked like a side flip): it now cancels the way pros do, the nose dipping
// 16-18 degrees before the cancel catches it (pros: 17-24) and wheels down about 1.08 s after the dodge (pros: 1.09);
// the diagonal speedflip gets there in 1.86 s (the SSL median is 1.92, the fastest pros about 1.87), off-centre 2.12
// (2.17), back centre 2.44 (2.48). The right spawn's kickoff comes first (x < 0 on blue is the right side from the driver's seat;
// the labels were swapped until Coop caught it, Nov 2), then the same one mirrored from the left spawn.
// Steps: boost and line up (to the jump), the speedflip (jump, diagonal dodge, cancel, wheels down), the flip into the
// ball; then the whole kickoff from each side, scored by time to the ball against stars from the replays.
// ============================================================

namespace
{
	int RisingJump(const std::vector<Frame>& fr, int from, int to)
	{
		for (int t = std::max(1, from); t <= to && t < static_cast<int>(fr.size()); t++)
			if (fr[t].jump && !fr[t - 1].jump) return t;
		return -1;
	}
	float Speed(const Frame& f) { return Len(f.carVX, f.carVY, f.carVZ); }
	float Deg(int unreal) { return unreal * (360.0f / 65536.0f); }
	float HeadingDeg(const Frame& f) { float d = Deg(f.carYaw); while (d > 180) d -= 360; while (d < -180) d += 360; return d; }
	float AngleDiff(float a, float b) { float d = a - b; while (d > 180) d -= 360; while (d < -180) d += 360; return std::fabs(d); }
	bool StickDiagonal(const Frame& f) { return f.pitch <= -0.3f && std::max(std::fabs(f.yaw), std::fabs(f.steer)) >= 0.4f; }

	// What happened in one kickoff (live or the model), found by the inputs and the car: the first jump, the dodge after
	// it, landing, the flip into the ball, the hit and the arrival
	KickoffSide FindKo(const std::vector<Frame>& fr, int start, int end, bool speedflip)
	{
		KickoffSide k;
		k.start = start;
		k.end = end = std::min(end, static_cast<int>(fr.size()) - 1);
		for (int t = start; t <= end; t++)
		{
			if (k.arrive < 0 && CarBall(fr[t]) < 200.0f) k.arrive = t;
			if (k.touch < 0 && t > start && fr[t].touch && !fr[t - 1].touch) k.touch = t;
		}
		const int hit = k.touch >= 0 ? k.touch : end;
		const int j = RisingJump(fr, start + 1, hit);
		// A speedflip's jump comes well before the ball; a first jump in the last half second is a plain flip-in
		if (speedflip && j >= 0 && (k.touch < 0 || j < k.touch - 60))
		{
			k.jump = j;
			const int d = RisingJump(fr, k.jump + 1, std::min(hit, k.jump + 60));
			if (d >= 0) k.dodge = d;   // (a second press soon after the jump: the car can still read as on the ground the tick it flips)
			const int from = k.dodge >= 0 ? k.dodge + 4 : k.jump + 4;
			for (int t = from; t <= hit; t++) if (fr[t].onGround) { k.land = t; break; }
			if (k.land >= 0) k.flipIn = RisingJump(fr, k.land, hit);
		}
		else k.flipIn = j;
		return k;
	}

	// Which kickoff in the model a stage belongs to (by its start tick)
	const KickoffSide& KoSideFor(const Lesson& L, int tick)
	{
		for (const KickoffSide& s : L.koSides) if (tick >= s.start && tick <= s.end) return s;
		return L.koSides.front();
	}

	int StarScore(const Lesson& L, float secs, std::string& why)
	{
		const float* st = L.koStars;
		const int stars = secs <= st[2] ? 3 : secs <= st[1] ? 2 : secs <= st[0] ? 1 : 0;
		if (stars == 3) why = std::format("{:.2f} s to the ball: three stars", secs);
		else why = std::format("{:.2f} s to the ball: {} (the next star at {:.2f} s)", secs, stars == 2 ? "two stars" : stars == 1 ? "one star" : "no stars yet",
			st[std::min(2, stars)]);
		return stars == 3 ? 5 : stars == 2 ? 4 : stars == 1 ? 3 : secs <= st[0] + 0.15f ? 2 : 1;
	}

	// The step (0 the start, 1 the speedflip, 2 the flip in) of a kickoff lesson's step index; flip-in lessons have
	// steps 0 the drive, 1 the flip in
	int KoStepKind(const Lesson& L, int step)
	{
		const int per = L.koSpeedflip ? 3 : 2;
		const int k = step % per;
		return L.koSpeedflip ? k : (k == 0 ? 0 : 2);
	}
}

Lesson Lessons::Kickoff(const std::string& id, const std::vector<Frame>& model)
{
	Lesson L;
	L.kind = LessonKind::Kickoff;
	L.id = id;
	L.model = model;
	L.koSpeedflip = id != "kickoff_flipin";
	const bool back = id == "kickoff_back";
	L.koSpawn = id == "kickoff_offcentre" ? "off-centre" : back ? "back centre" : "diagonal";
	L.name = L.koSpeedflip ? std::format("Kickoff: {} speedflip", L.koSpawn) : "Kickoff: boost and flip in";
	if (model.size() < 120) return Lesson{};
	// Stars from the openings book (ranked and pro replays, time to within 200 of the ball): one star is a typical
	// Gold-Plat kickoff, two a typical Champ one, three about the SSL median. Nov 4: re-run with 360 ranked 1v1s and
	// 400 more pro games (30,006 takers; back centre went from a thin sample to 2,137, since it's mostly a 1v1 spawn).
	// Medians, Gold-Plat / Champ / SSL: diagonal 2.11 / 1.97 / 1.93, off-centre 2.40 / 2.20 / 2.17, back centre
	// 2.60 / 2.50 / 2.48. Flip-ins: about 2.20 (slow Gold), 2.11 (the Gold-Plat median), 2.09 (Diamond-Champ, 2.05-2.10).
	L.koStarsWho = L.koSpeedflip ? "One star is a typical Gold-Plat kickoff, two a typical Champ one, three about the SSL median."
		: "Two stars is the typical Gold-Plat flip-in, three a Diamond-Champ one.";
	if (!L.koSpeedflip) { L.koStars[0] = 2.20f; L.koStars[1] = 2.11f; L.koStars[2] = 2.09f; }   // (2.09: the model gets there in 2.083)
	else if (id == "kickoff_offcentre") { L.koStars[0] = 2.40f; L.koStars[1] = 2.20f; L.koStars[2] = 2.18f; }
	else if (back) { L.koStars[0] = 2.60f; L.koStars[1] = 2.50f; L.koStars[2] = 2.49f; }
	else { L.koStars[0] = 2.11f; L.koStars[1] = 1.97f; L.koStars[2] = 1.94f; }

	// The kickoffs in the model: a new one starts where the car jumps back to a spawn
	std::vector<int> starts = { 0 };
	for (int t = 1; t < static_cast<int>(model.size()); t++)
		if (Len(model[t].carX - model[t - 1].carX, model[t].carY - model[t - 1].carY, 0) > 500.0f) starts.push_back(t);
	for (size_t i = 0; i < starts.size() && i < 2; i++)
	{
		const int end = i + 1 < starts.size() ? starts[i + 1] - 1 : static_cast<int>(model.size()) - 1;
		KickoffSide k = FindKo(model, starts[i], end, L.koSpeedflip);
		if (k.touch < 0 || k.arrive < 0 || k.flipIn < 0 || (L.koSpeedflip && (k.dodge < 0 || k.land < 0))) return Lesson{};
		L.koSides.push_back(k);
	}
	const KickoffSide& a = L.koSides.front();
	const float jumpS = (a.jump - a.start) / 120.0f, dodgeS = (a.dodge - a.start) / 120.0f, landS = (a.land - a.start) / 120.0f;
	const float arriveS = (a.arrive - a.start) / 120.0f;
	const float landSpeed = a.land >= 0 ? Speed(model[a.land]) : 0.0f;

	LessonStep lineUp;
	lineUp.name = "Boost and line up";
	lineUp.what = L.koSpeedflip ? "Boost from the very first moment, angle the car a touch off the ball, and jump." : "Boost from the very first moment and drive straight at the ball.";
	lineUp.why = L.koSpeedflip ? "Every tick without boost is speed you never get back, and the angle is what points the diagonal flip at the ball." : "A kickoff is a race: the first half second of boost is most of your speed at the ball.";
	if (L.koSpeedflip)
		lineUp.tips = { "Boost and throttle on the countdown's go, not a beat after.",
			std::format("Steer a little so the car points just off the ball (the model is {:.0f} degrees off when it jumps).", AngleDiff(HeadingDeg(model[a.jump]),
				std::atan2(model[a.jump].ballY - model[a.jump].carY, model[a.jump].ballX - model[a.jump].carX) * 57.29578f)),
			std::format("Jump at about {:.2f} s, with boost still held (good players jump at 0.37-0.47 s from {}).", jumpS, L.koSpawn == "diagonal" ? "the diagonal" : L.koSpawn) };
	else
		lineUp.tips = { "Boost and throttle on the countdown's go.", "Keep the car pointed at the ball: no swerving, it costs speed.", "Hold boost the whole way." };
	lineUp.cue = L.koSpeedflip ? "Boost on go, jump on time." : "Boost on go, straight at it.";
	lineUp.start = a.start;

	LessonStep sf;
	sf.name = "The speedflip";
	sf.what = "A quick jump, a diagonal flip, then pull the stick straight back to cancel it and hold air roll, so you land on your wheels already going fast.";
	sf.why = "The flip's push without the flip's spin: you keep boosting the whole time and land supersonic, instead of tumbling.";
	sf.tips = { std::format("Tap jump, then flip diagonally almost at once (the model flips {:.2f} s after the jump).", dodgeS - jumpS),
		"Straight away pull the stick back to stop the front flip, and hold air roll to stop the side spin. Keep boosting.",
		std::format("Land flat on your wheels (the model lands at {:.2f} s going {:.0f}).", landS, landSpeed) };
	sf.cue = "Flip, cancel, wheels down.";
	sf.start = std::max(a.start, a.jump - 8);

	LessonStep in;
	in.name = "Flip into the ball";
	in.what = "Keep boosting straight at the ball and flip into it just before you reach it.";
	in.why = "The flip is extra power at the moment that counts: it decides who wins the hit when you both arrive together.";
	in.tips = { std::format("Flip when you're about {:.0f} away (the model starts its jump {:.2f} s before it gets there).",
		CarBall(model[a.flipIn]) - 92.75f, (a.arrive - a.flipIn) / 120.0f),
		"Flip forward through the middle of the ball.",
		std::format("Get there fast: the model is at the ball in {:.2f} s.", arriveS) };
	in.cue = "Flip right into it.";
	in.start = L.koSpeedflip ? std::max(a.land + 6, a.flipIn - 40) : std::max(a.start, a.flipIn - 40);

	if (L.koSpeedflip) L.steps = { lineUp, sf, in };
	else { lineUp.name = "Boost at the ball"; L.steps = { lineUp, in }; }
	const int per = static_cast<int>(L.steps.size());
	const std::string side0 = back ? "" : " (right)";
	if (L.koSpeedflip)
		L.stages = { { 0, 0, "Step 1: Boost and line up" }, { 1, 1, "Step 2: The speedflip" }, { 0, 1, "Steps 1-2 together" },
			{ 2, 2, "Step 3: Flip into the ball" }, { 0, 2, "The whole kickoff" + side0, true } };
	else
		L.stages = { { 0, 0, "Step 1: Boost at the ball" }, { 1, 1, "Step 2: Flip into it" }, { 0, 1, "The whole kickoff" + side0, true } };
	if (L.koSides.size() > 1)
	{
		// The left spawn: the same steps from the mirrored kickoff (practiced whole)
		const KickoffSide& b = L.koSides[1];
		for (int i = 0; i < per; i++)
		{
			LessonStep st = L.steps[i];
			st.name += back ? " (mirrored)" : " (left)";
			st.start = L.steps[i].start - a.start + b.start;
			L.steps.push_back(st);
		}
		L.stages.push_back({ per, 2 * per - 1, back ? "The whole kickoff (mirrored)" : "The whole kickoff (left)", true });
	}
	L.notes = {
		"Who wins a kickoff isn't only who gets there first: in the replays, getting there 0.15 s+ first sent the ball up field 61% of the time, but your team only won the next touch 21% (40% even, 40% lost). Hit it clean and be ready for the next touch.",
		"Variations: at SSL a delayed kickoff (arrive a beat late, let them hit it into you) and a hook (curve in from the side) win the ball more often than a straight speedflip. A fake (sit still and wait) beats a flip-in more than it loses.",
		"Teammates: sitting back in the middle beat cheating up at SSL. Grab corner boost only when your taker is sure to get there first.",
		"Test it against real kickoffs: the Combine's Kickoffs event plays a real taker's kickoff from ranked and pro games against you.",
	};
	return L;
}

static int KoWatchEnd(const Lesson& L, int stage)
{
	const LessonStage& st = L.stages[stage];
	const KickoffSide& k = KoSideFor(L, L.steps[st.first].start);
	switch (KoStepKind(L, st.last))
	{
	case 0: return L.koSpeedflip ? k.jump + 14 : k.flipIn - 2;
	case 1: return k.land + 14;
	default: return std::min(k.end, k.touch + 90);
	}
}

static bool KoStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	const int t = static_cast<int>(full.size()) - 1;
	if (t <= startTick + 4) return false;
	const LessonStage& st = L.stages[stage];
	const KickoffSide& m = KoSideFor(L, startTick);
	const int lastKind = KoStepKind(L, st.last);
	if (t > startTick + 720) return true;   // 6 s: something went wrong
	KickoffSide k = FindKo(full, m.start, t, L.koSpeedflip);
	if (lastKind == 0)
	{
		if (L.koSpeedflip) return (k.jump >= startTick && t >= k.jump + 10) || t > m.jump + 40;
		return (k.flipIn >= startTick) || t >= m.flipIn + 6;   // the drive is over once you go for the flip
	}
	if (lastKind == 1) return (k.dodge >= 0 && k.land >= 0 && t >= k.land + 8) || t > m.land + 48;
	// up to the hit: a second after it (where the ball's going is part of the score), or a second past the model's
	return (k.touch >= 0 && t >= k.touch + 120) || t > m.touch + 180;
}

static StageScore KoScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	StageScore out;
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const LessonStage& st = L.stages[stage];
	const KickoffSide& m = KoSideFor(L, startTick);
	const KickoffSide k = FindKo(full, m.start, n - 1, L.koSpeedflip);
	struct Why { int score; std::string text; };
	std::vector<Why> whys;
	const int firstKind = KoStepKind(L, st.first), lastKind = KoStepKind(L, st.last);

	// The start: boost from go, the angle, the jump's timing
	if (firstKind == 0 && L.koSpeedflip)
	{
		int sc = 5;
		const int to = k.jump >= 0 ? k.jump : std::min(n - 1, m.jump);
		int on = 0, all = 0;
		for (int t = startTick; t < to; t++) { all++; on += full[t].boost; }
		if (all > 0 && on < all * 0.9f) { sc = std::min(sc, 3); whys.push_back({ 3, std::format("Boost from the very first moment (on for {:.0f}% of the start)", 100.0f * on / all) }); }
		if (k.jump < 0 && k.flipIn >= 0) { sc = 2; whys.push_back({ 2, "Went straight for the flip-in: no speedflip" }); }
		else if (k.jump < 0) { sc = 1; whys.push_back({ 1, "No jump: the speedflip starts with one" }); }
		else
		{
			const float d = (k.jump - m.jump) / 120.0f;
			const float ad = std::fabs(d);
			const int js = ad <= 0.05f ? 5 : ad <= 0.10f ? 4 : ad <= 0.20f ? 3 : 2;
			if (js < 5) whys.push_back({ js, std::format("Jumped {:.2f} s {} than the model", ad, d > 0 ? "later" : "earlier") });
			sc = std::min(sc, js);
			const float ah = AngleDiff(HeadingDeg(full[k.jump]), HeadingDeg(full[m.jump]));
			if (ah > 6.0f)
			{
				const int hs = ah <= 12.0f ? 4 : ah <= 20.0f ? 3 : 2;
				sc = std::min(sc, hs);
				whys.push_back({ hs, std::format("Pointed {:.0f} degrees off the model's angle when you jumped", ah) });
			}
		}
		out.stepScores.push_back(sc);
	}
	else if (firstKind == 0)
	{
		// flip-in lesson: the drive
		int sc = 5;
		const int to = k.flipIn >= 0 ? k.flipIn : std::min(n - 1, m.flipIn);
		int on = 0, all = 0;
		for (int t = startTick; t < to; t++) { all++; on += full[t].boost; }
		if (all > 0 && on < all * 0.9f) { sc = 3; whys.push_back({ 3, std::format("Hold boost the whole way (on for {:.0f}%)", 100.0f * on / all) }); }
		const float sp = Speed(full[to]), msp = Speed(full[std::min(static_cast<int>(full.size()) - 1, m.flipIn)]);
		if (sp < msp - 150.0f) { sc = std::min(sc, sp < msp - 400.0f ? 2 : 4); whys.push_back({ sc, std::format("{:.0f} uu/s going into the flip (the model: {:.0f})", sp, msp) }); }
		out.stepScores.push_back(sc);
	}

	// The speedflip itself
	if (L.koSpeedflip && firstKind <= 1 && lastKind >= 1)
	{
		int sc = 5;
		if (k.dodge < 0)
		{
			sc = k.jump < 0 && k.flipIn >= 0 ? 2 : 1;   // went straight for the flip-in: a kickoff, just not this one
			if (!(firstKind == 0 && k.jump < 0)) whys.push_back({ sc, k.jump < 0 ? "No speedflip: you went straight for the flip-in" : "No flip after the jump" });
		}
		else
		{
			const Frame& d = full[k.dodge];
			if (!StickDiagonal(d)) { sc = std::min(sc, 3); whys.push_back({ 3, "Flip diagonally (stick forward and to the side), not straight" }); }
			const float gap = (k.dodge - k.jump) / 120.0f, mgap = (m.dodge - m.jump) / 120.0f;
			if (gap > mgap + 0.08f) { sc = std::min(sc, gap > mgap + 0.2f ? 3 : 4); whys.push_back({ sc, std::format("Flip sooner after the jump ({:.2f} s; the model: {:.2f})", gap, mgap) }); }
			// the cancel: stick back within 10 ticks, and held for most of the flip
			int firstBack = -1, back = 0, span = 0;
			const int until = std::min({ n - 1, k.dodge + 50, k.land >= 0 ? k.land : n - 1 });
			for (int t = k.dodge + 1; t <= until; t++) { span++; if (full[t].pitch >= 0.5f) { back++; if (firstBack < 0) firstBack = t; } }
			if (firstBack < 0 || firstBack > k.dodge + 10 || back < span * 0.7f)
			{
				sc = std::min(sc, 2);
				whys.push_back({ 2, firstBack < 0 ? "Pull the stick back right after the flip: that's the cancel" : "Pull the stick back sooner and hold it there (the cancel)" });
			}
			if (k.land < 0) { sc = std::min(sc, 2); whys.push_back({ 2, "Never landed before the ball" }); }
			else
			{
				const Frame& l = full[k.land];
				if (std::fabs(Deg(l.carPitch)) > 25.0f || std::fabs(Deg(l.carRoll)) > 25.0f) { sc = std::min(sc, 2); whys.push_back({ 2, "Landed tilted: hold air roll to keep the wheels flat" }); }
				const float ls = Speed(l), mls = Speed(full[std::min(n - 1, m.land)]);
				if (ls < mls - 120.0f) { const int s2 = ls < mls - 300.0f ? 3 : 4; sc = std::min(sc, s2); whys.push_back({ s2, std::format("Landed going {:.0f} (the model: {:.0f})", ls, mls) }); }
			}
		}
		out.stepScores.push_back(sc);
	}

	// Into the ball, or the whole kickoff: time to the ball, the flip into it, where the ball went
	if (lastKind == 2)
	{
		int sc;
		const bool whole = st.whole;
		if (k.arrive < 0 || k.touch < 0) { sc = 1; whys.push_back({ 1, k.arrive >= 0 ? "Got there but never touched it" : "Never got to the ball" }); }
		else
		{
			std::string why;
			if (whole) sc = StarScore(L, (k.arrive - startTick) / 120.0f, why);
			else
			{
				const float d = (k.arrive - m.arrive) / 120.0f;
				sc = d <= 0.05f ? 5 : d <= 0.10f ? 4 : d <= 0.20f ? 3 : d <= 0.35f ? 2 : 1;
				why = std::fabs(d) < 0.01f ? "At the ball with the model" : std::format("At the ball {:.2f} s {} the model", std::fabs(d), d > 0 ? "after" : "before");
			}
			whys.push_back({ sc, why });
			// flipped into it: a jump started within 0.5 s before the hit
			const int fj = RisingJump(full, std::max(startTick, k.touch - 60), k.touch);
			if (fj < 0) { sc = std::min(sc, 3); whys.push_back({ 3, "Flip into the ball for a stronger hit" }); }
			const int after = std::min(n - 1, k.touch + 60);
			if (full[after].ballVY < -300.0f) { sc = std::min(sc, 2); whys.push_back({ 2, "The ball went back toward your goal: hit through the middle of it" }); }
			if (whole && L.koSpeedflip && k.dodge >= 0 && !StickDiagonal(full[k.dodge])) { sc = std::min(sc, 3); whys.push_back({ 3, "A straight flip, not a speedflip" }); }
		}
		out.stepScores.push_back(sc);
	}

	int score = 5;
	for (int s : out.stepScores) score = std::min(score, s);
	out.score = out.stepScores.empty() ? 0 : score;
	std::stable_sort(whys.begin(), whys.end(), [](const Why& x, const Why& y) { return x.score < y.score; });
	for (const Why& w : whys) out.lines.push_back(w.text);
	if (out.lines.empty() || out.score == 5) out.lines.insert(out.lines.begin(), out.score == 5 ? "Clean" : "");
	return out;
}

// ============================================================
// Foundations: shot lessons (Nov 2). Aerial, power shot and dribble (tier 1), fast aerial and off the wall (tier 2),
// each taught from one of Coop's own best reps. Steps are the rulebook's steps for the mechanic (Mechanics.cpp), each
// starting from the model's car and ball; every attempt plays on to the finish, and the review's grader scores it
// with the model as "your fastest goal on this shot", so the timing grades are against the model. A step is scored on
// its own segment's grade; the whole thing on the rep's overall grade, like any rep (power shot: how hard it went in).
// ============================================================

namespace
{
	std::string Lower(std::string s)
	{
		for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return s;
	}

	int FromGrade(Grade g)
	{
		switch (g)
		{
		case Grade::Calculated:
		case Grade::Clean:  return 5;
		case Grade::Solid:  return 4;
		case Grade::Sloppy: return 3;
		case Grade::Costly: return 2;
		case Grade::None:   return 0;
		default:            return 1;
		}
	}

	// The segment of a kind that belongs to the shot (its touch), else the last one of that kind
	int SegOf(const RepSegments& segs, SegKind k)
	{
		const int shotTouch = static_cast<int>(segs.touchTicks.size());
		int any = -1, mine = -1;
		for (int i = 0; i < static_cast<int>(segs.segs.size()); i++)
		{
			if (segs.segs[i].kind != k) continue;
			any = i;
			if (segs.segs[i].touch == shotTouch || segs.segs[i].touch == 0) mine = i;
		}
		return mine >= 0 ? mine : any;
	}

	const SegmentGrade* GradeOf(const RepGrade& g, int segIndex)
	{
		for (const SegmentGrade& sg : g.segs) if (sg.segIndex == segIndex && sg.grade != Grade::None) return &sg;
		return nullptr;
	}

	// The first jump at or after a tick (the takeoff)
	int JumpFrom(const std::vector<Frame>& fr, int from)
	{
		for (int t = std::max(1, from); t < static_cast<int>(fr.size()); t++) if (fr[t].jump && !fr[t - 1].jump) return t;
		return -1;
	}

	// Power: the shot's speed and whether you flipped into it (a second jump press within 0.5 s before the hit)
	int PowerLevel(const std::vector<Frame>& full, const ShotAnalysis& s, bool wentIn, std::string& why)
	{
		if (!s.valid) { why = "Never touched the ball"; return 1; }
		int presses = 0;
		for (int t = std::max(1, s.touchTick - 60); t <= s.touchTick && t < static_cast<int>(full.size()); t++) presses += full[t].jump && !full[t - 1].jump;
		const bool flipped = presses >= 2 || (presses >= 1 && !full[std::max(0, s.touchTick - 1)].onGround && !full[std::max(0, s.touchTick - 1)].hasFlip);
		const float kph = s.shotSpeed * 0.036f;
		int sc = s.shotSpeed >= 3100.0f ? 5 : s.shotSpeed >= 2700.0f ? 4 : 3;
		why = std::format("{:.0f} uu/s ({:.0f} km/h){}", s.shotSpeed, kph, sc == 5 ? ", a real power shot" : sc == 4 ? ", hard" : ": hit it harder (2,700 for a 4, 3,100 for a 5)");
		if (!flipped && sc > 4) { sc = 4; why += ", no flip into it"; }
		if (!wentIn)
		{
			sc = std::min(sc, s.reachesGoalLine && s.missDist <= 150.0f ? 2 : 1);
			why = (!s.reachesGoalLine ? std::string("Never reached the goal") : s.missDir.empty() || s.missDist < 1.0f ? std::string("Just missed (off the frame)")
				: std::format("Missed {} by {:.0f}", s.missDir, s.missDist)) + " at " + why;
		}
		return sc;
	}

	// Steps that start a little before their segment, so there's time to do them
	int StepStartFor(SegKind k, const Segment& sg, const ShotAnalysis& s, int prevStart)
	{
		int st = sg.start;
		if (k == SegKind::Contact) st = s.touchTick - 50;
		else if (k == SegKind::Flick || k == SegKind::Takeoff) st = sg.start - 24;
		return std::max({ st, prevStart + 6, 1, s.startTick });
	}

	struct ShotText { const char* what; const char* why; std::vector<std::string> tips; const char* cue; };
}

Lesson Lessons::Shot(const std::string& id, const std::vector<Frame>& model)
{
	Lesson L;
	L.kind = LessonKind::Shot;
	L.id = id;
	L.model = model;
	if (model.size() < 60) return Lesson{};
	if (id == "aerial") { L.name = "Aerial"; L.shotType = ShotType::Aerial; }
	else if (id == "fast_aerial") { L.name = "Fast aerial"; L.shotType = ShotType::Aerial; L.needDoubleJump = true; }
	else if (id == "off_the_wall") { L.name = "Off the wall"; L.shotType = ShotType::Aerial; L.needWallTakeoff = true; }
	else if (id == "power_shot") { L.name = "Power shot"; L.shotType = ShotType::Ground; L.power = true; }
	else if (id == "dribble") { L.name = "Dribble"; L.shotType = ShotType::Dribble; }
	else if (id == "front_flick") { L.name = "Front flick"; L.shotType = ShotType::Dribble; L.needFrontFlick = true; }   // (Nov 3) the model: Coop's front flick goal, Oct 10
	// (Nov 4) models from Coop's saved reps: a goal hit while driving on the wall, and an aerial goal off a ball that came
	// off the backboard first. The shot type is whatever the grader made of the model (a wall hit can grade as an aerial).
	else if (id == "wall_hit") { L.name = "Wall hit"; L.needWallTouch = true; }
	else if (id == "backboard_read") { L.name = "Backboard read"; L.shotType = ShotType::Aerial; L.needBackboard = true; }
	else return Lesson{};
	L.modelShot = ShotGeometry::Analyze(model);
	const ShotAnalysis& s = L.modelShot;
	if (L.needWallTouch && s.valid) L.shotType = s.type;
	if (!s.valid || s.type != L.shotType) return Lesson{};
	const RepSegments segs = Segmenter::Split(model, -1);
	const int takeoff = SegOf(segs, SegKind::Takeoff);
	L.modelRef.valid = true;
	L.modelRef.bestFile = "model";
	L.modelRef.touchSec = (s.touchTick - s.startTick) / 120.0f;
	L.modelRef.jumpSec = takeoff >= 0 ? (segs.segs[takeoff].start - s.startTick) / 120.0f : -1.0f;
	L.shotTick = s.touchTick;
	const float jumpS = takeoff >= 0 ? (segs.segs[takeoff].start - s.startTick) / 120.0f : 0.0f;
	const float touchS = (s.touchTick - s.startTick) / 120.0f;
	const Frame& hit = model[s.touchTick];

	auto text = [&](SegKind k) -> ShotText {
		if (L.needWallTouch)
		{
			if (k == SegKind::Contact) return { "Hit the ball while you're still driving on the wall, through it toward the goal.",
				"Hitting it off the wall turns a ball the other team gave up on into a shot, and you're already moving toward the net.",
				{ "Stay on the wall for the hit: no jump.", "Drive into the ball, not alongside it.", std::format("The model hits it {:.0f} high.", hit.ballZ) },
				"On the wall, through the ball." };
			return { "Drive up the wall on the ball's line, matching its speed, so you reach it while it's still on the wall.",
				"The wall hit is all timing: up the wall too early and you're waiting, too late and it's past you.",
				{ "Turn up the wall early and smoothly.", "Keep the ball in front of you, a little ahead of your nose.", std::format("The model hits it {:.2f} s in.", touchS) },
				"Up the wall, on its line." };
		}
		if (L.needBackboard && k == SegKind::Approach) return { "Watch the ball off the backboard and get to where it's coming down, then jump to meet it.",
			"Reading the bounce is the whole skill: the ball comes off the backboard faster and lower than people expect.",
			{ "Watch the ball hit the backboard before you commit.", "Go to where it will be, not where it is.", std::format("Jump at about {:.2f} s (the model).", jumpS) },
			"Read the bounce, then go." };
		switch (L.shotType)
		{
		case ShotType::Aerial:
			switch (k)
			{
			case SegKind::Approach: return { "Drive to the spot under the ball's path and jump at the right moment.",
				"Jump too early and you hang in the air waiting for it; too late and it's past you. The jump sets up the whole aerial.",
				{ "Watch where the ball is going, not where it is.", "Line up so the ball, you and the goal are close to one line.",
				  std::format("Jump at about {:.2f} s (the model).", jumpS) }, "Lined up, jump on time." };
			case SegKind::Takeoff: return { L.needDoubleJump ? "Jump, then jump again straight away while pulling the nose up, boosting the whole time."
					: L.needWallTakeoff ? "Drive up the wall toward the ball and jump off it with the nose pointed at the ball, boosting."
					: "Jump, pull the nose up toward the ball and boost straight away.",
				L.needDoubleJump ? "The second jump right after the first is free height: you get to high balls before anyone who single-jumps."
					: "The first half second off the ground is where your height comes from: boost late and you never catch up.",
				L.needDoubleJump ? std::vector<std::string>{ "Hold the first jump a moment, then tap jump again (within about 0.1-0.2 s).", "Pull back while you jump so the nose comes up.", "Boost from the first jump on." }
					: std::vector<std::string>{ "Boost the moment you leave the ground.", "Pull back to point the nose at the ball.", "Hold the jump button a moment for full height." },
				L.needDoubleJump ? "Jump, jump again, nose up, boost." : "Nose up, boost right away." };
			case SegKind::Flight: return { "Fly to where you'll meet the ball, steering with the stick and feathering boost.",
				"Aerials are won in the flight: arriving early or late is what makes a touch weak or a whiff.",
				{ "Aim at where the ball will be, on its goal side.", "Feather boost if you'd get there early.",
				  std::format("The model meets it {:.0f} high, {:.2f} s after the jump.", hit.ballZ, touchS - jumpS) }, "Fly to the meeting point." };
			default: return { "Hit the ball with the nose of the car, through its middle toward the goal.",
				"A clean hit puts all of your flight's speed into the ball.",
				{ "Point the nose at the side of the ball away from the goal.", "Keep boosting into the hit.", "Small corrections only in the last moment." },
				"Nose through the ball, toward the goal." };
			}
		case ShotType::Ground:
			if (k == SegKind::Approach) return { "Get behind the ball on the line to the goal and build up all the speed you can.",
				"Power comes from your speed and a straight line through the ball. A sharp turn at the end costs both.",
				{ "Drive to the side of the ball away from the goal, then turn in early.", "Hold boost on the way in.", "Come in on the line from the goal through the ball." },
				"Behind it, on the line, full speed." };
			return { "Jump and flip forward into the ball just before you reach it.",
				"The flip adds a burst of speed at the moment of the hit: it's the difference between a hard shot and a power shot.",
				{ std::format("Jump about {:.2f} s before the hit (the model), then flip right after.", (s.touchTick - (takeoff >= 0 ? segs.segs[takeoff].start : s.touchTick - 30)) / 120.0f),
				  "Flip forward through the middle of the ball.", "Keep boosting until the flip." },
				"Flip through the middle of the ball." };
		default:
			if (k == SegKind::Dribble) return { "Keep the ball balanced on your roof while you drive it toward the goal.",
				"A steady carry is what gives you the flick and the shot. Lose it and there's nothing to flick.",
				{ "Small steering inputs, steady throttle.", "Ball rolling forward: speed up a touch. Rolling back: ease off.",
				  std::format("Carry it at a steady {:.0f} uu/s (the model's pace).", s.dribblePace) }, "Steady speed, ball on the roof." };
			if (k == SegKind::Flick && L.needFrontFlick) return { "Jump to pop the ball off your roof, wait a beat, then flip straight forward into it.",
				"A front flick sends the ball up and over the defender in front of you, fast. Straight forward is the point: a diagonal or a 45 is a different flick.",
				{ "Stick straight forward for the flip, no sideways.", std::format("Wait about {:.2f} s between the jump and the flip (the model).", s.flipDelay),
				  "Point the car at the goal before you jump." },
				"Jump, beat, straight forward." };
			if (k == SegKind::Flick) return { "Jump to pop the ball up off your roof, then flip forward into it.",
				"The flick turns a carry into a shot: the ball leaves fast and up, over anyone in front of you.",
				{ "Jump, wait a moment, then flip forward.", "Point the car at the goal before you flick.",
				  std::format("Flick when you're in range (the model flicks from {:.0f} out).", std::fabs(5120.0f * s.goalSign - hit.ballY)) },
				"Jump, then flip forward into it." };
			return { "Approach the ball and settle it on your roof.", "The carry can't start until the ball sits still on the car.",
				{ "Get under the ball as it drops.", "Match its speed so it lands softly." }, "Get it settled on the roof." };
		}
	};

	const MechanicSpec& spec = Mechanics::Spec(L.shotType);
	int prevStart = -1000;
	for (const MechanicStep& ms : spec.steps)
	{
		const int si = SegOf(segs, ms.seg);
		if (si < 0) continue;
		LessonStep st;
		const ShotText tx = text(ms.seg);
		st.name = ms.name;
		st.what = tx.what; st.why = tx.why; st.tips = tx.tips; st.cue = tx.cue;
		st.start = StepStartFor(ms.seg, segs.segs[si], s, prevStart);
		if (st.start >= s.touchTick) continue;
		prevStart = st.start;
		L.steps.push_back(st);
		L.stepSegs.push_back(ms.seg);
	}
	if (L.power && !L.steps.empty()) L.steps.back().name = "The flip into it";
	const int n = static_cast<int>(L.steps.size());
	if (n == 0) return Lesson{};
	// Stages: step 1, step 2, steps 1-2, step 3, steps 1-3, ... then the whole thing
	for (int i = 0; i < n; i++)
	{
		L.stages.push_back({ i, i, std::format("Step {}: {}", i + 1, L.steps[i].name) });
		if (i >= 1 && i + 1 < n) L.stages.push_back({ 0, i, std::format("Steps 1-{} together", i + 1) });
	}
	L.stages.push_back({ 0, n - 1, "The whole " + (id == "aerial" ? std::string("aerial") : L.power || id == "dribble" ? std::string("shot") : Lower(L.name)), true });
	if (n == 1) L.stages.erase(L.stages.begin());
	return L;
}

static int ShotWatchEnd(const Lesson& L, int stage)
{
	const int last = L.stages[stage].last;
	if (last + 1 < static_cast<int>(L.steps.size())) return L.steps[last + 1].start + 20;
	return std::min(static_cast<int>(L.model.size()) - 1, L.shotTick + 90);
}

static StageScore ShotScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst, bool compareToModel)
{
	StageScore out;
	const LessonStage& st = L.stages[stage];
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const int end = n - 1;
	const ShotAnalysis shot = ShotGeometry::Analyze(full);
	const RepSegments segs = Segmenter::Split(full, end);
	const RepGrade g = Grader::GradeRep(full, segs, shot, wentIn, floorFirst, L.modelRef);
	struct Why { int score; std::string text; };
	std::vector<Why> whys;

	for (int step = st.first; step <= st.last; step++)
	{
		const SegKind k = L.stepSegs[step];
		int sc = 0;
		std::string why;
		const int si = SegOf(segs, k);
		const SegmentGrade* sg = si >= 0 ? GradeOf(g, si) : nullptr;
		if (L.shotType == ShotType::Aerial && k == SegKind::Approach)
		{
			// the line-up's job is the jump: graded by the jump's timing against the model's
			const SegmentGrade* tg = GradeOf(g, SegOf(segs, SegKind::Takeoff));
			if (!shot.valid && SegOf(segs, SegKind::Takeoff) < 0) { sc = 1; why = "Never jumped for it"; }
			else if (tg) { sc = FromGrade(tg->grade); why = tg->why; }
			else { sc = 3; why = "Couldn't time your jump against the model's"; }
		}
		else if (L.shotType == ShotType::Aerial && k == SegKind::Takeoff)
		{
			// the climb: height gained in the 0.4 s after the jump, against the model's, and boosting from the start
			const int j = JumpFrom(full, startTick), mj = JumpFrom(L.model, std::max(0, L.steps[step].start - 30));
			if (j < 0) { sc = 1; why = "Never jumped"; }
			else
			{
				const int a = std::min(n - 1, j + 48), ma = std::min(static_cast<int>(L.model.size()) - 1, mj + 48);
				const float gain = full[a].carZ - full[j].carZ, mGain = mj >= 0 ? L.model[ma].carZ - L.model[mj].carZ : gain;
				const float r = mGain > 1.0f ? gain / mGain : 1.0f;
				sc = r >= 0.9f ? 5 : r >= 0.75f ? 4 : r >= 0.6f ? 3 : 2;
				why = std::format("Climbed {:.0f} in the first 0.4 s (the model: {:.0f})", gain, mGain);
				int boostAt = -1;
				for (int t = j; t <= std::min(n - 1, j + 36); t++) if (full[t].boost) { boostAt = t; break; }
				if (boostAt < 0 || boostAt > j + 18) { sc = std::min(sc, 3); why += boostAt < 0 ? ", no boost off the ground" : ", boost came late"; }
				if (L.needDoubleJump)
				{
					const int j2 = JumpFrom(full, j + 2);
					if (j2 < 0 || j2 > j + 36) { sc = std::min(sc, 2); why += ", no second jump right after the first"; }
				}
				if (L.needWallTakeoff && !(full[std::max(0, j - 1)].onWall || full[std::max(0, j - 1)].carZ > 150.0f)) { sc = std::min(sc, 2); why += ", but you left from the ground, not the wall"; }
			}
		}
		else if (L.power && k == SegKind::Contact) sc = PowerLevel(full, shot, wentIn, why);
		else if (L.needFrontFlick && k == SegKind::Flick && sg)
		{
			sc = FromGrade(sg->grade); why = sg->why;
			if (shot.flipKind != FlipKind::Front && shot.flipKind != FlipKind::DiagonalFront)
			{
				sc = std::min(sc, 2);
				why = std::format("That was a {}, not a front flick: stick straight forward for the flip", ShotGeometry::FlipName(shot.flipKind, true));
			}
		}
		else if (sg) { sc = FromGrade(sg->grade); why = sg->why; }
		else if (si < 0) { sc = !shot.valid ? 1 : 2; why = std::format("No {} in this attempt", Lower(L.steps[step].name)); }
		else { sc = 3; why = "Not graded"; }
		sc = std::max(1, sc);
		out.stepScores.push_back(sc);
		whys.push_back({ sc, std::format("{}: {}", L.steps[step].name, why) });
	}

	int score = 5;
	for (int s2 : out.stepScores) score = std::min(score, s2);
	if (st.whole)
	{
		int overall = g.overall;
		std::string why = g.overallWhy;
		if (L.power) { overall = PowerLevel(full, shot, wentIn, why); why = (wentIn ? "Goal, " : "") + why; }
		if (shot.valid && shot.type != L.shotType) { overall = std::min(overall, wentIn ? 2 : 1); why = std::format("Not a {}: {}", Lower(Mechanics::Spec(L.shotType).name), why); }
		// fast aerial / off the wall: the takeoff is the point of the lesson
		for (size_t i = 0; i < out.stepScores.size(); i++)
			if ((L.stepSegs[st.first + i] == SegKind::Takeoff && (L.needDoubleJump || L.needWallTakeoff) || L.stepSegs[st.first + i] == SegKind::Flick && L.needFrontFlick) && out.stepScores[i] <= 2) overall = std::min(overall, 3);
		// wall hit: the hit has to come from the wall; backboard read: off the backboard first, then your touch
		if (L.needWallTouch && shot.valid)
		{
			bool onWall = false;
			for (int t = std::max(0, shot.touchTick - 2); t <= std::min(n - 1, shot.touchTick + 1); t++) onWall = onWall || full[t].onWall;
			if (!onWall) { overall = std::min(overall, 2); why = "You hit it off the ground or in the air: stay on the wall for the hit. " + why; }
		}
		if (L.needBackboard && shot.valid)
		{
			int bb = -1, first = -1;
			for (int t = std::max(1, startTick); t < n; t++) if (full[t].touch && first < 0) first = t;
			for (int t = std::max(1, startTick); t < shot.touchTick && t < n; t++)
				if (std::fabs(full[t].ballY) > 4800.0f && full[t].ballZ > 600.0f && full[t - 1].ballVY * full[t].ballVY < 0 && std::fabs(full[t - 1].ballVY) > 300.0f) { bb = t; break; }
			if (bb < 0 || (first >= 0 && first < bb)) { overall = std::min(overall, 2); why = "Wait for it to come off the backboard, then read it. " + why; }
		}
		score = overall;
		whys.insert(whys.begin(), { 0, std::format("Overall: {} ({})", overall, why) });
	}
	if (compareToModel)
	{
		const float dev = LineMatch(L, stage, full, startTick, end);
		out.lineMatch = dev;
		whys.push_back({ 6, std::format("Line match: {:.0f} from the model's path on average{}", dev, dev <= 150.0f ? " (right on it)" : dev <= 400.0f ? " (close)" : " (your own line)") });
	}
	std::stable_sort(whys.begin(), whys.end(), [](const Why& a, const Why& b) { return a.score < b.score; });
	// The grader talks about "your fastest goal on this shot"; in a lesson that's the model
	auto say = [](std::string t) {
		for (const char* from : { "your fastest goal on this shot", "your best attempt" })
			for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p)) { t.replace(p, std::strlen(from), "the model"); p += 9; }
		return t;
	};
	for (const Why& w : whys) out.lines.push_back(say(w.text));
	out.score = std::max(1, score);
	return out;
}

// ============================================================
// Foundations: movement lessons (Nov 2). Half flip, powerslide turn (tier 1), speedflip and wavedash (tier 2), from
// models found in RocketSim (tools/learn/move_model.cpp) with the ball parked out of the way. Each is timed against the
// model: the half flip and the powerslide turn on how soon you're turned around and going (facing back the way you
// came, 1,000 uu/s that way), the speedflip on how soon you're supersonic from a standstill, the wavedash on the speed
// it adds from 1,400 (top speed without boost).
// ============================================================

namespace
{
	float HeadingRad(const Frame& f) { return f.carYaw * (3.14159265f / 32768.0f); }
	float RadDiff(float a, float b) { float d = a - b; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f; return std::fabs(d); }
	float Spd(const Frame& f) { return std::sqrt(f.carVX * f.carVX + f.carVY * f.carVY + f.carVZ * f.carVZ); }
	int Press(const std::vector<Frame>& fr, int from, int to)
	{
		const int n = static_cast<int>(fr.size());
		for (int t = std::max(0, from); t <= to && t < n; t++) if (fr[t].jump && (t == 0 || !fr[t - 1].jump)) return t;
		return -1;
	}

	struct MoveEvents { int act = -1, dodge = -1, land = -1, done = -1; float doneSpeed = 0; };

	// What happened in a move attempt (or the model), by the inputs and the car
	MoveEvents FindMove(const Lesson& L, const std::vector<Frame>& fr, int from)
	{
		MoveEvents e;
		const int n = static_cast<int>(fr.size());
		const float back = HeadingRad(L.model[0]) + 3.14159265f;
		const bool turn = L.moveKind == "half_flip" || L.moveKind == "powerslide";
		if (L.moveKind == "recovery")
		{
			// knocked into the air: land wheels first, then drive back to your back post (the model's spot)
			e.act = std::max(0, from);
			for (int t = std::max(1, from); t < n; t++) if (fr[t].onGround && !fr[t - 1].onGround) { e.land = t; break; }
			if (e.land < 0) return e;
			for (int t = e.land; t < n; t++)
			{
				const Frame& f = fr[t];
				const float dx = 800.0f - f.carX, dy = -4900.0f - f.carY, d = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
				const float toward = (f.carVX * dx + f.carVY * dy) / d;
				const float tilt = std::max(std::fabs(f.carPitch), std::fabs(f.carRoll)) * (180.0f / 32768.0f);
				if (f.onGround && tilt < 15.0f && RadDiff(HeadingRad(f), std::atan2(dy, dx)) < 0.5f && toward > 1000.0f) { e.done = t; e.doneSpeed = Spd(f); break; }
			}
			return e;
		}
		if (L.moveKind == "powerslide") { for (int t = std::max(0, from); t < n; t++) if (fr[t].handbrake) { e.act = t; break; } }
		else
		{
			e.act = Press(fr, from, n - 1);
			if (e.act >= 0) { int rel = -1; for (int t = e.act; t < n; t++) if (!fr[t].jump) { rel = t; break; } if (rel >= 0) e.dodge = Press(fr, rel, std::min(n - 1, e.act + 200)); }
			if (e.dodge >= 0) for (int t = e.dodge + 3; t < n; t++) if (fr[t].onGround) { e.land = t; break; }
		}
		for (int t = std::max(0, from); t < n; t++)
		{
			const Frame& f = fr[t];
			if (turn)
			{
				const float toward = -(f.carVX * std::cos(back - 3.14159265f) + f.carVY * std::sin(back - 3.14159265f));
				if (f.onGround && RadDiff(HeadingRad(f), back) < 0.35f && toward > 1000.0f && (L.moveKind == "powerslide" || e.land >= 0)) { e.done = t; e.doneSpeed = Spd(f); break; }
			}
			else if (L.moveKind == "speedflip") { if (f.onGround && e.land >= 0 && t >= e.land && Spd(f) >= 2200.0f) { e.done = t; e.doneSpeed = Spd(f); break; } }
			else if (L.moveKind == "wavedash") { if (e.land >= 0 && t >= e.land + 30) { e.done = t; e.doneSpeed = Spd(f); break; } }
		}
		return e;
	}

	// How far behind the model. The models are physics-sim lines (frame-perfect inputs), so a clean human rep is a
	// tenth or two slower: (Nov 3, Coop: the whole half flip felt "SO harsh") a 5 is within 0.15 s, a 4 within 0.3
	int TimeLevel(float late) { return late <= 0.15f ? 5 : late <= 0.30f ? 4 : late <= 0.50f ? 3 : late <= 0.80f ? 2 : 1; }
}

// ============================================================
// Air control (Nov 3): air roll control (tier 1) and the tornado spin (tier 2). Models from RocketSim
// (tools/learn/orient_model.cpp): a PD attitude controller in one flight. Air roll: (1) roll all the way over and stop
// level, (2) nose to the ball, (3) wheels to the ball (how a flip reset starts), each held a moment, each step starting
// where the model finished the last one. Tornado: jump and boost up to a high ball holding air roll the whole way, nose
// on the ball, and hit it.
// ============================================================

namespace
{
	struct V3 { float x, y, z; };
	constexpr float kU2Rad = 3.14159265f / 32768.0f;
	V3 FwdOf(const Frame& f)
	{
		const float p = f.carPitch * kU2Rad, y = f.carYaw * kU2Rad;
		return { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p) };
	}
	V3 UpOf(const Frame& f)   // Unreal's rotator (RocketSim's matches; checked Nov 3)
	{
		const float p = f.carPitch * kU2Rad, y = f.carYaw * kU2Rad, r = f.carRoll * kU2Rad;
		const float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
		return { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
	}
	float Dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	V3 Cross3(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float AngTo(V3 a, V3 b) { const float n = std::sqrt(Dot3(a, a) * Dot3(b, b)); return n > 1e-6f ? std::acos(std::clamp(Dot3(a, b) / n, -1.0f, 1.0f)) : 3.14159f; }
	V3 ToBall(const Frame& f) { return { f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ }; }

	// Air roll: the tick a step's target has been held (step 0: a full turn, either way, stopped level; 1: nose on
	// the ball; 2: wheels to the ball), looking from `from`; -1 if it never was before the car came down
	int AirRollDone(const std::vector<Frame>& fr, int step, int from)
	{
		const int n = static_cast<int>(fr.size());
		float rolled = 0.0f;
		int held = 0;
		for (int t = std::max(0, from); t < n; t++)
		{
			const Frame& f = fr[t];
			if (f.onGround && t > from + 4) return -1;
			bool ok = false;
			if (step == 0)
			{
				float rate = 0.0f;
				if (t > from)
				{
					const V3 u0 = UpOf(fr[t - 1]), u1 = UpOf(f), fw = FwdOf(f);
					const float d = std::atan2(Dot3(fw, Cross3(u0, u1)), Dot3(u0, u1));
					rolled += d;
					rate = std::fabs(d) * 120.0f;
				}
				ok = std::fabs(std::fabs(rolled) - 6.2831853f) < 0.35f && UpOf(f).z > 0.94f && rate < 1.2f;
			}
			else if (step == 1) ok = AngTo(FwdOf(f), ToBall(f)) < 0.26f;
			else { const V3 u = UpOf(f); ok = AngTo({ -u.x, -u.y, -u.z }, ToBall(f)) < 0.44f; }
			held = ok ? held + 1 : 0;
			if (held >= (step == 0 ? 15 : 30)) return t;
		}
		return -1;
	}

	// (Nov 4, Coop) A tornado spin is directional air roll with the stick held the other way: the nose circles. The
	// lesson is the hover: point the spin's middle straight up and boost, and you hang in the air. revs = times the nose
	// went round (around straight up), coneDeg = how far off vertical it sat on average, hoverTicks = ticks spent
	// hovering (up high, spinning, not flying off).
	struct TornadoEvents { int leave = -1, touch = -1, hoverTicks = 0; float rollShare = 0, revs = 0, coneDeg = 0; };
	TornadoEvents FindTornado(const std::vector<Frame>& fr, int from, int cap = 150)   // cap: how long after leaving the ground to measure, at most
	{
		TornadoEvents e;
		const int n = static_cast<int>(fr.size());
		for (int t = std::max(1, from); t < n; t++) if (!fr[t].onGround && fr[t - 1].onGround) { e.leave = t; break; }
		if (e.leave < 0) return e;
		const int end = std::min(n - 1, e.leave + cap);
		int air = 0, on = 0, coneN = 0;
		float cone = 0.0f, turned = 0.0f, prevAz = 0.0f;
		bool havePrev = false;
		for (int t = e.leave; t <= end; t++)
		{
			if (fr[t].onGround) break;
			air++;
			on += std::fabs(fr[t].roll) >= 0.7f;
			if (t < e.leave + 24) continue;
			// the nose's angle off straight up, and where it sits around it
			const V3 f = FwdOf(fr[t]), b = { 0, 0, 1 };
			const float cn = AngTo(f, b) * 57.2958f;
			cone += cn / 57.2958f; coneN++;
			const Frame& q = fr[t];
			if (q.carZ > 300.0f && std::fabs(q.roll) >= 0.7f && cn >= 8.0f && cn <= 65.0f && std::hypot(q.carVX, q.carVY) < 700.0f) e.hoverTicks++;
			const V3 ref = std::fabs(b.z) < 0.9f ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
			V3 e1 = Cross3(ref, b);
			const float el = std::sqrt(Dot3(e1, e1));
			e1 = { e1.x / el, e1.y / el, e1.z / el };
			const V3 e2 = Cross3(b, e1);
			const float az = std::atan2(Dot3(f, e2), Dot3(f, e1));
			if (havePrev)
			{
				float d = az - prevAz;
				while (d > 3.14159265f) d -= 6.2831853f;
				while (d < -3.14159265f) d += 6.2831853f;
				turned += d;
			}
			prevAz = az; havePrev = true;
		}
		e.rollShare = air ? static_cast<float>(on) / air : 0.0f;
		e.revs = std::fabs(turned) / 6.2831853f;
		e.coneDeg = coneN ? cone / coneN * 57.2958f : 0.0f;
		return e;
	}

	Lesson AirControl(const std::string& id, const std::vector<Frame>& model)
	{
		Lesson L;
		L.kind = LessonKind::Move;
		L.id = id;
		L.model = model;
		if (model.size() < 60) return Lesson{};
		if (id == "air_roll")
		{
			L.moveKind = "air_roll";
			const int d0 = AirRollDone(model, 0, 0), d1 = d0 >= 0 ? AirRollDone(model, 1, d0) : -1, d2 = d1 >= 0 ? AirRollDone(model, 2, d1) : -1;
			if (d2 < 0) return Lesson{};
			L.moveStart = 0; L.moveAct = 0; L.moveDone = d2;
			L.name = "Air roll control";
			LessonStep a, b, c;
			a.name = "Roll all the way over";
			a.what = "Up in the air with no boost: air roll one full turn (either way) and stop with the car level.";
			a.why = "Air roll is how you aim in the air. Starting and stopping it on purpose is the whole skill.";
			a.tips = { "Hold air roll (or directional air roll) and let go a little before you're level: the car keeps turning for a moment.", "Tap the other way to stop the spin.", std::format("The model is round and level in {:.2f} s.", d0 / 120.0f) };
			a.cue = "Roll, let go early, stop level.";
			a.start = 0;
			b.name = "Nose to the ball";
			b.what = "Point the nose straight at the ball and hold it there.";
			b.why = "Every aerial touch starts with the nose on the ball; air roll plus pitch and yaw gets you there fastest.";
			b.tips = { "Ball cam on.", "Pitch and yaw to the ball; roll only if it's quicker that way.", std::format("Hold it a quarter second (the model takes {:.2f} s).", (d1 - d0) / 120.0f) };
			b.cue = "Nose on the ball, hold.";
			b.start = d0;
			c.name = "Wheels to the ball";
			c.what = "Turn so your wheels face the ball, and hold it.";
			c.why = "Wheels to the ball is how a flip reset starts: all four wheels touching it gives you your flip back.";
			c.tips = { "Roll and pitch so the ball is under your wheels from where you sit.", "Think of putting the car's floor against the ball.", std::format("Hold it a quarter second (the model takes {:.2f} s).", (d2 - d1) / 120.0f) };
			c.cue = "Floor to the ball, hold.";
			c.start = d1;
			L.steps = { a, b, c };
			L.stages = { { 0, 0, "Step 1: " + a.name }, { 1, 1, "Step 2: " + b.name }, { 2, 2, "Step 3: " + c.name }, { 0, 2, "All three in one flight", true } };
			return L;
		}
		L.moveKind = "tornado";
		const TornadoEvents m = FindTornado(model, 0, 100000);
		if (m.leave < 0 || m.hoverTicks < 120) return Lesson{};
		L.moveStart = 0; L.moveAct = m.leave; L.moveDone = static_cast<int>(model.size()) - 1;
		L.name = "Tornado spin";
		LessonStep a, b;
		a.name = "Point up and spin";
		a.what = "Jump, tip the nose up toward the ceiling, then hold directional air roll and push the stick all the way the other way (air roll right, stick left). The car spins, its nose circling straight up.";
		a.why = "Air roll one way and steering the other turns the car around an axis off its nose, so the nose draws a circle. With the middle of that circle pointing straight up, your boost pushes you straight up.";
		a.tips = { "Air roll right with the stick full left (or air roll left, stick full right), both held the whole way.", "Get the nose most of the way up before you start the spin.", "A little boost on the way up." };
		a.cue = "Nose up, air roll right, stick left.";
		a.start = 0;
		b.name = "Hold the hover";
		b.what = "Keep spinning and feather the boost so you hang in the air in one spot.";
		b.why = "Holding a hover means your spin is really centred: the same control you need for tornado shots, flicks and air dribbles.";
		b.tips = { "Tap boost when you start to drop, let go when you rise.", "If you drift one way, the circle's leaning: tip it back with a touch of pitch.",
			std::format("The model hovers {:.1f} s.", m.hoverTicks / 120.0f) };
		b.cue = "Spin, feather boost, stay put.";
		b.start = 0;
		L.steps = { a, b };
		L.stages = { { 0, 0, "Step 1: " + a.name }, { 0, 1, "The whole tornado spin", true } };
		return L;
	}

	bool AirControlStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
	{
		const int t = static_cast<int>(full.size()) - 1;
		if (t <= startTick + 4) return false;
		if (t > startTick + (L.moveKind == "tornado" ? 720 : 480)) return true;
		const LessonStage& st = L.stages[stage];
		if (full[t].onGround && !full[std::max(0, t - 1)].onGround && t > startTick + 10) return true;   // came down
		if (L.moveKind == "air_roll")
		{
			int from = startTick;
			for (int s = st.first; s <= st.last; s++) { from = AirRollDone(full, s, from); if (from < 0) return false; }
			return t >= from + 20;
		}
		const TornadoEvents e = FindTornado(full, startTick, 150);
		if (st.last == 0) return e.leave >= 0 && t >= e.leave + 150;
		return e.leave >= 0 && t >= e.leave + (L.moveDone - L.moveAct) + 30;   // as long as the model's flight, then a beat
	}

	StageScore AirControlScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
	{
		StageScore out;
		const LessonStage& st = L.stages[stage];
		std::vector<std::string> whys;
		if (L.moveKind == "air_roll")
		{
			int from = startTick, mFrom = L.steps[st.first].start;
			bool lost = false;
			for (int s = st.first; s <= st.last; s++)
			{
				int sc;
				std::string why;
				const int mDone = AirRollDone(L.model, s, mFrom);
				const int done = lost ? -1 : AirRollDone(full, s, from);
				if (done < 0)
				{
					sc = 1;
					why = lost ? "Didn't get to it" : s == 0 ? "Never got all the way round and stopped level" : s == 1 ? "Never held the nose on the ball" : "Never held your wheels to the ball";
					lost = true;
				}
				else
				{
					const float took = (done - from) / 120.0f, mTook = (mDone - mFrom) / 120.0f;
					sc = TimeLevel(took - mTook);
					why = std::format("Done in {:.2f} s (the model: {:.2f})", took, mTook);
					from = done;
				}
				mFrom = mDone >= 0 ? mDone : mFrom;
				out.stepScores.push_back(sc);
				whys.push_back(std::format("{}: {}", L.steps[s].name, why));
			}
		}
		else
		{
			const int cap = st.last == 0 ? 150 : 100000;   // step 1 stops 1.25 s after leaving the ground (AirControlStageDone): measure the model the same way
			const TornadoEvents e = FindTornado(full, startTick, cap), m = FindTornado(L.model, 0, cap);
			for (int s = st.first; s <= st.last; s++)
			{
				int sc;
				std::string why;
				if (e.leave < 0) { sc = 1; why = "Never left the ground"; }
				else if (s == 0)
				{
					const float r = e.rollShare, rv = e.revs, cn = e.coneDeg, mRv = std::max(0.5f, m.revs);
					why = std::format("Nose circled {:.1f} times, {:.0f} degrees off straight up (the model: {:.1f}, {:.0f})", rv, cn, m.revs, m.coneDeg);
					if (r < 0.6f) { sc = r >= 0.3f ? 2 : 1; why = std::format("Air roll held only {:.0f}% of the way: hold it the whole time", r * 100.0f); }
					else if (cn < 8.0f) { sc = 2; why = "That's a barrel roll, not a tornado: push the stick the opposite way from your air roll (air roll right, stick left) so the nose circles"; }
					else
					{
						sc = rv >= mRv * 0.8f ? 5 : rv >= mRv * 0.6f ? 4 : rv >= mRv * 0.35f ? 3 : 2;
						if (cn > 60.0f) { sc = std::min(sc, 3); why += ": you're leaning too far over, get the nose closer to straight up"; }
						else if (sc <= 3) why += ": keep the spin going the whole time";
					}
				}
				else
				{
					const float hv = e.hoverTicks / 120.0f, mh = std::max(1.0f, m.hoverTicks / 120.0f), r = hv / mh;
					sc = r >= 0.85f ? 5 : r >= 0.65f ? 4 : r >= 0.45f ? 3 : r >= 0.2f ? 2 : 1;
					why = std::format("Hovered {:.1f} s (the model: {:.1f})", hv, mh);
					if (sc <= 3) why += e.rollShare < 0.6f ? ": keep the air roll held" : ": stay up high and in one spot, feathering boost";
				}
				out.stepScores.push_back(sc);
				whys.push_back(std::format("{}: {}", L.steps[s].name, why));
			}
		}
		int score = 5;
		for (int s2 : out.stepScores) score = std::min(score, s2);
		std::vector<std::pair<int, std::string>> order;
		for (size_t i = 0; i < whys.size(); i++) order.push_back({ out.stepScores[i], whys[i] });
		std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		if (st.whole) out.lines.push_back(std::format("Overall: {}", score));
		for (auto& o : order) out.lines.push_back(o.second);
		out.score = std::max(1, score);
		return out;
	}
}

Lesson Lessons::Move(const std::string& id, const std::vector<Frame>& model)
{
	if (id == "air_roll" || id == "tornado_spin") return AirControl(id, model);
	Lesson L;
	L.kind = LessonKind::Move;
	L.id = id;
	L.model = model;
	if (model.size() < 60) return Lesson{};
	L.moveKind = id == "powerslide_turn" ? "powerslide" : id;
	const MoveEvents m = FindMove(L, model, 0);
	if (m.done < 0 || m.act < 0 || (L.moveKind != "powerslide" && L.moveKind != "recovery" && (m.dodge < 0 || m.land < 0))) return Lesson{};
	L.moveStart = 0; L.moveAct = m.act; L.moveDone = m.done;
	L.dodgeTick = m.dodge; L.landTick = m.land;
	const float doneS = m.done / 120.0f;

	LessonStep a, b;
	if (L.moveKind == "half_flip")
	{
		L.name = "Half flip";
		a.name = "The backflip";
		a.what = "From a standstill (or rolling backward), jump and flip backward straight away.";
		a.why = "A half flip turns you around twice as fast as steering, and it starts with a backflip.";
		a.tips = { "Hold the stick straight back as you press jump the second time: that's a backflip.", std::format("Flip right after the jump (the model flips {:.2f} s after it).", (m.dodge - m.act) / 120.0f), "Don't hold any air roll yet." };
		a.cue = "Jump, then flip backward.";
		a.start = 0;
		b.name = "Cancel and roll over";
		b.what = "The car jumps and starts the backflip for you. Once the nose points up, push the stick forward to stop the flip, then air roll to come upright facing the other way.";
		b.why = "The cancel stops the backflip halfway, and the air roll turns the upside-down car right way up: you land facing back the way you came.";
		b.tips = { "Push the stick all the way forward when the nose is pointing at the sky.", "Hold air roll (either way) until your wheels face the floor.", "Boost and drive off the moment you land." };
		b.cue = "Stick forward, roll over, go.";
		b.start = m.dodge + 4;
	}
	else if (L.moveKind == "speedflip")
	{
		L.name = "Speedflip";
		a.name = "Jump and diagonal flip";
		a.what = "Boosting from a standstill, jump and flip diagonally forward almost at once.";
		a.why = "The diagonal flip's push is the speedflip's speed; it has to come early, while you're still slow.";
		a.tips = { "Boost the whole time.", std::format("Jump at about {:.2f} s, then flip diagonally (stick forward and to the side) {:.2f} s later.", m.act / 120.0f, (m.dodge - m.act) / 120.0f), "A short tap of jump is enough." };
		a.cue = "Jump, diagonal flip, keep boosting.";
		a.start = 0;
		b.name = "Cancel and land";
		b.what = "The car jumps for you. Flip diagonally, pull the stick straight back right after to cancel it, hold air roll against the spin, and land flat.";
		b.why = "Cancelled, the flip's push stays and the spin goes: you land going fast and pointed where you were going.";
		b.tips = { "Pull back within a few ticks of the flip.", "Hold air roll the other way from your flip to stop the roll.", std::format("Land flat and keep boosting (the model is supersonic at {:.2f} s).", doneS) };
		b.cue = "Cancel, wheels flat.";
		b.start = m.act + 2;   // (Nov 3) handed over in the jump: the cancel comes 5 ticks after the flip, too soon to hand over between them
	}
	else if (L.moveKind == "wavedash")
	{
		L.name = "Wavedash";
		a.name = "Jump and tilt back";
		a.what = "Driving at top speed without boost, jump, and tilt the nose up a little on the way down.";
		a.why = "The back wheels touch first; that's the moment the flip turns into speed.";
		a.tips = { "A short jump is enough.", "Tilt the nose up about 20-40 degrees before you land.", "No boost: the whole point is free speed." };
		a.cue = "Small hop, nose up a touch.";
		a.start = 0;
		b.name = "Flip as you land";
		b.what = "Flip forward just as your back wheels touch the ground.";
		b.why = "Flip a moment before the wheels touch and the car slides out of the flip at higher speed; too early and you just flip.";
		b.tips = { "Watch the ground under you, not the ball.", "Push the stick forward and press jump right as you land.", std::format("Done right you go from 1,400 to about {:.0f}.", Spd(model[m.done])) };
		b.cue = "Flip right as the wheels touch.";
		b.start = std::max(0, m.dodge - 30);
	}
	else if (L.moveKind == "recovery")
	{
		L.name = "Recovery";
		a.name = "Land and go";
		a.what = "Knocked into the air and spinning: turn your wheels to the floor, land on them, and drive back toward your back post (the blue ring).";
		a.why = "A clean landing keeps your speed and gets you back in the play; landing on your roof or side costs a second or more.";
		a.tips = { "Air roll and pitch to stop the spin and get the wheels facing down early.", "Point the nose where you want to go before you land, so you drive away straight.",
			std::format("Boost and drive off the moment you land (the model is back on its wheels at {:.2f} s and driving away at {:.2f} s).", m.land / 120.0f, doneS) };
		a.cue = "Wheels down, nose where you're going.";
		a.start = 0;
	}
	else
	{
		L.name = "Powerslide turn";
		a.name = "Handbrake turn";
		a.what = "At speed, steer hard and hold the handbrake to swing the car around, then let go and drive back the way you came.";
		a.why = "A powerslide turns you around in a fraction of the space, and keeps far more speed than braking and turning.";
		a.tips = { std::format("Hold the handbrake for about {:.2f} s while steering all the way.", 40 / 120.0f), "Let go a little before you face the new way, or you'll spin past it.", "Keep the throttle on through the slide, then boost out of it." };
		a.cue = "Steer, handbrake, let go, boost.";
		a.start = 0;
	}
	L.steps.push_back(a);
	if (L.moveKind != "powerslide" && L.moveKind != "recovery")
	{
		L.steps.push_back(b);
		L.stages = { { 0, 0, "Step 1: " + a.name }, { 1, 1, "Step 2: " + b.name }, { 0, 1, "The whole " + Lower(L.name), true } };
	}
	else L.stages = { { 0, 0, "The whole " + Lower(L.name), true } };
	return L;
}

static int MoveWatchEnd(const Lesson& L, int stage)
{
	const int last = L.stages[stage].last;
	if (last + 1 < static_cast<int>(L.steps.size())) return L.steps[last + 1].start + 20;
	return std::min(static_cast<int>(L.model.size()) - 1, L.moveDone + 40);
}

static bool MoveStageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	if (L.moveKind == "air_roll" || L.moveKind == "tornado") return AirControlStageDone(L, stage, full, startTick);
	const int t = static_cast<int>(full.size()) - 1;
	if (t <= startTick + 4) return false;
	if (t > startTick + 480) return true;   // 4 s: give it up
	const LessonStage& st = L.stages[stage];
	const MoveEvents e = FindMove(L, full, 0);
	if (st.last == 0 && L.steps.size() > 1)
		return (e.dodge >= startTick && t >= e.dodge + 20) || t > L.dodgeTick + 90;   // step 1 is the flip
	return (e.done >= 0 && t >= e.done + 30) || t > L.moveDone + 240;
}

static StageScore MoveScore(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	if (L.moveKind == "air_roll" || L.moveKind == "tornado") return AirControlScore(L, stage, full, startTick);
	StageScore out;
	const int n = static_cast<int>(full.size());
	if (n <= startTick + 10) return out;
	const LessonStage& st = L.stages[stage];
	const MoveEvents e = FindMove(L, full, 0), m = FindMove(L, L.model, 0);
	struct Why { int score; std::string text; };
	std::vector<Why> whys;
	auto stickBack = [](const Frame& f) { return f.pitch >= 0.5f; };
	auto stickFwd = [](const Frame& f) { return f.pitch <= -0.5f; };
	auto diagonal = [](const Frame& f) { return f.pitch <= -0.3f && std::max(std::fabs(f.yaw), std::fabs(f.steer)) >= 0.4f; };

	for (int step = st.first; step <= st.last; step++)
	{
		int sc = 5;
		std::string why;
		if (L.moveKind == "recovery")
		{
			if (e.land < 0) { sc = 1; why = "Never landed"; }
			else
			{
				const Frame& l = full[e.land];
				const float tilt = std::max(std::fabs(l.carPitch), std::fabs(l.carRoll)) * (180.0f / 32768.0f);
				if (e.done < 0) { sc = 1; why = "Never got going back toward your back post"; }
				else { sc = TimeLevel((e.done - m.done) / 120.0f); why = std::format("Landed and driving back in {:.2f} s (the model: {:.2f})", e.done / 120.0f, m.done / 120.0f); }
				if (tilt > 30.0f) { sc = std::min(sc, 2); why = std::format("Landed {:.0f} degrees over: get the wheels to the floor first. ", tilt) + why; }
			}
		}
		else if (L.moveKind == "powerslide")
		{
			if (e.act < 0) { sc = 1; why = "No handbrake: hold it while you steer"; }
			else if (e.done < 0) { sc = 1; why = "Never got turned around and going"; }
			else
			{
				const float late = (e.done - m.done) / 120.0f;
				sc = TimeLevel(late);
				why = std::format("Turned around and going in {:.2f} s (the model: {:.2f})", e.done / 120.0f, m.done / 120.0f);
			}
		}
		else if (step == 0)
		{
			// the jump and the flip
			if (e.act < 0) { sc = 1; why = "No jump"; }
			else if (e.dodge < 0 || (L.moveKind != "wavedash" && e.dodge > e.act + 60)) { sc = 1; why = e.dodge < 0 && L.moveKind == "wavedash" ? "No flip: flip forward as you land" : "No flip after the jump"; }
			else
			{
				const Frame& d = full[e.dodge];
				const float gap = (e.dodge - e.act) / 120.0f, mgap = (m.dodge - m.act) / 120.0f;
				if (L.moveKind == "half_flip" && !stickBack(d)) { sc = 2; why = "Flip backward (stick back), not forward or sideways"; }
				else if (L.moveKind == "speedflip" && !diagonal(d)) { sc = 2; why = "Flip diagonally (stick forward and to the side)"; }
				else if (L.moveKind == "wavedash" && !stickFwd(d)) { sc = 2; why = "Flip forward into the landing"; }
				else if (L.moveKind != "wavedash" && gap > mgap + 0.15f) { sc = gap > mgap + 0.3f ? 3 : 4; why = std::format("Flip sooner after the jump ({:.2f} s; the model: {:.2f})", gap, mgap); }
				else why = L.moveKind == "half_flip" ? "Clean backflip" : L.moveKind == "speedflip" ? "Clean diagonal flip" : "Flipped forward";
				if (L.moveKind == "speedflip")
				{
					int on = 0, all = 0;
					for (int t = std::max(startTick, 0); t < e.dodge; t++) { all++; on += full[t].boost; }
					if (all > 0 && on < all * 0.9f) { sc = std::min(sc, 3); why += std::format(", boost the whole time (on {:.0f}%)", 100.0f * on / all); }
				}
				if (L.moveKind == "wavedash")
				{
					// the tilt: nose up before the landing
					float nose = 0;
					for (int t = std::max(e.act, e.dodge - 20); t <= e.dodge; t++) nose = std::max(nose, full[t].carPitch * (180.0f / 32768.0f));
					if (nose < 5.0f) { sc = std::min(sc, 3); why += std::format(", tilt the nose up before you land ({:.0f} degrees)", nose); }
					else if (nose > 60.0f) { sc = std::min(sc, 3); why += std::format(", too much tilt ({:.0f} degrees)", nose); }
				}
			}
		}
		else
		{
			// the second step: the cancel and landing (half flip, speedflip), or the flip at the landing (wavedash)
			if (e.dodge < 0) { sc = 1; why = "No flip"; }
			else if (L.moveKind == "wavedash")
			{
				if (e.land < 0) { sc = 1; why = "Never landed"; }
				else
				{
					const int gapT = e.land - e.dodge;
					if (gapT > 15) { sc = 2; why = std::format("Flipped {:.2f} s before landing: flip right as the wheels touch", gapT / 120.0f); }
					else
					{
						const float gain = e.done >= 0 ? e.doneSpeed - Spd(full[std::max(0, e.act - 1)]) : 0.0f, mGain = m.doneSpeed - Spd(L.model[std::max(0, m.act - 1)]);
						const float r = mGain > 1.0f ? gain / mGain : 0.0f;
						sc = r >= 0.85f ? 5 : r >= 0.7f ? 4 : r >= 0.5f ? 3 : r > 0.15f ? 2 : 1;
						why = std::format("Gained {:.0f} uu/s (the model: {:.0f})", gain, mGain);
					}
				}
			}
			else
			{
				int fwd = -1;
				const int until = e.land >= 0 ? e.land : std::min(n - 1, e.dodge + 120);
				const auto cancel = L.moveKind == "half_flip" ? stickFwd : stickBack;
				for (int t = std::max(e.dodge + 1, startTick); t <= until; t++) if (cancel(full[t])) { fwd = t; break; }   // (a running start's inputs are the model's)
				const int mFwd = [&] { const int u = m.land; for (int t = m.dodge + 1; t <= u; t++) if (cancel(L.model[t])) return t; return -1; }();
				if (fwd < 0) { sc = 2; why = L.moveKind == "half_flip" ? "No cancel: push the stick forward to stop the backflip" : "No cancel: pull the stick back right after the flip"; }
				else if (const int slack = L.moveKind == "speedflip" ? 12 : 24; mFwd >= 0 && std::abs(fwd - mFwd) > slack) { sc = std::abs(fwd - mFwd) > 2 * slack ? 3 : 4; why = std::format("Cancelled {:.2f} s {} than the model", std::abs(fwd - mFwd) / 120.0f, fwd > mFwd ? "later" : "sooner"); }
				else why = "Cancelled on time";
				if (e.land < 0) { sc = std::min(sc, 2); why += ", but never landed"; }
				else
				{
					const Frame& l = full[e.land];
					const float tilt = std::max(std::fabs(l.carPitch), std::fabs(l.carRoll)) * (180.0f / 32768.0f);
					if (tilt > 30.0f) { sc = std::min(sc, 2); why += std::format(", landed tilted ({:.0f} degrees): roll until the wheels face the floor", tilt); }
					if (L.moveKind == "half_flip" && RadDiff(HeadingRad(l), HeadingRad(L.model[0]) + 3.14159265f) > 0.6f) { sc = std::min(sc, 3); why += ", but not facing back the way you came"; }
				}
			}
		}
		out.stepScores.push_back(sc);
		if (L.steps.size() > 1 || !st.whole) whys.push_back({ sc, std::format("{}: {}", L.steps[step].name, why) });
		else whys.push_back({ sc, why });
	}
	int score = 5;
	for (int s2 : out.stepScores) score = std::min(score, s2);
	if (st.whole && L.moveKind != "powerslide" && L.moveKind != "recovery")
	{
		// the whole move: what it's for (time, or speed gained), with the steps listed
		int overall;
		std::string why;
		if (L.moveKind == "wavedash") { overall = out.stepScores.back(); why = whys.back().text.substr(whys.back().text.find(": ") + 2); }
		else if (e.done < 0) { overall = 1; why = L.moveKind == "speedflip" ? "Never got to supersonic" : "Never got turned around and going"; }
		else
		{
			overall = TimeLevel((e.done - m.done) / 120.0f);
			why = std::format("{} in {:.2f} s (the model: {:.2f})", L.moveKind == "speedflip" ? "Supersonic" : "Turned around and going", e.done / 120.0f, m.done / 120.0f);
		}
		if (out.stepScores.front() <= 2) overall = std::min(overall, 2);   // not this move
		score = overall;
		whys.insert(whys.begin(), { 0, std::format("Overall: {} ({})", overall, why) });
	}
	std::stable_sort(whys.begin(), whys.end(), [](const Why& a, const Why& b) { return a.score < b.score; });
	for (const Why& w : whys) out.lines.push_back(w.text);
	out.score = std::max(1, score);
	return out;
}

// The running start (Nov 2, Coop): step 3 of a speedflip used to drop you in frozen at full speed, just landed and
// pointed off the ball, so you sailed past it; step 2 gave you 8 ticks to jump. Now the model drives the half second
// before the step (never from before the kickoff's own start) and hands over where the step begins.
int Lesson::StageRideFrom(int stage) const
{
	if (stage < 0 || stage >= static_cast<int>(stages.size())) return -1;
	if (kind == LessonKind::Move)
	{
		if (stages[stage].first == 0) return -1;
		if (StageRideByInput(stage)) return 0;   // from the model's own start, so the game does the jump and flip itself
		return std::max(0, StageStart(stage) - 60);   // (Nov 2) the move's later steps too
	}
	if (kind != LessonKind::Kickoff || koSides.empty()) return -1;
	if (KoStepKind(*this, stages[stage].first) == 0) return -1;
	const int start = StageStart(stage);
	return std::max(KoSideFor(*this, start).start, start - 60);
}

// (Nov 3, Coop: step 2 of the half flip "starts you in the middle of the jump, impossible to cancel") Putting the car
// back frame by frame can't start a flip: the game only cancels a flip it's doing itself. So for the half flip and the
// speedflip the running start plays the model's inputs on your car instead, and the game jumps and flips for real.
bool Lesson::StageRideByInput(int stage) const
{
	if (kind != LessonKind::Move || stage < 0 || stage >= static_cast<int>(stages.size()) || stages[stage].first == 0) return false;
	return moveKind == "half_flip" || moveKind == "speedflip";
}

int Lesson::StageWatchEnd(int stage) const
{
	switch (kind)
	{
	case LessonKind::DoubleTap:   return DtWatchEnd(*this, stage);
	case LessonKind::CeilingShot: return CsWatchEnd(*this, stage);
	case LessonKind::FlipReset:   return FrWatchEnd(*this, stage);
	case LessonKind::Kickoff:     return KoWatchEnd(*this, stage);
	case LessonKind::Shot:        return ShotWatchEnd(*this, stage);
	case LessonKind::Move:        return MoveWatchEnd(*this, stage);
	default:                      return AdWatchEnd(*this, stage);
	}
}

bool Lessons::StageDone(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick)
{
	switch (L.kind)
	{
	case LessonKind::DoubleTap:   return DtStageDone(L, stage, full, startTick);
	case LessonKind::CeilingShot: return CsStageDone(L, stage, full, startTick);
	case LessonKind::FlipReset:   return FrStageDone(L, stage, full, startTick);
	case LessonKind::Kickoff:     return KoStageDone(L, stage, full, startTick);
	case LessonKind::Shot:        return false;   // every attempt plays on to the finish
	case LessonKind::Move:        return MoveStageDone(L, stage, full, startTick);
	default:                      return AdStageDone(L, stage, full, startTick);
	}
}

StageScore Lessons::Score(const Lesson& L, int stage, const std::vector<Frame>& full, int startTick, bool wentIn, bool floorFirst,
	const ShotReference& ref, bool compareToModel)
{
	switch (L.kind)
	{
	case LessonKind::DoubleTap:   return DtScore(L, stage, full, startTick, wentIn, floorFirst, ref, compareToModel);
	case LessonKind::CeilingShot: return CsScore(L, stage, full, startTick, wentIn, floorFirst, ref, compareToModel);
	case LessonKind::FlipReset:   return FrScore(L, stage, full, startTick, wentIn, floorFirst, ref, compareToModel);
	case LessonKind::Kickoff:     return KoScore(L, stage, full, startTick);
	case LessonKind::Shot:        return ShotScore(L, stage, full, startTick, wentIn, floorFirst, compareToModel);
	case LessonKind::Move:        return MoveScore(L, stage, full, startTick);
	default:                      return AdScore(L, stage, full, startTick, wentIn, floorFirst, ref, compareToModel);
	}
}

Lesson Lessons::Build(const std::string& id, const std::vector<Frame>& model)
{
	if (id == "air_dribble") return AirDribble(model);
	if (id == "double_tap") return DoubleTap(model);
	if (id == "ceiling_shot") return CeilingShot(model);
	if (id == "flip_reset") return FlipReset(model);
	if (id.rfind("kickoff_", 0) == 0) return Kickoff(id, model);
	if (id == "aerial" || id == "power_shot" || id == "dribble" || id == "front_flick" || id == "fast_aerial" || id == "off_the_wall" || id == "wall_hit" || id == "backboard_read") return Shot(id, model);
	if (id == "half_flip" || id == "speedflip" || id == "wavedash" || id == "powerslide_turn" || id == "recovery" || id == "air_roll" || id == "tornado_spin") return Move(id, model);
	return Lesson{};
}

const std::vector<LessonInfo>& Lessons::Catalog()
{
	// The skill tree (Oct 28 map; tiers 1-2 built Nov 2), tier by tier. Order is the tree's reading order: roots first,
	// then what they open. A prerequisite that isn't built yet doesn't lock anything (RLCoachLearn.cpp).
	static const std::vector<LessonInfo> c = {
		// Tier 1: foundations (each can be tested out of)
		{ "aerial", "Aerial", "Jump, boost and meet the ball in the air.", {}, true, 1 },
		{ "power_shot", "Power shot", "Boost in and flip through the ball for the hardest hit.", {}, true, 1 },
		{ "dribble", "Dribble", "Carry the ball on your roof, then flick it in.", {}, true, 1 },
		{ "half_flip", "Half flip", "Backflip, cancel, roll upright: the fastest way to turn around.", {}, true, 1 },
		{ "powerslide_turn", "Powerslide turn", "A handbrake turn that keeps your speed.", {}, true, 1 },
		{ "recovery", "Recovery", "Land on your wheels after a hit, pointed where you're going.", {}, true, 1 },
		{ "air_roll", "Air roll control", "Point the car any way you want in the air.", { "aerial" }, true, 1 },
		{ "wall_hit", "Wall hit", "Drive up the wall and hit the ball off it.", {}, false, 1 },
		// Tier 2: core
		{ "fast_aerial", "Fast aerial", "Jump, jump again straight away, boost: first to the high balls.", { "aerial" }, true, 2 },
		{ "speedflip", "Speedflip", "A cancelled diagonal flip: the quickest way up to full speed.", { "half_flip" }, true, 2 },
		{ "wavedash", "Wavedash", "A flip just as your wheels land, for free speed.", { "recovery" }, true, 2 },
		{ "front_flick", "Front flick", "Pop the ball off your roof with a front flip.", { "dribble" }, true, 2 },
		{ "ground_catch", "Ground catch", "Cushion a falling ball onto your car.", { "dribble" }, false, 2 },
		{ "tornado_spin", "Tornado spin", "Air roll one way, stick the other, nose up: spin and hover in the air.", { "air_roll" }, true, 2 },
		{ "off_the_wall", "Off the wall", "Jump off the wall at the ball and hit it.", { "wall_hit", "fast_aerial" }, true, 2 },
		{ "backboard_read", "Backboard read", "Meet a ball coming off the backboard.", { "fast_aerial" }, true, 2 },
		// Tier 3: advanced
		{ "air_dribble", "Air dribble", "Off the wall and carried in the air, then the finish.", { "off_the_wall", "air_roll" }, true, 3 },
		{ "double_tap", "Double tap", "Into the backboard, read the bounce, second touch in.", { "off_the_wall", "backboard_read" }, true, 3 },
		{ "ceiling_shot", "Ceiling shot", "Onto the ceiling, fall off with your flip, shoot.", { "off_the_wall" }, true, 3 },
		// Tier 4: expert
		{ "flip_reset", "Flip reset", "Get your flip back off the ball and use it on the shot.", { "air_dribble" }, true, 4 },
		{ "air_dribble_double_tap", "Air dribble double tap", "Carry it into the backboard, then the second touch.", { "air_dribble", "double_tap" }, false, 4 },
		{ "ceiling_double_tap", "Ceiling double tap", "Off the ceiling into the backboard, then the second touch.", { "ceiling_shot", "double_tap" }, false, 4 },
		// Tier 5: master
		{ "ceiling_flip_reset", "Ceiling flip reset", "Off the ceiling into a reset, then the shot.", { "ceiling_shot", "flip_reset" }, false, 5 },
		{ "flip_reset_double_tap", "Flip reset double tap", "Reset, flip it into the backboard, then the second touch.", { "flip_reset", "double_tap" }, false, 5 },
		// Kickoffs (Nov 1): the flip-in first, then the speedflip from each spawn
		{ "kickoff_flipin", "Kickoff: boost and flip in", "Boost on go, straight at the ball, flip into it. The kickoff most players start with.", {}, true, 0 },
		{ "kickoff_diagonal", "Kickoff: diagonal speedflip", "The fastest kickoff from the corner spawns: jump, diagonal flip, cancel, flip in. SSL players use it 97% of the time.", { "kickoff_flipin" }, true, 0 },
		{ "kickoff_offcentre", "Kickoff: off-centre speedflip", "The speedflip from the spawns beside the goal.", { "kickoff_diagonal" }, true, 0 },
		{ "kickoff_back", "Kickoff: back centre speedflip", "The speedflip from the spawn in front of your goal: the longest run.", { "kickoff_diagonal" }, true, 0 },
	};
	return c;
}

const LessonInfo* Lessons::Find(const std::string& id)
{
	for (const LessonInfo& i : Catalog()) if (i.id == id) return &i;
	return nullptr;
}

// ---- Progress ----

void LessonProgress::Fit(int stages)
{
	best.resize(stages, 0);
	recentFull.resize(stages);
	unlocked = std::clamp(unlocked, 0, std::max(0, stages - 1));
}

int LessonProgress::PassedRecent(int stage) const
{
	if (stage < 0 || stage >= static_cast<int>(recentFull.size())) return 0;
	int n = 0;
	for (int s : recentFull[stage]) n += s >= 4;
	return n;
}

LessonProgress::Change LessonProgress::Record(int stage, int score, float speed, bool autoRamp)
{
	Change c;
	c.speed = speed;
	if (stage < 0 || stage >= static_cast<int>(best.size()) || score <= 0) return c;
	const bool full = speed >= 0.999f;
	if (full) best[stage] = std::max(best[stage], score);

	if (autoRamp && !full)
	{
		rampStreak = score >= 4 ? rampStreak + 1 : 0;
		if (rampStreak >= kRampNeed)
		{
			for (float r : kRamp) if (r > speed + 0.001f) { c.speed = r; break; }
			c.rampedUp = true;
			rampStreak = 0;
		}
	}
	if (full)
	{
		auto& q = recentFull[stage];
		q.push_back(score);
		while (static_cast<int>(q.size()) > kUnlockOf) q.pop_front();
		if (stage == unlocked && PassedRecent(stage) >= kUnlockNeed)
		{
			if (unlocked + 1 < static_cast<int>(best.size())) { unlocked++; c.unlockedNext = true; }
			else if (!completed) { completed = true; c.completed = true; }   // the last stage: the lesson's done
		}
	}
	return c;
}

std::string LessonProgress::Save(const std::string& id) const
{
	std::string s = std::format("{},unlocked,{}\n{},completed,{}\n{},testout,{}\n", id, unlocked, id, completed ? 1 : 0, id, testOut);
	for (size_t i = 0; i < best.size(); i++)
	{
		std::string r;
		for (int v : recentFull[i]) r += (r.empty() ? "" : ";") + std::to_string(v);
		s += std::format("{},stage,{},{},{}\n", id, i, best[i], r);
	}
	return s;
}

void LessonProgress::Load(const std::string& id, const std::string& csv)
{
	std::istringstream in(csv);
	std::string line;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::vector<std::string> p;
		std::stringstream ls(line);
		std::string cell;
		while (std::getline(ls, cell, ',')) p.push_back(cell);
		if (p.size() < 3 || p[0] != id) continue;
		if (p[1] == "unlocked") unlocked = std::atoi(p[2].c_str());
		else if (p[1] == "completed") completed = std::atoi(p[2].c_str()) != 0;
		else if (p[1] == "testout") testOut = std::atoi(p[2].c_str());
		else if (p[1] == "stage" && p.size() >= 4)
		{
			int i = std::atoi(p[2].c_str());
			if (i < 0 || i >= static_cast<int>(best.size())) continue;
			best[i] = std::atoi(p[3].c_str());
			recentFull[i].clear();
			if (p.size() >= 5)
			{
				std::stringstream rs(p[4]);
				std::string v;
				while (std::getline(rs, v, ';')) if (!v.empty()) recentFull[i].push_back(std::atoi(v.c_str()));
			}
		}
	}
	Fit(static_cast<int>(best.size()));
	// Passed the last stage before completing it was tracked (Oct 27)
	const int last = static_cast<int>(best.size()) - 1;
	if (last >= 0 && unlocked == last && PassedRecent(last) >= kUnlockNeed) completed = true;
}
