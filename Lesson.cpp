// No pch on purpose: plain C++ (see Lesson.h)
#include "Lesson.h"
#include "Grader.h"
#include "Segmenter.h"

#include <algorithm>
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

int Lesson::StageWatchEnd(int stage) const
{
	switch (kind)
	{
	case LessonKind::DoubleTap:   return DtWatchEnd(*this, stage);
	case LessonKind::CeilingShot: return CsWatchEnd(*this, stage);
	case LessonKind::FlipReset:   return FrWatchEnd(*this, stage);
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
	default:                      return AdScore(L, stage, full, startTick, wentIn, floorFirst, ref, compareToModel);
	}
}

Lesson Lessons::Build(const std::string& id, const std::vector<Frame>& model)
{
	if (id == "air_dribble") return AirDribble(model);
	if (id == "double_tap") return DoubleTap(model);
	if (id == "ceiling_shot") return CeilingShot(model);
	if (id == "flip_reset") return FlipReset(model);
	return Lesson{};
}

const std::vector<LessonInfo>& Lessons::Catalog()
{
	// Order is the tree's left-to-right reading order: roots first, then what they open
	static const std::vector<LessonInfo> c = {
		{ "air_dribble", "Air dribble", "Off the wall and carried in the air, then the finish.", {}, true },
		{ "double_tap", "Double tap", "Into the backboard, read the bounce, second touch in.", {}, true },
		{ "ceiling_shot", "Ceiling shot", "Onto the ceiling, fall off with your flip, shoot.", {}, true },
		{ "flip_reset", "Flip reset", "Get your flip back off the ball and use it on the shot.", { "air_dribble" }, true },
		{ "air_dribble_double_tap", "Air dribble double tap", "Carry it into the backboard, then the second touch.", { "air_dribble", "double_tap" }, false },
		{ "ceiling_double_tap", "Ceiling double tap", "Off the ceiling into the backboard, then the second touch.", { "ceiling_shot", "double_tap" }, false },
		{ "ceiling_flip_reset", "Ceiling flip reset", "Off the ceiling into a reset, then the shot.", { "ceiling_shot", "flip_reset" }, false },
		{ "flip_reset_double_tap", "Flip reset double tap", "Reset, flip it into the backboard, then the second touch.", { "flip_reset", "double_tap" }, false },
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
	std::string s = std::format("{},unlocked,{}\n{},completed,{}\n", id, unlocked, id, completed ? 1 : 0);
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
