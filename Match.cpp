// No pch on purpose: plain C++.
#include "Match.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace
{
	// One comma-separated line, cut in place (no copies of the cells)
	struct Cells
	{
		const char* a[32];
		const char* b[32];
		int n = 0;
		Cells(const char* p, const char* end)
		{
			const char* s = p;
			for (const char* q = p; ; q++)
				if (q == end || *q == ',')
				{
					if (n < 32) { a[n] = s; b[n] = q; n++; }
					s = q + 1;
					if (q == end) break;
				}
		}
		float F(int i) const
		{
			if (i >= n) return 0.0f;
			float v = 0.0f;
			auto r = std::from_chars(a[i], b[i], v);
			if (r.ec != std::errc()) v = std::strtof(std::string(a[i], b[i]).c_str(), nullptr);
			return std::isfinite(v) ? v : 0.0f;
		}
		int I(int i) const
		{
			if (i >= n) return 0;
			int v = 0;
			auto r = std::from_chars(a[i], b[i], v);
			if (r.ec != std::errc()) v = static_cast<int>(F(i));
			return v;
		}
		std::string S(int i) const { return i < n ? std::string(a[i], b[i]) : std::string(); }
	};

	float Sq(float v) { return v * v; }
	float Dist(float ax, float ay, float az, float bx, float by, float bz) { return std::sqrt(Sq(ax - bx) + Sq(ay - by) + Sq(az - bz)); }
	// +1 for blue, -1 for orange: multiply a Y by this and "forward" is always up the pitch for that team
	float Dir(int team) { return team == 0 ? 1.0f : -1.0f; }
}

const char* Match::KindName(MomentKind k)
{
	switch (k)
	{
	case MomentKind::Kickoff: return "Kickoff";
	case MomentKind::Goal: return "Goal";
	case MomentKind::Shot: return "Shot";
	case MomentKind::Save: return "Save";
	case MomentKind::Assist: return "Assist";
	case MomentKind::Fifty: return "50/50";
	case MomentKind::Pass: return "Pass";
	case MomentKind::Clear: return "Clear";
	case MomentKind::Giveaway: return "Giveaway";
	case MomentKind::Demo: return "Demo";
	case MomentKind::NoBoost: return "No boost";
	case MomentKind::DoubleCommit: return "Double commit";
	case MomentKind::Beaten: return "Beaten";
	case MomentKind::OpenNet: return "Open net";
	case MomentKind::LeftForBoost: return "Left for boost";
	case MomentKind::TookMateBoost: return "Took a teammate's boost";
	case MomentKind::Aerial: return "Aerial";
	case MomentKind::NoFollowUp: return "No follow-up";
	case MomentKind::Shadow: return "Shadow";
	case MomentKind::CutRotation: return "Cut rotation";
	case MomentKind::BallSide: return "Ball side";
	case MomentKind::Play: return "Play";
	default: return "Wasted boost";
	}
}

bool Match::Load(const std::filesystem::path& file, MatchData& d, std::string& err)
{
	d = MatchData{};
	std::ifstream in(file, std::ios::binary);
	if (!in) { err = "Couldn't open the match file"; return false; }
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string text = ss.str();
	const char* p = text.data();
	const char* const end = p + text.size();
	MatchFrame* cur = nullptr;
	while (p < end)
	{
		const char* le = p;
		while (le < end && *le != '\n') le++;
		const char* b = le;
		while (b > p && (b[-1] == '\r' || b[-1] == '\n')) b--;
		if (b - p >= 2)
		{
			if (*p == '#')
			{
				const std::string line(p, b);
				if (line.rfind("# replay,", 0) == 0) d.replayName = line.substr(9);
				else if (line.rfind("# file,", 0) == 0) d.replayFile = line.substr(7);
				else if (line.rfind("# me,", 0) == 0) d.recordedBy = line.substr(5);
				else if (line.rfind("# frames,", 0) == 0) d.replayFrames = std::atoi(line.c_str() + 9);
				else if (line.rfind("# fps,", 0) == 0) d.fps = std::max(1, std::atoi(line.c_str() + 6));
			}
			else if (p[1] == ',')
			{
				const Cells c(p + 2, b);
				switch (*p)
				{
				case 'P':
				{
					MatchPlayer pl;
					pl.id = c.I(0);
					pl.team = c.I(1);
					pl.name = c.S(2);
					if (pl.id == static_cast<int>(d.players.size()) && pl.id < 16) d.players.push_back(pl);
					break;
				}
				case 'G':
				{
					MatchFrame f;
					f.frame = c.I(0);
					f.time = c.F(1);
					f.clock = c.I(2);
					f.overtime = c.I(3) != 0;
					f.score[0] = c.I(4);
					f.score[1] = c.I(5);
					d.frames.push_back(std::move(f));
					cur = &d.frames.back();
					break;
				}
				case 'B':
					if (cur && c.I(0) == cur->frame)
					{
						cur->hasBall = true;
						cur->bx = c.F(1); cur->by = c.F(2); cur->bz = c.F(3);
						cur->bvx = c.F(4); cur->bvy = c.F(5); cur->bvz = c.F(6);
					}
					break;
				case 'C':
					if (cur && c.I(0) == cur->frame)
					{
						const int id = c.I(1);
						if (id < 0 || id >= 16) break;
						if (static_cast<int>(cur->cars.size()) <= id) cur->cars.resize(static_cast<size_t>(id) + 1);
						MatchCar& car = cur->cars[id];
						car.present = true;
						car.x = c.F(2); car.y = c.F(3); car.z = c.F(4);
						car.pitch = c.I(5); car.yaw = c.I(6); car.roll = c.I(7);
						car.vx = c.F(8); car.vy = c.F(9); car.vz = c.F(10);
						car.avx = c.F(11); car.avy = c.F(12); car.avz = c.F(13);
						car.boost = c.F(14);
						car.onGround = c.I(15) != 0;
						car.supersonic = c.I(16) != 0;
						car.jumped = c.I(17) != 0;
						car.doubleJumped = c.I(18) != 0;
						car.throttle = (c.I(19) - 128) / 127.0f;   // the replay keeps these as 0-255, 128 in the middle
						car.steer = (c.I(20) - 128) / 127.0f;
						car.handbrake = c.I(21) != 0;
						if (c.n > 23) { car.onWall = c.I(22); car.hasFlip = c.I(23); }
					}
					break;
				case 'T':
					d.touches.push_back({ c.I(0), c.I(1) });
					break;
				case 'S':
					d.counters.push_back({ c.I(0), c.I(1), c.I(2), c.I(3), c.I(4), c.I(5), c.I(6), c.I(7) });
					break;
				default: break;
				}
			}
		}
		p = le < end ? le + 1 : end;
	}
	for (MatchFrame& f : d.frames) f.cars.resize(d.players.size());
	if (d.frames.size() < 60 || d.players.empty()) { err = "The match file is empty or cut short"; return false; }
	return true;
}

namespace
{
	// On the floor, a wall or the ceiling, from where the car is (Nov 13). The match review uses this rather than the
	// game's own wheels-down flag, because replay files don't carry that flag and the rank baselines come from replay
	// files: on the same game the flag put cars in the air about 5 points less of the time, which made in-game scans
	// read about half a rank lower. (The shot breakdown still uses the game's flag, where it has it.)
	bool OnSurface(const MatchCar& c)
	{
		const float x = std::fabs(c.x), y = std::fabs(c.y);
		return c.z < 30.0f || c.z > 2010.0f || x > 4060.0f || (y > 5085.0f && !(x < 893.0f && c.z < 642.0f)) || x + y > 7850.0f;
	}
	std::string Lower(std::string x) { for (char& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return x; }
	std::string Lower1(std::string x) { if (!x.empty()) x[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(x[0]))); return x; }
#include "MatchModels.inc"
	// (the Play label cut-offs and the touch grade cut-offs are generated too: kPlay*, kTouchCut)

	// Where a value sits among players, 0..100, from the five percentiles a baseline keeps (10th, 25th, 50th, 75th,
	// 90th): straight lines between them, carried on past the ends, and the middle of a run of equal ones.
	// tools/match/gen.py has the same function; the two have to stay the same.
	float PctOf(const float* q, float v)
	{
		static const float P[5] = { 10, 25, 50, 75, 90 };
		int lo = -1, hi = 5;
		for (int k = 0; k < 5; k++) if (q[k] < v) lo = k;
		for (int k = 4; k >= 0; k--) if (q[k] > v) hi = k;
		if (hi - lo > 1)   // v equals one or more of them
		{
			float sum = 0;
			for (int k = lo + 1; k < hi; k++) sum += P[k];
			return sum / (hi - lo - 1);
		}
		if (lo < 0) return q[1] > q[0] ? std::max(1.0f, 10.0f - (q[0] - v) * 15.0f / (q[1] - q[0])) : 5.0f;
		if (hi > 4) return q[4] > q[3] ? std::min(99.0f, 90.0f + (v - q[4]) * 15.0f / (q[4] - q[3])) : 95.0f;
		return P[lo] + (P[hi] - P[lo]) * (v - q[lo]) / (q[hi] - q[lo]);
	}
}

const char* Match::LevelName(int level)
{
	static const char* names[] = { "Gold", "Platinum", "Diamond", "Champion", "Grand Champion", "Supersonic Legend" };
	return names[std::clamp(level, 0, 5)];
}
const char* Match::GradeName(int g)
{
	static const char* n[] = { "Calculated", "Clean", "Solid", "Sloppy", "Costly", "Throw" };
	return g >= 0 && g < ReviewGrade::Count ? n[g] : "";
}
const char* Match::GradeMark(int g)
{
	static const char* n[] = { "!!", "!", "", "?!", "?", "??" };
	return g >= 0 && g < ReviewGrade::Count ? n[g] : "";
}
const char* Match::AreaName(int area)
{
	static const char* n[] = { "Attack", "Defence", "Ball use", "Positioning", "Boost", "Speed" };
	return area >= 0 && area < Area_Count ? n[area] : "";
}

float Match::LevelOf(int teamSize, int stat, float raw)
{
	// straight lines between each rank's middle value (kept rising, in case two ranks cross)
	float med[6];
	for (int r = 0; r < 6; r++) med[r] = std::max(r > 0 ? med[r - 1] + 1e-3f : -1e9f, Baseline(teamSize, r, stat)[2]);
	if (raw <= med[0]) return 0.0f;
	for (int r = 1; r < 6; r++)
		if (raw <= med[r]) return r - 1 + (raw - med[r - 1]) / (med[r] - med[r - 1]);
	return 5.0f;
}
int Match::QuarterOf(int teamSize, int level, int stat, float value)
{
	const float* b = Baseline(teamSize, level, stat);
	return value <= b[1] ? 0 : value < b[2] ? 1 : value < b[3] ? 2 : 3;
}

float Match::StatValue(const MatchPlayerStats& s, int stat, float seconds) { return BaseValue(s, stat, seconds); }
const float* Match::Baseline(int teamSize, int level, int stat)
{
	return kBase[std::clamp(teamSize, 1, 3) - 1][std::clamp(level, 0, 5)][std::clamp(stat, 0, static_cast<int>(Base_Count) - 1)];
}
int Match::StatBetter(int stat) { return stat >= 0 && stat < Base_Count ? kBaseBetter[stat] : 0; }
bool Match::StatPerFive(int stat) { return stat >= 0 && stat < Base_Count && kBasePer5[stat]; }

float Match::Chance(const float f[kChanceFeatures])
{
	float h1[kChH1], h2[kChH2];
	for (int j = 0; j < kChH1; j++)
	{
		float v = kChB1[j];
		for (int i = 0; i < kChanceFeatures; i++) v += kChW1[j][i] * f[i];
		h1[j] = v > 0.0f ? v : 0.0f;
	}
	for (int j = 0; j < kChH2; j++)
	{
		float v = kChB2[j];
		for (int i = 0; i < kChH1; i++) v += kChW2[j][i] * h1[i];
		h2[j] = v > 0.0f ? v : 0.0f;
	}
	float v = kChB3;
	for (int i = 0; i < kChH2; i++) v += kChW3[i] * h2[i];
	return 1.0f / (1.0f + std::exp(-v));
}

bool Match::ChanceFeatures(const MatchData& d, int index, int team, int possTeam, float out[kChanceFeatures], int skip)
{
	for (int k = 0; k < kChanceFeatures; k++) out[k] = 0.0f;
	if (index < 0 || index >= static_cast<int>(d.frames.size()) || (team != 0 && team != 1)) return false;
	const MatchFrame& f = d.frames[index];
	if (!f.hasBall) return false;
	const float fwd = Dir(team), side = f.bx >= 0.0f ? 1.0f : -1.0f;
	const float X = f.bx * side, Y = f.by * fwd, Z = f.bz;
	const float toGoal = std::sqrt(Sq(X) + Sq(5120.0f - Y));
	// how wide the goal mouth looks from the ball (the posts are 893 either side of the middle)
	const float angle = std::fabs(std::atan2(893.0f - X, std::max(1.0f, 5120.0f - Y)) - std::atan2(-893.0f - X, std::max(1.0f, 5120.0f - Y)));
	float a1 = 9000.0f, a2 = 9000.0f, a1Closing = 0, a1Boost = 0, a1Behind = 0;
	float d1 = 9000.0f, d1Closing = 0, d1Boost = 0, minDefGoal = 12000.0f;
	int attackers = 0, defenders = 0, covering = 0;
	const int P = static_cast<int>(std::min(d.players.size(), f.cars.size()));
	for (int pl = 0; pl < P; pl++)
	{
		const MatchCar& c = f.cars[pl];
		const int tm = d.players[pl].team;
		if (!c.present || (tm != 0 && tm != 1) || pl == skip) continue;
		const float dx = f.bx - c.x, dy = f.by - c.y, dz = f.bz - c.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float closing = dist > 1.0f ? (c.vx * dx + c.vy * dy + c.vz * dz) / dist : 0.0f;
		if (tm == team)
		{
			attackers++;
			if (dist < a1) { a2 = a1; a1 = dist; a1Closing = closing; a1Boost = c.boost; a1Behind = c.y * fwd < Y ? 1.0f : 0.0f; }
			else if (dist < a2) a2 = dist;
		}
		else
		{
			defenders++;
			if (dist < d1) { d1 = dist; d1Closing = closing; d1Boost = c.boost; }
			const float goalDist = std::sqrt(Sq(c.x) + Sq(5120.0f - c.y * fwd));
			minDefGoal = std::min(minDefGoal, goalDist);
			if (goalDist < toGoal) covering++;
		}
	}
	auto cap = [](float v, float hi) { return std::clamp(v, -hi, hi); };
	out[0] = X / 4096.0f;
	out[1] = Y / 5120.0f;
	out[2] = Z / 2000.0f;
	out[3] = f.bvx * side / 2000.0f;
	out[4] = f.bvy * fwd / 2000.0f;
	out[5] = f.bvz / 2000.0f;
	out[6] = cap(toGoal / 6000.0f, 2.0f);
	out[7] = angle;
	out[8] = cap(a1 / 3000.0f, 3.0f);
	out[9] = cap(a1Closing / 2300.0f, 1.5f);
	out[10] = a1Boost / 100.0f;
	out[11] = a1Behind;
	out[12] = cap(a2 / 3000.0f, 3.0f);
	out[13] = cap(d1 / 3000.0f, 3.0f);
	out[14] = cap(d1Closing / 2300.0f, 1.5f);
	out[15] = d1Boost / 100.0f;
	out[16] = defenders > 0 ? static_cast<float>(covering) / defenders : 0.0f;
	out[17] = cap(minDefGoal / 6000.0f, 2.0f);
	out[18] = possTeam == team ? 1.0f : possTeam == 1 - team ? 0.0f : 0.5f;
	out[19] = static_cast<float>(std::clamp(std::max(attackers, defenders), 1, 3) - 2);
	out[20] = static_cast<float>(attackers - defenders);
	return true;
}

MatchAnalysis Match::Analyze(const MatchData& d)
{
	MatchAnalysis a;
	a.replayName = d.replayName;
	a.replayFile = d.replayFile;
	a.recordedBy = d.recordedBy;
	a.players = d.players;
	const int P = static_cast<int>(d.players.size()), N = static_cast<int>(d.frames.size());
	if (N < 60 || P == 0) { a.error = "Nothing to break down"; return a; }
	a.stats.assign(P, {});
	a.passTo.assign(P, std::vector<int>(P, 0));
	// How long one replay frame really is. The replay says "30 a second", but the frames come about every 0.046 s
	// (measured on the first scan), so it's taken from the frames' own times: the middle one of all the steps.
	float dt = 1.0f / std::max(1, d.fps);
	{
		std::vector<float> steps;
		steps.reserve(N);
		for (int i = 1; i < N; i++)
		{
			const int df = d.frames[i].frame - d.frames[i - 1].frame;
			const float t = d.frames[i].time - d.frames[i - 1].time;
			if (df > 0 && t > 0.0f && t / df < 0.2f) steps.push_back(t / df);
		}
		if (steps.size() > 30)
		{
			std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
			dt = steps[steps.size() / 2];
		}
	}
	const int fps = std::max(1, static_cast<int>(std::lround(1.0f / dt)));   // replay frames in a second
	auto team = [&](int pl) { return pl >= 0 && pl < P ? d.players[pl].team : -1; };
	auto name = [&](int pl) { return pl >= 0 && pl < P ? d.players[pl].name : std::string("?"); };
	// the place in `frames` of a replay frame number (frames can be skipped, so search)
	auto indexOf = [&](int frame) {
		auto it = std::lower_bound(d.frames.begin(), d.frames.end(), frame, [](const MatchFrame& f, int v) { return f.frame < v; });
		return std::clamp(static_cast<int>(it - d.frames.begin()), 0, N - 1);
	};
	auto addMoment = [&](MomentKind k, int idx, int tm, int pl, int other, std::string text) {
		Moment m;
		m.kind = k;
		m.index = idx;
		m.frame = d.frames[idx].frame;
		m.time = d.frames[idx].time;
		m.clock = d.frames[idx].clock;
		m.team = tm;
		m.player = pl;
		m.other = other;
		m.text = std::move(text);
		a.moments.push_back(std::move(m));
	};
	int teamCount[2] = { 0, 0 };
	for (const MatchPlayer& pl : d.players) if (pl.team == 0 || pl.team == 1) teamCount[pl.team]++;
	a.teamSize = std::max(teamCount[0], teamCount[1]);
	a.score[0] = d.frames.back().score[0];
	a.score[1] = d.frames.back().score[1];
	a.t0 = d.frames.front().time;
	a.t1 = d.frames.back().time;
	a.frame0 = d.frames.front().frame;
	a.frame1 = d.frames.back().frame;

	// ---- When the ball is live: not while it sits on the centre spot before a kickoff, and not between a goal and
	// the next kickoff ----
	std::vector<char> live(N, 1);
	std::vector<int> kickoffStart;   // index where each kickoff wait ends (the ball first moves)
	{
		int waitFrom = -1;
		for (int i = 0; i < N; i++)
		{
			const MatchFrame& f = d.frames[i];
			const bool still = f.hasBall && std::fabs(f.bx) < 2.0f && std::fabs(f.by) < 2.0f && std::fabs(f.bvx) < 2.0f && std::fabs(f.bvy) < 2.0f;
			if (still) { if (waitFrom < 0) waitFrom = i; }
			else if (waitFrom >= 0)
			{
				if (i - waitFrom >= 15) { for (int k = waitFrom; k < i; k++) live[k] = 0; kickoffStart.push_back(i); }
				waitFrom = -1;
			}
		}
		// after each goal, nothing counts until the next kickoff
		for (int i = 1; i < N; i++)
			if (d.frames[i].score[0] != d.frames[i - 1].score[0] || d.frames[i].score[1] != d.frames[i - 1].score[1])
			{
				auto next = std::lower_bound(kickoffStart.begin(), kickoffStart.end(), i);
				const int until = next == kickoffStart.end() ? N : *next;
				for (int k = i; k < until; k++) live[k] = 0;
			}
	}

	// ---- Touches: the game's own reports, repeats within a few frames folded into one ----
	struct Touch { int idx = 0, player = 0, team = 0; bool fifty = false; int fiftyWith = -1; };
	std::vector<Touch> touches;
	{
		std::vector<MatchData::Touch> raw = d.touches;
		if (raw.empty())   // the game didn't report touches in this replay: use each player's touch counter going up
		{
			std::vector<int> last(P, 0);
			for (const MatchData::Counter& c : d.counters)
				if (c.player >= 0 && c.player < P && c.touches > last[c.player]) { raw.push_back({ c.frame, c.player }); last[c.player] = c.touches; }
		}
		std::stable_sort(raw.begin(), raw.end(), [](const MatchData::Touch& x, const MatchData::Touch& y) { return x.frame < y.frame; });
		std::vector<int> lastFrame(P, -1000);
		for (const MatchData::Touch& t : raw)
		{
			if (t.player < 0 || t.player >= P) continue;
			if (t.frame - lastFrame[t.player] <= 3) { lastFrame[t.player] = t.frame; continue; }
			lastFrame[t.player] = t.frame;
			touches.push_back({ indexOf(t.frame), t.player, team(t.player) });
		}
		for (const Touch& t : touches) a.stats[t.player].touches++;
	}
	// the ball's speed up the pitch (blue's way) a little after index i
	auto ballVyAfter = [&](int i, int frames) { return d.frames[std::min(N - 1, i + frames)].bvy; };

	// ---- What the game counted: shots, saves, goals, assists ----
	struct Counted { int idx, player; MomentKind kind; };
	std::vector<Counted> counted;
	{
		std::vector<MatchData::Counter> last(P);
		for (const MatchData::Counter& c : d.counters)
		{
			if (c.player < 0 || c.player >= P) continue;
			const MatchData::Counter& was = last[c.player];
			const int idx = indexOf(c.frame);
			if (c.shots > was.shots) counted.push_back({ idx, c.player, MomentKind::Shot });
			if (c.saves > was.saves) counted.push_back({ idx, c.player, MomentKind::Save });
			if (c.goals > was.goals) counted.push_back({ idx, c.player, MomentKind::Goal });
			if (c.assists > was.assists) counted.push_back({ idx, c.player, MomentKind::Assist });
			last[c.player] = c;
			MatchPlayerStats& s = a.stats[c.player];
			s.shots = c.shots; s.saves = c.saves; s.goals = c.goals; s.assists = c.assists; s.score = c.score;
		}
	}
	auto countedNear = [&](int idx, int player, MomentKind k, int within) {
		for (const Counted& c : counted) if (c.kind == k && c.player == player && std::abs(c.idx - idx) <= within) return true;
		return false;
	};
	for (const Counted& c : counted)
	{
		const char* verb = c.kind == MomentKind::Shot ? "shot on goal" : c.kind == MomentKind::Save ? "save" : c.kind == MomentKind::Goal ? "scored" : "assist";
		addMoment(c.kind, c.idx, team(c.player), c.player, -1, c.kind == MomentKind::Goal ? name(c.player) + " scored" : std::format("{}: {}", name(c.player), verb));
	}

	// ---- Kickoffs: who came out of it with the ball going their way (two seconds on) ----
	for (int ks : kickoffStart)
	{
		const int later = std::min(N - 1, ks + 2 * fps);
		if (!live[later]) continue;   // a goal straight off the kickoff: the goal says it
		const float by = d.frames[later].by;
		const int winner = by > 700.0f ? 0 : by < -700.0f ? 1 : -1;
		int first = -1;
		for (const Touch& t : touches) if (t.idx >= ks - 2 && t.idx <= ks + fps) { first = t.player; break; }
		a.kickoffs++;
		if (winner >= 0) a.kickoffsWon[winner]++;
		for (const Touch& t : touches) if (t.idx >= ks - 2 && t.idx <= ks + fps) a.stats[t.player].kickoffsTaken++;
		addMoment(MomentKind::Kickoff, ks, winner, winner >= 0 && team(first) == winner ? first : -1, -1,
			winner < 0 ? "Kickoff: even, the ball stayed in the middle" : std::format("Kickoff: {} came out ahead", winner == 0 ? "blue" : "orange"));
	}

	// ---- 50/50s: two cars from opposite teams on the ball within a quarter of a second ----
	for (size_t i = 0; i + 1 < touches.size(); i++)
	{
		Touch& x = touches[i];
		Touch& y = touches[i + 1];
		if (x.fifty || x.team == y.team || y.idx - x.idx > fps / 4 + 1 || !live[x.idx]) continue;
		bool kick = false;
		for (int ks : kickoffStart) if (std::abs(ks - x.idx) <= fps) kick = true;
		x.fifty = y.fifty = true;
		x.fiftyWith = y.player;
		y.fiftyWith = x.player;
		if (kick) continue;   // the kickoff moment covers it
		const float vy = ballVyAfter(y.idx, fps / 2);
		const int winner = vy > 350.0f ? 0 : vy < -350.0f ? 1 : -1;
		a.stats[x.player].fifties++;
		a.stats[y.player].fifties++;
		const int wp = winner < 0 ? -1 : (x.team == winner ? x.player : y.player), lp = wp == x.player ? y.player : x.player;
		if (wp >= 0) a.stats[wp].fiftiesWon++;
		for (const Touch* t : { &x, &y })
		{
			const MatchCar& c = d.frames[t->idx].cars[t->player];
			if (c.present && c.boost < 12.0f) a.stats[t->player].lowBoostFifties++;
		}
		addMoment(MomentKind::Fifty, x.idx, winner, wp >= 0 ? wp : x.player, wp >= 0 ? lp : y.player,
			wp >= 0 ? std::format("50/50: {} won it against {}", name(wp), name(lp)) : std::format("50/50: {} and {}, nobody won it", name(x.player), name(y.player)));
	}

	// ---- Possession: the ball belongs to the team that touched it last ----
	{
		int curTeam = -1, curPlayer = -1, from = 0;
		size_t ti = 0;
		auto close = [&](int to) {
			if (to > from) a.possession.push_back({ from, to, d.frames[from].time, d.frames[std::min(to, N - 1)].time, curTeam, curPlayer });
			from = to;
		};
		for (int i = 0; i < N; i++)
		{
			if (!live[i]) { if (curTeam != -1) { close(i); curTeam = curPlayer = -1; } }
			while (ti < touches.size() && touches[ti].idx <= i)
			{
				const Touch& t = touches[ti++];
				if (!live[i]) continue;
				if (t.team != curTeam || t.player != curPlayer) { close(i); curTeam = t.team; curPlayer = t.player; }
			}
		}
		close(N);
		float secs[2] = { 0, 0 };
		for (const PossessionSpan& s : a.possession)
		{
			const float len = (s.to - s.from) * dt;
			if (s.team == 0 || s.team == 1) secs[s.team] += len;
			if (s.player >= 0) a.stats[s.player].onBallSecs += len;
		}
		const float both = secs[0] + secs[1];
		if (both > 0) { a.possessionPct[0] = 100.0f * secs[0] / both; a.possessionPct[1] = 100.0f * secs[1] / both; }
	}

	// ---- Passes, clears and giveaways: what each touch led to ----
	for (size_t i = 0; i + 1 < touches.size(); i++)
	{
		const Touch& x = touches[i];
		const Touch& y = touches[i + 1];
		if (!live[x.idx] || x.fifty) continue;
		bool atKickoff = false;   // the first second of a kickoff is the kickoff's own moment
		for (int ks : kickoffStart) if (x.idx >= ks - 2 && x.idx <= ks + fps) atKickoff = true;
		if (atKickoff) continue;
		const MatchFrame& fx = d.frames[x.idx];
		const MatchFrame& fy = d.frames[y.idx];
		const float gap = (y.idx - x.idx) * dt;
		const float fwd = Dir(x.team);
		const bool ownThird = fx.by * fwd < -1700.0f;
		// a clear: from your own third, sent up the pitch hard, and it got out
		const MatchFrame& soon = d.frames[std::min(N - 1, x.idx + std::max(2, fps / 6))];
		const MatchFrame& later = d.frames[std::min(N - 1, x.idx + 2 * fps)];
		const bool cleared = ownThird && soon.bvy * fwd > 900.0f && later.by * fwd > -1700.0f;
		bool goalBetween = false;
		for (int k = x.idx; k <= y.idx && !goalBetween; k++) if (!live[k]) goalBetween = true;
		if (goalBetween) continue;
		if (y.team == x.team && y.player != x.player && gap >= 0.35f && gap <= 4.0f && Dist(fx.bx, fx.by, 0, fy.bx, fy.by, 0) > 700.0f)
		{
			a.stats[x.player].passes++;
			a.passTo[x.player][y.player]++;
			addMoment(MomentKind::Pass, x.idx, x.team, x.player, y.player, std::format("Pass: {} to {}", name(x.player), name(y.player)));
		}
		else if (cleared)
		{
			a.stats[x.player].clears++;
			// where it went: a second on, is the ball still in front of your own goal?
			const MatchFrame& sec = d.frames[std::min(N - 1, x.idx + fps)];
			const bool middle = std::fabs(sec.bx) < 1100.0f && sec.by * fwd < 0.0f;
			if (middle) a.stats[x.player].clearsMiddle++;
			addMoment(MomentKind::Clear, x.idx, x.team, x.player, -1, std::format("Clear: {}{}", name(x.player), middle ? ", through the middle" : ""));
			a.moments.back().detail = middle ? MomentDetail::ClearMiddle : MomentDetail::ClearSide;
		}
		else if (y.team != x.team && !y.fifty && gap <= 5.0f
			&& !countedNear(x.idx, x.player, MomentKind::Shot, fps) && !countedNear(y.idx, y.player, MomentKind::Save, fps))
		{
			MatchPlayerStats& gs = a.stats[x.player];
			gs.giveaways++;
			// which kind: from your own third is dangerous; one that ends deep in their half is relief; one hit hard
			// with no teammate ahead of the ball is booming it; the rest go to an opponent in midfield
			bool mateAhead = false;
			for (int o = 0; o < P; o++)
				if (o != x.player && team(o) == x.team && fx.cars[o].present && fx.cars[o].y * fwd > fx.by * fwd + 300.0f) mateAhead = true;
			const float speedAfter = std::sqrt(Sq(soon.bvx) + Sq(soon.bvy) + Sq(soon.bvz));
			int kind;
			const char* how;
			if (ownThird) { kind = MomentDetail::GiveDangerous; how = "in their own third"; gs.giveDangerous++; }
			else if (fy.by * fwd > 2600.0f) { kind = MomentDetail::GiveRelief; how = "deep in the other end (no danger)"; gs.giveRelief++; }
			else if (speedAfter > 2300.0f && !mateAhead) { kind = MomentDetail::GiveBoomed; how = "booted up the pitch with nobody ahead"; gs.giveBoomed++; }
			else { kind = MomentDetail::GiveMidfield; how = "in midfield"; gs.giveMidfield++; }
			addMoment(MomentKind::Giveaway, x.idx, x.team, x.player, y.player, std::format("Giveaway: {} to {}, {}", name(x.player), name(y.player), how));
			a.moments.back().detail = kind;
		}
	}

	// ---- Per player: boost, where they were, demos ----
	const int sampleEvery = std::max(1, fps / 2);
	a.boost.assign(P, {});
	for (int i = 0; i < N; i += sampleEvery) { a.sampleTime.push_back(d.frames[i].time); a.sampleFrame.push_back(d.frames[i].frame); }
	for (int pl = 0; pl < P; pl++)
	{
		MatchPlayerStats& s = a.stats[pl];
		const int tm = team(pl);
		const float fwd = Dir(tm);
		int liveFrames = 0, zeroRun = 0, zeroFrom = 0, missing = 0, missFrom = 0;
		double boostSum = 0, speedSum = 0;
		int sup = 0, air = 0, def = 0, mid = 0, off = 0, behind = 0, closest = 0, lastBack = 0;
		float prevBoost = -1;
		auto endZero = [&](int i) {
			if (zeroRun >= 3 * fps)
				addMoment(MomentKind::NoBoost, zeroFrom, tm, pl, -1, std::format("{} had no boost for {:.0f} s", name(pl), zeroRun * dt));
			zeroRun = 0;
			(void)i;
		};
		for (int i = 0; i < N; i++)
		{
			const MatchFrame& f = d.frames[i];
			const MatchCar& c = f.cars[pl];
			if (i % sampleEvery == 0) a.boost[pl].push_back(c.present ? c.boost : -1.0f);
			if (!live[i]) { endZero(i); prevBoost = -1; missing = 0; continue; }
			if (!c.present)
			{
				if (missing++ == 0) missFrom = i;
				endZero(i);
				prevBoost = -1;
				continue;
			}
			if (missing >= fps)   // gone for a second or more in live play: demolished
			{
				s.demosTaken++;
				addMoment(MomentKind::Demo, missFrom, tm, pl, -1, name(pl) + " was demolished");
			}
			missing = 0;
			liveFrames++;
			boostSum += c.boost;
			speedSum += std::sqrt(Sq(c.vx) + Sq(c.vy) + Sq(c.vz));
			// from the speed, not the game's flag (which stays on down to 2,100): replay files don't carry the flag, and the
			// rank baselines come from replay files, so both have to be measured the same way
			if (Sq(c.vx) + Sq(c.vy) + Sq(c.vz) >= Sq(2200.0f)) sup++;
			if (!OnSurface(c)) air++;
			const float fy = c.y * fwd;
			if (fy < -1700.0f) def++; else if (fy > 1700.0f) off++; else mid++;
			if (f.hasBall && fy < f.by * fwd) behind++;
			// against teammates: nearest to the ball, and furthest back
			bool nearest = true, furthestBack = true;
			const float myDist = Dist(c.x, c.y, c.z, f.bx, f.by, f.bz);
			for (int o = 0; o < P; o++)
			{
				if (o == pl || team(o) != tm || !f.cars[o].present) continue;
				const MatchCar& m = f.cars[o];
				if (Dist(m.x, m.y, m.z, f.bx, f.by, f.bz) < myDist) nearest = false;
				if (m.y * fwd < fy) furthestBack = false;
			}
			if (nearest) closest++;
			if (furthestBack) lastBack++;
			// boost: time on empty, and what was picked up
			if (c.boost <= 1.0f) { if (zeroRun++ == 0) zeroFrom = i; s.noBoostSecs += dt; }
			else endZero(i);
			if (prevBoost >= 0 && c.boost - prevBoost >= 6.0f)
			{
				const float gain = c.boost - prevBoost;
				s.boostCollected += gain;
				if (gain > 20.0f || (c.boost >= 99.5f && prevBoost < 88.0f))
				{
					s.bigPads++;
					// a full pad is worth 100: whatever you already had is what it didn't give you
					if (prevBoost >= 60.0f)
					{
						s.boostWasted += prevBoost;
						addMoment(MomentKind::WastedBoost, i, tm, pl, -1, std::format("{} took a full boost pad with {:.0f} already", name(pl), prevBoost));
					}
				}
				else s.smallPads++;
			}
			// what was burned, and how much of it on the ground at top speed (2,300 is the limit; boost adds nothing there)
			if (prevBoost >= 0 && c.boost < prevBoost)
			{
				s.boostUsed += prevBoost - c.boost;
				if (OnSurface(c) && Sq(c.vx) + Sq(c.vy) + Sq(c.vz) >= Sq(2270.0f)) s.supersonicBoost += prevBoost - c.boost;
			}
			prevBoost = c.boost;
		}
		endZero(N);
		if (liveFrames > 0)
		{
			const float n = static_cast<float>(liveFrames);
			s.avgBoost = static_cast<float>(boostSum / n);
			s.avgSpeed = static_cast<float>(speedSum / n);
			s.supersonicPct = 100.0f * sup / n;
			s.airPct = 100.0f * air / n;
			s.defPct = 100.0f * def / n; s.midPct = 100.0f * mid / n; s.offPct = 100.0f * off / n;
			s.behindBallPct = 100.0f * behind / n;
			s.closestPct = 100.0f * closest / n;
			s.lastBackPct = 100.0f * lastBack / n;
		}
	}
	int liveCount = 0;
	for (char l : live) liveCount += l;
	a.seconds = liveCount * dt;

	// =====================================================================================================
	// Game sense (Nov 12): roles, double commits, beaten challenges, open nets, boost choices, and the chain
	// behind each goal. Every one is a test on the data with its moments kept, so a takeaway can show its evidence.
	// =====================================================================================================
	std::vector<signed char> possTeam(N, -1);
	for (const PossessionSpan& sp : a.possession) for (int i = sp.from; i < sp.to && i < N; i++) possTeam[i] = static_cast<signed char>(sp.team);
	a.live = live;
	a.possTeam = possTeam;
	auto goalSide = [&](int pl, int i) {   // between the ball and their own goal
		const MatchCar& c = d.frames[i].cars[pl];
		return c.present && c.y * Dir(team(pl)) < d.frames[i].by * Dir(team(pl)) - 100.0f;
	};
	auto secsAfter = [&](int i, float secs) { return std::min(N - 1, i + static_cast<int>(std::lround(secs * fps))); };

	// ---- Territory, time ahead of the play, nearly stopped ----
	{
		int half[2] = { 0, 0 }, liveN = 0;
		std::vector<int> ahead(P, 0), stopped(P, 0), liveP(P, 0);
		for (int i = 0; i < N; i++)
		{
			if (!live[i] || !d.frames[i].hasBall) continue;
			const MatchFrame& f = d.frames[i];
			liveN++;
			if (f.by > 0) half[0]++; else half[1]++;   // in orange's half = blue's territory
			for (int pl = 0; pl < P; pl++)
			{
				const MatchCar& c = f.cars[pl];
				if (!c.present) continue;
				liveP[pl]++;
				const float fwd = Dir(team(pl));
				if (c.y * fwd > 1700.0f && f.by * fwd < -1700.0f) ahead[pl]++;
				if (OnSurface(c) && Sq(c.vx) + Sq(c.vy) < Sq(250.0f)) stopped[pl]++;
			}
		}
		if (liveN > 0) { a.territoryPct[0] = 100.0f * half[0] / liveN; a.territoryPct[1] = 100.0f * half[1] / liveN; }
		for (int pl = 0; pl < P; pl++)
		{
			a.stats[pl].aheadSecs = ahead[pl] * dt;
			a.stats[pl].stoppedPct = liveP[pl] > 0 ? 100.0f * stopped[pl] / liveP[pl] : 0.0f;
			a.stats[pl].firstPct = a.stats[pl].closestPct;
		}
	}

	// ---- Double commits: at a touch, a teammate was right there too and closing on it ----
	// (Nov 13, after Coop's 3v3) It counts against the one who went for a teammate's ball, not both. Usually that's the
	// teammate who came in; but if the one touching had just taken it off that teammate (the teammate touched it last,
	// within two seconds), the one touching is the one who cut in.
	{
		int lastAt = -1000;
		for (size_t ti = 0; ti < touches.size(); ti++)
		{
			const Touch& t = touches[ti];
			if (!live[t.idx] || t.idx - lastAt < fps) continue;
			bool kick = false;
			for (int ks : kickoffStart) if (t.idx >= ks - 2 && t.idx <= ks + fps) kick = true;
			if (kick || a.teamSize < 2) continue;
			const MatchFrame& f = d.frames[std::max(0, t.idx - 1)];
			for (int o = 0; o < P; o++)
			{
				if (o == t.player || team(o) != t.team || !f.cars[o].present) continue;
				const MatchCar& c = f.cars[o];
				const float dx = f.bx - c.x, dy = f.by - c.y, dz = f.bz - c.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (dist > 900.0f || dist < 1.0f) continue;
				const float closing = (c.vx * dx + c.vy * dy + c.vz * dz) / dist;
				if (closing < 600.0f) continue;
				const bool tookIt = ti > 0 && touches[ti - 1].player == o && (t.idx - touches[ti - 1].idx) * dt <= 2.0f;
				const int went = tookIt ? t.player : o, owner = tookIt ? o : t.player;
				a.stats[went].doubleCommits++;
				a.doubleCommits[t.team]++;
				addMoment(MomentKind::DoubleCommit, t.idx, t.team, went, owner, std::format("Double commit: {} went for {}'s ball", name(went), name(owner)));
				lastAt = t.idx;
				break;
			}
		}
	}

	// ---- Beaten: you were on the ball carrier (a lost 50, or right there when they touched it) and a second later
	// the ball is behind you. Worse as the last player back. ----
	{
		std::vector<int> lastBeaten(P, -1000);
		for (const Touch& t : touches)
		{
			if (!live[t.idx]) continue;
			const MatchFrame& f = d.frames[t.idx];
			const int later = secsAfter(t.idx, 1.0f);
			if (!live[later]) continue;
			for (int pl = 0; pl < P; pl++)
			{
				if (team(pl) == t.team || team(pl) < 0 || !f.cars[pl].present || t.idx - lastBeaten[pl] < 2 * fps) continue;
				const MatchCar& c = f.cars[pl];
				const float fwd = Dir(team(pl));
				const float dx = f.bx - c.x, dy = f.by - c.y, dz = f.bz - c.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (dist > 500.0f || dist < 1.0f) continue;
				const float closing = (c.vx * dx + c.vy * dy + c.vz * dz) / dist;
				if (closing < 500.0f) continue;                       // they were going for it
				if (!(c.y * fwd < f.by * fwd + 150.0f)) continue;     // and were goal side of it (or level) when they did
				const MatchFrame& g = d.frames[later];
				if (!g.cars[pl].present || !(g.by * fwd < g.cars[pl].y * fwd - 300.0f)) continue;   // now the ball is behind them
				if (g.bvy * fwd > -200.0f) continue;                  // and still heading for their goal
				bool last = true;
				for (int o = 0; o < P; o++) if (o != pl && team(o) == team(pl) && goalSide(o, t.idx)) last = false;
				lastBeaten[pl] = t.idx;
				a.stats[pl].beaten++;
				if (last && a.teamSize > 1) a.stats[pl].beatenLast++;
				addMoment(MomentKind::Beaten, t.idx, team(pl), pl, t.player,
					std::format("Beaten: {} went past {}{}", name(t.player), name(pl), last && a.teamSize > 1 ? ", the last player back" : ""));
				a.moments.back().detail = last ? 1 : 0;
				a.moments.back().boost = c.boost;
			}
		}
	}

	// ---- Open net: the other team has the ball in your half and nobody is covering your goal ----
	// Covering (Nov 12, after Coop's 2:08 in clip 220): nearer to the middle of your own goal than the ball is, or right
	// on the ball. The first version only asked "further back down the pitch than the ball", which called it an open
	// net when a defender was level with a ball going wide into the corner, a car length from it.
	auto covering = [&](int pl, int i) {
		const MatchFrame& f = d.frames[i];
		const MatchCar& c = f.cars[pl];
		if (!c.present) return false;
		const float gy = -5120.0f * Dir(team(pl));
		const float carToGoal = std::sqrt(Sq(c.x) + Sq(c.y - gy)), ballToGoal = std::sqrt(Sq(f.bx) + Sq(f.by - gy));
		return carToGoal < ballToGoal || Dist(c.x, c.y, c.z, f.bx, f.by, f.bz) < 500.0f;
	};
	for (int tm = 0; tm < 2; tm++)
	{
		const float fwd = Dir(tm);
		int from = -1, lastBack = -1;
		auto close = [&](int to) {
			if (from >= 0 && (to - from) * dt >= 1.0f)
			{
				a.openNets[tm]++;
				a.openNetSecs[tm] += (to - from) * dt;
				if (lastBack >= 0) a.stats[lastBack].leftNetOpen++;
				addMoment(MomentKind::OpenNet, from, tm, lastBack, -1, lastBack >= 0
					? std::format("Open net for {:.0f} s: {} was the last to leave", (to - from) * dt, name(lastBack))
					: std::format("Open net for {:.0f} s", (to - from) * dt));
			}
			from = -1;
		};
		int prevBack = -1;
		for (int i = 0; i < N; i++)
		{
			const MatchFrame& f = d.frames[i];
			const bool danger = live[i] && f.hasBall && possTeam[i] == 1 - tm && f.by * fwd < -500.0f;
			int back = -1, anyone = 0;
			for (int pl = 0; pl < P; pl++)
				if (team(pl) == tm && covering(pl, i)) { anyone++; back = pl; }
			if (danger && anyone == 0) { if (from < 0) { from = i; lastBack = prevBack; } }
			else close(i);
			if (anyone > 0) prevBack = back;
		}
		close(N);
	}

	// ---- Boost choices: stolen pads, and leaving as the last player back to fetch a full pad ----
	for (int pl = 0; pl < P; pl++)
	{
		const int tm = team(pl);
		if (tm < 0) continue;
		const float fwd = Dir(tm);
		float prev = -1;
		for (int i = 0; i < N; i++)
		{
			const MatchCar& c = d.frames[i].cars[pl];
			if (!live[i] || !c.present) { prev = -1; continue; }
			if (prev >= 0 && (c.boost - prev > 20.0f || (c.boost >= 99.5f && prev < 88.0f)))
			{
				if (c.y * fwd > 3000.0f) a.stats[pl].stolenPads++;
				// a teammate's boost: you had 40 or more, they had far less and were a second away, driving at the same pad
				if (prev >= 40.0f)
					for (int o = 0; o < P; o++)
					{
						const MatchCar& m = d.frames[i].cars[o];
						if (o == pl || team(o) != tm || !m.present || m.boost > 35.0f || m.boost > prev - 25.0f) continue;
						const float dx = c.x - m.x, dy = c.y - m.y, dist = std::sqrt(dx * dx + dy * dy);
						if (dist > 1400.0f || dist < 1.0f || (m.vx * dx + m.vy * dy) / dist < 500.0f) continue;
						a.stats[pl].tookMateBoost++;
						addMoment(MomentKind::TookMateBoost, i, tm, pl, o, std::format("{} took the full pad {} was heading for ({:.0f} boost against {:.0f})", name(pl), name(o), prev, m.boost));
						break;
					}
				// took it as the only one back, with the other team on the ball in this half, off to the side of the goal
				bool onlyBack = true;
				for (int o = 0; o < P; o++) if (o != pl && team(o) == tm && goalSide(o, i)) onlyBack = false;
				const MatchFrame& f = d.frames[i];
				if (a.teamSize > 1 && onlyBack && possTeam[i] == 1 - tm && f.by * fwd < 0.0f && std::fabs(c.x) > 2500.0f && c.y * fwd < f.by * fwd)
				{
					a.stats[pl].leftForBoost++;
					addMoment(MomentKind::LeftForBoost, i, tm, pl, -1, std::format("{} went for a full pad as the last player back", name(pl)));
				}
			}
			prev = c.boost;
		}
	}

	// ---- Who did the defending, and how fast each player gets goal side after their team loses the ball ----
	for (const Touch& t : touches)
		if (live[t.idx] && d.frames[t.idx].by * Dir(t.team) < -1700.0f) a.stats[t.player].defensiveActions++;
	for (int pl = 0; pl < P; pl++) a.stats[pl].defensiveActions += a.stats[pl].saves;
	{
		std::vector<float> sum(P, 0.0f);
		std::vector<int> n(P, 0);
		for (size_t k = 1; k < a.possession.size(); k++)
		{
			const PossessionSpan& was = a.possession[k - 1];
			const PossessionSpan& now = a.possession[k];
			if (was.team < 0 || now.team < 0 || was.team == now.team) continue;
			for (int pl = 0; pl < P; pl++)
			{
				if (team(pl) != was.team || goalSide(pl, now.from)) continue;   // already goal side: nothing to recover
				int i = now.from;
				const int limit = secsAfter(now.from, 8.0f);
				while (i < limit && live[i] && !goalSide(pl, i)) i++;
				sum[pl] += (i - now.from) * dt;
				n[pl]++;
			}
		}
		for (int pl = 0; pl < P; pl++) a.stats[pl].recoverSecs = n[pl] >= 3 ? sum[pl] / n[pl] : 0.0f;
	}

	// =====================================================================================================
	// The rest of the catalogue (Nov 12): races in the air, following up a shot, shadowing, cutting rotation,
	// which post a player comes back to, and spacing.
	// =====================================================================================================
	std::vector<char> kickoffTime(N, 0);   // the two seconds after each kickoff: everyone is bunched and chasing by design
	for (int ks : kickoffStart) for (int i = std::max(0, ks - 2); i < std::min(N, ks + 2 * fps); i++) kickoffTime[i] = 1;

	// ---- Races in the air: a touch on a high ball with an opponent up there too, close and closing ----
	for (const Touch& t : touches)
	{
		if (!live[t.idx] || t.fifty) continue;
		const MatchFrame& f = d.frames[t.idx];
		if (f.bz < 450.0f) continue;
		for (int o = 0; o < P; o++)
		{
			const MatchCar& c = f.cars[o];
			if (team(o) == t.team || team(o) < 0 || !c.present || OnSurface(c) || c.z < 250.0f) continue;
			const float dx = f.bx - c.x, dy = f.by - c.y, dz = f.bz - c.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist > 800.0f || dist < 1.0f || (c.vx * dx + c.vy * dy + c.vz * dz) / dist < 300.0f) continue;
			a.stats[t.player].aerialsWon++;
			a.stats[o].aerialsLost++;
			addMoment(MomentKind::Aerial, t.idx, t.team, t.player, o, std::format("In the air: {} got there before {}", name(t.player), name(o)));
			break;
		}
	}

	// ---- Following up a shot: it didn't go in, the ball is still in their end a second later, and no teammate is
	// close enough for what comes off the save or the backboard ----
	if (a.teamSize > 1)
		for (const Counted& c : counted)
		{
			if (c.kind != MomentKind::Shot) continue;
			const int tm = team(c.player);
			if (tm < 0) continue;
			const float fwd = Dir(tm);
			const int later = secsAfter(c.idx, 1.0f), end = secsAfter(c.idx, 3.0f);
			bool dead = false;
			for (int k = c.idx; k <= end; k++) if (!live[k]) dead = true;     // it went in (or was on its way in)
			if (dead) continue;
			const MatchFrame& g = d.frames[later];
			if (!g.hasBall || g.by * fwd < 1700.0f) continue;                // cleared: nothing to follow up
			a.shotsStayed[tm]++;
			bool followed = false;
			for (int o = 0; o < P; o++)
			{
				const MatchCar& m = g.cars[o];
				// 4,000 is from about halfway to the goal line: further than that and a rebound is the other team's
				if (o != c.player && team(o) == tm && m.present && Dist(m.x, m.y, 0, g.bx, g.by, 0) < 4000.0f) followed = true;
			}
			for (const Touch& t : touches)
				if (t.idx > c.idx + 2 && t.idx <= end && t.team == tm && t.player != c.player) followed = true;
			if (followed) continue;
			a.shotsNoFollow[tm]++;
			addMoment(MomentKind::NoFollowUp, later, tm, c.player, -1, std::format("Nobody followed up {}'s shot", name(c.player)));
		}

	// ---- Per player, frame by frame: shadowing, cutting rotation, which post they come back to ----
	for (int pl = 0; pl < P; pl++)
	{
		const int tm = team(pl);
		if (tm < 0) continue;
		const float fwd = Dir(tm);
		int shadowFrom = -1, lastCut = -100000, lastReturn = -100000;
		float prevY = 0;
		bool hadPrev = false;
		auto endShadow = [&](int to) {
			if (shadowFrom >= 0 && (to - shadowFrom) * dt >= 1.2f)
			{
				a.stats[pl].shadows++;
				addMoment(MomentKind::Shadow, shadowFrom, tm, pl, -1, std::format("{} shadowed the ball back for {:.0f} s", name(pl), (to - shadowFrom) * dt));
			}
			shadowFrom = -1;
		};
		for (int i = 0; i < N; i++)
		{
			const MatchFrame& f = d.frames[i];
			const MatchCar& c = f.cars[pl];
			if (!live[i] || !c.present || !f.hasBall) { endShadow(i); hadPrev = false; continue; }
			const bool theirs = possTeam[i] == 1 - tm;
			const float cy = c.y * fwd, by = f.by * fwd;
			const float flat = Dist(c.x, c.y, 0, f.bx, f.by, 0);

			// shadowing: the other team is bringing it at your goal; you're goal side, a little way off, going back with it
			const bool shadowing = theirs && OnSurface(c) && by < 1000.0f && f.bvy * fwd < -500.0f && cy < by - 300.0f
				&& flat > 500.0f && flat < 2200.0f && c.vy * fwd < -400.0f && std::fabs(c.x - f.bx) < 1500.0f;
			if (shadowing) { if (shadowFrom < 0) shadowFrom = i; } else endShadow(i);

			// cutting rotation: the other team has it in your half, you're the wrong side of the ball and you go at it
			// anyway, while a teammate is goal side and near enough to be the one challenging
			if (a.teamSize > 1 && theirs && !kickoffTime[i] && by < 0.0f && by > -3800.0f && cy > by + 400.0f && i - lastCut > 3 * fps)
			{
				const float dx = f.bx - c.x, dy = f.by - c.y, dz = f.bz - c.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (dist < 600.0f && dist > 1.0f && (c.vx * dx + c.vy * dy + c.vz * dz) / dist >= 500.0f)
					for (int o = 0; o < P; o++)
					{
						const MatchCar& m = f.cars[o];
						if (o == pl || team(o) != tm || !goalSide(o, i) || Dist(m.x, m.y, 0, f.bx, f.by, 0) > 2200.0f) continue;
						lastCut = i;
						a.stats[pl].cutRotations++;
						addMoment(MomentKind::CutRotation, i, tm, pl, o, std::format("Cut rotation: {} chased it from behind with {} already goal side", name(pl), name(o)));
						break;
					}
			}

			// coming back: dropping into the last quarter of the pitch in front of their own goal while the other team has
			// the ball off to one side and they're out of the play (well away from it). Down the ball's side of the
			// pitch, or round the far post? Wider than the goal post on the ball's side counts as ball side.
			if (hadPrev && prevY >= -3800.0f && cy < -3800.0f && c.vy * fwd < -300.0f && theirs && by < 1000.0f && by > cy + 300.0f
				&& flat > 1500.0f && std::fabs(f.bx) > 1000.0f && i - lastReturn > 4 * fps)
			{
				lastReturn = i;
				a.stats[pl].returns++;
				if (c.x * (f.bx > 0 ? 1.0f : -1.0f) > 900.0f)
				{
					a.stats[pl].returnsBallSide++;
					addMoment(MomentKind::BallSide, i, tm, pl, -1, std::format("{} came back down the ball's side, not round the far post", name(pl)));
				}
			}
			prevY = cy;
			hadPrev = true;
		}
		endShadow(N);
	}

	// ---- Positioning (Nov 13): roles each frame, and where each player stands in theirs ----
	{
		struct Acc { int n = 0, first = 0, last = 0, theirs = 0, goalSideTheirs = 0, sup = 0, lastDef = 0, lastAtt = 0, flat = 0, supAtt = 0, wide = 0, far = 0;
			double supDist = 0, supBack = 0, lastDefD = 0, lastAttD = 0; };
		std::vector<Acc> acc(P);
		for (int i = 0; i < N; i++)
		{
			if (!live[i] || kickoffTime[i]) continue;
			const MatchFrame& f = d.frames[i];
			if (!f.hasBall) continue;
			for (int tm = 0; tm < 2; tm++)
			{
				const float fwd = Dir(tm);
				std::vector<int> mates;
				for (int pl = 0; pl < P; pl++) if (team(pl) == tm && pl < static_cast<int>(f.cars.size()) && f.cars[pl].present) mates.push_back(pl);
				if (mates.empty()) continue;
				auto dBall = [&](int pl) { return Dist(f.cars[pl].x, f.cars[pl].y, f.cars[pl].z, f.bx, f.by, f.bz); };
				auto upPitch = [&](int pl) { return f.cars[pl].y * fwd + 5120.0f; };   // 0 at their own goal line
				int first = mates[0];
				for (int pl : mates) if (dBall(pl) < dBall(first)) first = pl;
				int last = -1;
				for (int pl : mates) if (pl != first && (last < 0 || upPitch(pl) < upPitch(last))) last = pl;
				const bool ours = possTeam[i] == tm, theirs = possTeam[i] == 1 - tm;
				const float ballUp = f.by * fwd + 5120.0f;
				for (int pl : mates)
				{
					Acc& q = acc[pl];
					const MatchCar& c = f.cars[pl];
					q.n++;
					if (pl == first) q.first++;
					if (pl == last) q.last++;
					if (dBall(pl) > 5000.0f) q.far++;
					if (theirs) { q.theirs++; if (upPitch(pl) < ballUp - 100.0f) q.goalSideTheirs++; }
					// second: in 2v2 the one not on the ball; in 3v3 the middle one
					const bool second = pl != first && (mates.size() == 2 || (mates.size() == 3 && pl != last));
					if (second && ours)
					{
						q.sup++;
						q.supDist += Dist(c.x, c.y, 0, f.cars[first].x, f.cars[first].y, 0);
						q.supBack += ballUp - upPitch(pl);
						if (f.by * fwd > 0.0f) { q.supAtt++; if (std::fabs(c.x - f.bx) > 1200.0f) q.wide++; }
					}
					if (pl == last && mates.size() >= 2)
					{
						if (theirs) { q.lastDef++; q.lastDefD += upPitch(pl); }
						else if (ours && f.by * fwd > 0.0f) { q.lastAtt++; q.lastAttD += upPitch(pl); }
					}
					// flat: level with a teammate (the one on the ball included), so nobody is behind the other
					if (pl != first)
						for (int o : mates)
							if (o != pl && std::fabs(upPitch(o) - upPitch(pl)) < 500.0f) { q.flat++; break; }
				}
			}
		}
		for (int pl = 0; pl < P; pl++)
		{
			const Acc& q = acc[pl];
			MatchPlayerStats& s = a.stats[pl];
			if (q.n == 0) continue;
			s.firstSharePct = 100.0f * q.first / q.n;
			s.lastSharePct = 100.0f * q.last / q.n;
			s.farFromPlayPct = 100.0f * q.far / q.n;
			s.flatPct = 100.0f * q.flat / q.n;
			s.goalSideTheirsPct = q.theirs ? 100.0f * q.goalSideTheirs / q.theirs : 0.0f;
			s.supportDist = q.sup ? static_cast<float>(q.supDist / q.sup) : 0.0f;
			s.supportBack = q.sup ? static_cast<float>(q.supBack / q.sup) : 0.0f;
			s.wideSupportPct = q.supAtt ? 100.0f * q.wide / q.supAtt : 0.0f;
			s.lastDefDepth = q.lastDef ? static_cast<float>(q.lastDefD / q.lastDef) : 0.0f;
			s.lastAttDepth = q.lastAtt ? static_cast<float>(q.lastAttD / q.lastAtt) : 0.0f;
		}
	}

	// ---- Spacing: how much of the time two teammates were within two car lengths (kickoffs aside) ----
	if (a.teamSize > 1)
	{
		std::vector<int> near(P, 0);
		int teamNear[2] = { 0, 0 }, n = 0;
		for (int i = 0; i < N; i++)
		{
			if (!live[i] || kickoffTime[i]) continue;
			const MatchFrame& f = d.frames[i];
			n++;
			bool any[2] = { false, false };
			for (int x = 0; x < P; x++)
			{
				bool close = false;
				for (int y = 0; y < P; y++)
					if (x != y && team(x) == team(y) && team(x) >= 0 && f.cars[x].present && f.cars[y].present
						&& Dist(f.cars[x].x, f.cars[x].y, f.cars[x].z, f.cars[y].x, f.cars[y].y, f.cars[y].z) < 700.0f) close = true;
				if (close) { near[x]++; any[team(x)] = true; }
			}
			for (int tm = 0; tm < 2; tm++) if (any[tm]) teamNear[tm]++;
		}
		if (n > 0)
		{
			for (int pl = 0; pl < P; pl++) a.stats[pl].bunchedPct = 100.0f * near[pl] / n;
			for (int tm = 0; tm < 2; tm++) a.bunchedPct[tm] = 100.0f * teamNear[tm] / n;
		}
	}

	// =====================================================================================================
	// The scoring-chance line (Nov 13): for every frame, the chance each team scores in the next ten seconds, from a
	// model fitted on ranked replays. Then every touch is judged by how far it moved the line, and the ones that
	// moved it most, either way, earn a quick-chat label.
	// =====================================================================================================
	a.frameTime.resize(N);
	for (int i = 0; i < N; i++) a.frameTime[i] = d.frames[i].time;
	for (int tm = 0; tm < 2; tm++)
	{
		std::vector<float> raw(N, 0.0f);
		for (int i = 0; i < N; i++)
		{
			float feat[kChanceFeatures];
			if (live[i] && ChanceFeatures(d, i, tm, possTeam[i], feat)) raw[i] = Chance(feat);
		}
		// a light smooth (about a quarter of a second) so one odd frame doesn't make a spike
		const int w = std::max(1, fps / 8);
		a.chance[tm].assign(N, 0.0f);
		for (int i = 0; i < N; i++)
		{
			if (!live[i]) continue;
			float sum = 0; int n = 0;
			for (int k = std::max(0, i - w); k <= std::min(N - 1, i + w); k++) if (live[k]) { sum += raw[k]; n++; }
			a.chance[tm][i] = n > 0 ? sum / n : 0.0f;
		}
		bool in = false; int quiet = 0;
		for (int i = 0; i < N; i++)
		{
			const float c = a.chance[tm][i];
			if (!in && c >= 0.35f) { in = true; a.bigChances[tm]++; }
			if (in) { quiet = c < 0.2f ? quiet + 1 : 0; if (quiet > 2 * fps || !live[i]) { in = false; quiet = 0; } }
		}
	}
	{
		// the line's value for a team at a frame: their chance minus the other team's. A goal is the far end of it.
		auto value = [&](int tm, int i) { return a.chance[tm][i] - a.chance[1 - tm][i]; };
		for (size_t ti = 0; ti < touches.size(); ti++)
		{
			const Touch& t = touches[ti];
			if (!live[t.idx] || kickoffTime[t.idx] || t.fifty) continue;
			const int before = std::max(0, t.idx - std::max(2, fps / 5));
			if (!live[before]) continue;
			int after = secsAfter(t.idx, 0.7f);
			if (ti + 1 < touches.size() && touches[ti + 1].player != t.player) after = std::min(after, touches[ti + 1].idx - 1);
			if (after <= t.idx) continue;
			float was = value(t.team, before), now = value(t.team, after);
			float ownWas = a.chance[t.team][before], ownNow = a.chance[t.team][after], oppWas = a.chance[1 - t.team][before], oppNow = a.chance[1 - t.team][after];
			bool scored = false, ownGoal = false;
			for (int k = t.idx; k <= after; k++)
				if (!live[k])   // the ball went in before the look-ahead ran out
				{
					const MatchFrame& g = d.frames[std::min(N - 1, k + 1)];
					const MatchFrame& h = d.frames[t.idx];
					if (g.score[t.team] > h.score[t.team]) { scored = true; now = 1.0f; ownNow = 1.0f; oppNow = 0.0f; }
					else if (g.score[1 - t.team] > h.score[1 - t.team]) { ownGoal = true; now = -1.0f; ownNow = 0.0f; oppNow = 1.0f; }
					else now = was;
					break;
				}
			// a last touch on a shot that was already going in is a save that didn't come off, not a mistake: it counts nothing
			if (ownGoal && oppWas >= 0.4f) continue;
			const float swing = now - was;
			a.stats[t.player].chanceAdded += swing;
			a.touchSwings.push_back(swing);
			{
				const int g = swing >= kTouchCut[0] ? ReviewGrade::Calculated : swing >= kTouchCut[1] ? ReviewGrade::Clean : swing > kTouchCut[2] ? ReviewGrade::Solid
					: swing > kTouchCut[3] ? ReviewGrade::Sloppy : swing > kTouchCut[4] ? ReviewGrade::Costly : ReviewGrade::Throw;
				a.stats[t.player].touchGrades[g]++;
			}
			a.touchSwingPlayer.push_back(t.player);
			// cut-offs from every judged touch in the ranked replays: the best and worst half a percent, and the next 3%
			int tier = 0;
			if (swing >= kPlayCalculated) tier = MomentDetail::PlayCalculated;
			else if (swing >= kPlayGreat) tier = MomentDetail::PlayGreat;
			else if (swing <= kPlayBlunder) tier = MomentDetail::PlayBlunder;
			else if (swing <= kPlayWhoops) tier = MomentDetail::PlayWhoops;
			if (tier == 0) continue;
			const bool good = tier == MomentDetail::PlayCalculated || tier == MomentDetail::PlayGreat;
			(good ? a.stats[t.player].playsGood : a.stats[t.player].playsBad)++;
			const int within = fps;
			const char* label = tier == MomentDetail::PlayCalculated ? "Calculated." : tier == MomentDetail::PlayBlunder ? "$#@%!" : tier == MomentDetail::PlayWhoops ? "Whoops..."
				: countedNear(t.idx, t.player, MomentKind::Save, within) ? "What a save!" : (scored || countedNear(t.idx, t.player, MomentKind::Shot, within)) ? "Nice shot!"
				: (ti + 1 < touches.size() && touches[ti + 1].team == t.team && touches[ti + 1].player != t.player) ? "Great pass!" : d.frames[t.idx].by * Dir(t.team) < -1700.0f ? "Great clear!" : "What a play!";
			const char* own = t.team == 0 ? "blue" : "orange";
			const char* opp = t.team == 0 ? "orange" : "blue";
			std::string text;
			if (scored) text = std::format("{} {} scored from a {:.0f}% chance", label, name(t.player), 100.0f * ownWas);
			else if (ownGoal) text = std::format("{} {}'s touch put it in their own net", label, name(t.player));
			else if (good && oppWas - oppNow >= ownNow - ownWas) text = std::format("{} {} cut {}'s chance from {:.0f}% to {:.0f}%", label, name(t.player), opp, 100.0f * oppWas, 100.0f * oppNow);
			else if (good) text = std::format("{} {} took {}'s chance from {:.0f}% to {:.0f}%", label, name(t.player), own, 100.0f * ownWas, 100.0f * ownNow);
			else if (oppNow - oppWas >= ownWas - ownNow) text = std::format("{} {}'s touch took {}'s chance from {:.0f}% to {:.0f}%", label, name(t.player), opp, 100.0f * oppWas, 100.0f * oppNow);
			else text = std::format("{} {}'s touch dropped {}'s chance from {:.0f}% to {:.0f}%", label, name(t.player), own, 100.0f * ownWas, 100.0f * ownNow);
			std::string brief;
			if (scored) brief = std::format("Scored from a {:.0f}% chance.", 100.0f * ownWas);
			else if (ownGoal) brief = "Put it in their own net.";
			else if (good && oppWas - oppNow >= ownNow - ownWas) brief = std::format("Cut {}'s chance from {:.0f}% to {:.0f}%.", opp, 100.0f * oppWas, 100.0f * oppNow);
			else if (good) brief = std::format("Took {}'s chance from {:.0f}% to {:.0f}%.", own, 100.0f * ownWas, 100.0f * ownNow);
			else if (oppNow - oppWas >= ownWas - ownNow) brief = std::format("Gave {} a chance: {:.0f}% to {:.0f}%.", opp, 100.0f * oppWas, 100.0f * oppNow);
			else brief = std::format("Dropped {}'s chance from {:.0f}% to {:.0f}%.", own, 100.0f * ownWas, 100.0f * ownNow);
			addMoment(MomentKind::Play, t.idx, t.team, t.player, -1, std::move(text));
			a.moments.back().brief = std::move(brief);
			a.moments.back().detail = tier;
			a.moments.back().swing = swing;
		}
	}

	// ---- The level each player's numbers look like, and the lobby's (the average): picks which rank's baselines the
	// takeaways compare against. One game is a rough read: right to within a rank about two times in three. ----
	if (liveCount * dt >= 120.0f && a.teamSize >= 1 && a.teamSize <= 3)
	{
		float sum = 0; int n = 0;
		for (int pl = 0; pl < P; pl++)
		{
			if (team(pl) < 0) continue;
			float v = kLevelB[a.teamSize - 1];
			for (int k = 0; k < kLevelN; k++) v += kLevelW[a.teamSize - 1][k] * BaseValue(a.stats[pl], kLevelStat[k], liveCount * dt);
			a.stats[pl].level = std::clamp(v, 0.0f, 5.0f);
			sum += a.stats[pl].level; n++;
			float pv = kPosB[a.teamSize - 1];
			for (int k = 0; k < kPosN; k++) pv += kPosW[a.teamSize - 1][k] * BaseValue(a.stats[pl], kPosStat[k], liveCount * dt);
			a.stats[pl].posLevel = std::clamp(pv, 0.0f, 5.0f);
		}
		// the fits pull everyone toward the middle ranks, so the lobby's average is read against what that average
		// actually is at each rank in the ranked replays
		if (n > 0) a.lobbyLevel = LevelOf(a.teamSize, Base_level, sum / n);
	}

	// ---- Impact against the lobby (Nov 13): chance added per five minutes, minus the average player's in this game.
	// In ranked replays a quarter to a half of the spread in chance added is the game itself (a fast, high-scoring game
	// lifts everyone), so the raw number graded nearly everyone in a lively lobby as top quarter. Against the lobby, it
	// also tells a win better. ----
	{
		float sum = 0; int n = 0;
		for (int pl = 0; pl < P; pl++) if (team(pl) >= 0) { sum += a.stats[pl].chanceAdded; n++; }
		const float mean = n > 0 ? sum / n : 0.0f;
		for (int pl = 0; pl < P; pl++) if (team(pl) >= 0) a.stats[pl].impact = a.stats[pl].chanceAdded - mean;
	}

	// ---- The review's grades (Nov 13): each area's numbers placed among players of the lobby's rank, averaged, and that
	// average placed among the same players' averages. The areas and their numbers are generated (kArea*), so the
	// grades here and the cut-offs in tools/match/gen.py come from the same list. ----
	if (a.lobbyLevel >= 0)
	{
		const int ts = std::clamp(a.teamSize, 1, 3), lv = static_cast<int>(std::lround(a.lobbyLevel));
		const float secs = liveCount * dt;
		auto gradeOf = [&](int idx, float score) {
			const float* c = kAreaCut[ts - 1][lv][idx];   // the 3rd, 10th, 25th, 75th and 95th percentile of the score
			return score >= c[4] ? ReviewGrade::Calculated : score >= c[3] ? ReviewGrade::Clean : score > c[2] ? ReviewGrade::Solid
				: score > c[1] ? ReviewGrade::Sloppy : score > c[0] ? ReviewGrade::Costly : ReviewGrade::Throw;
		};
		for (int pl = 0; pl < P; pl++)
		{
			if (team(pl) < 0) continue;
			MatchPlayerStats& s = a.stats[pl];
			float sum = 0;
			for (int ar = 0; ar < Area_Count; ar++)
			{
				float tot = 0; int n = 0;
				for (int k = 0; k < kAreaN[ar]; k++)
				{
					const int st = kAreaStat[ar][k];
					if (kAreaTeam[ar][k] && ts == 1) continue;
					if (st == Base_recoverSecs && s.recoverSecs <= 0.0f) continue;   // never measured in this game
					const float p = PctOf(Baseline(ts, lv, st), BaseValue(s, st, secs));
					tot += kBaseBetter[st] < 0 ? 100.0f - p : p;
					n++;
				}
				s.area[ar].score = n > 0 ? tot / n : 50.0f;
				s.area[ar].grade = gradeOf(ar, s.area[ar].score);
				sum += s.area[ar].score;
			}
			s.impactPct = PctOf(Baseline(ts, lv, Base_impact), BaseValue(s, Base_impact, secs));
			s.overallScore = (sum + s.impactPct) / (Area_Count + 1);
			s.overall = gradeOf(Area_Count, s.overallScore);
		}
	}

	std::stable_sort(a.moments.begin(), a.moments.end(), [](const Moment& x, const Moment& y) { return x.index < y.index; });
	const int M = static_cast<int>(a.moments.size());

	// ---- The chain behind each goal: the conceding team's mistakes in the six seconds before it ----
	for (int g = 0; g < M; g++)
	{
		const Moment& goal = a.moments[g];
		if (goal.kind != MomentKind::Goal || goal.team < 0) continue;
		const int against = 1 - goal.team;
		GoalChain ch;
		ch.goalMoment = g;
		ch.against = against;
		// a goal off a quick break: the scoring team won the ball in their own half less than six seconds before, and
		// kept it from there to the goal
		{
			int k = static_cast<int>(a.possession.size()) - 1;
			while (k >= 0 && a.possession[k].from > goal.index) k--;
			while (k >= 0 && a.possession[k].team != goal.team) k--;          // the scoring team's last spell before the goal
			while (k > 0 && a.possession[k - 1].team == goal.team) k--;       // back to where that spell began
			if (k > 0 && a.possession[k - 1].team == against && goal.time - a.possession[k].t0 <= 6.0f
				&& d.frames[a.possession[k].from].by * Dir(goal.team) < 0.0f)
				a.counterGoals[goal.team]++;
		}
		std::vector<bool> counted(P, false);
		for (int k = 0; k < M; k++)
		{
			const Moment& m = a.moments[k];
			if (m.team != against || m.time > goal.time || goal.time - m.time > 6.0f) continue;
			std::string part;
			switch (m.kind)
			{
			case MomentKind::Giveaway:
				if (m.detail == MomentDetail::GiveRelief) continue;
				part = std::format("{} gave it away {}", name(m.player), m.detail == MomentDetail::GiveDangerous ? "in their own third" : m.detail == MomentDetail::GiveBoomed ? "booting it away" : "in midfield");
				break;
			case MomentKind::Beaten: part = std::format("{} was beaten{}", name(m.player), m.detail == 1 ? " as the last player back" : ""); break;
			case MomentKind::DoubleCommit: part = std::format("{} and {} both went for it", name(m.player), name(m.other)); break;
			case MomentKind::OpenNet: part = m.player >= 0 ? std::format("the net was open ({} last to leave)", name(m.player)) : std::string("the net was open"); break;
			case MomentKind::LeftForBoost: part = std::format("{} had gone for boost", name(m.player)); break;
			case MomentKind::Clear: if (m.detail != MomentDetail::ClearMiddle) continue; part = std::format("{} cleared it through the middle", name(m.player)); break;
			case MomentKind::Fifty: if (m.player < 0 || team(m.player) == against) continue; part = std::format("{} lost a 50", name(m.other)); break;
			default: continue;
			}
			ch.moments.push_back(k);
			ch.text += (ch.text.empty() ? "" : ", then ") + part;
			for (int who : { m.player, m.other })
				if (who >= 0 && who < P && team(who) == against && !counted[who] && !(m.kind == MomentKind::Fifty && who == m.player))
				{
					counted[who] = true;
					a.stats[who].goalsAgainstIn++;
				}
		}
		// out of boost when it went in
		for (int pl = 0; pl < P; pl++)
		{
			if (team(pl) != against) continue;
			const MatchCar& c = d.frames[std::max(0, goal.index - fps)].cars[pl];
			if (c.present && c.boost <= 5.0f)
			{
				ch.text += (ch.text.empty() ? "" : "; ") + std::format("{} had no boost", name(pl));
			}
		}
		if (!ch.text.empty())
		{
			if (ch.text.rfind("the net", 0) == 0) ch.text[0] = 'T';   // (names are left exactly as the players wrote them)
			a.chains.push_back(std::move(ch));
		}
	}

	// =====================================================================================================
	// Takeaways: findings with reasons. Each needs a pattern (not one instance), carries the moments behind it, and
	// says "this happened", never "you were wrong": a scan has no comms and no intent.
	// =====================================================================================================
	{
		auto momentsOf = [&](std::initializer_list<MomentKind> kinds, int pl, int detail = 0, bool asOther = false) {
			std::vector<int> out;
			for (int k = 0; k < M; k++)
			{
				const Moment& m = a.moments[k];
				bool kindOk = false;
				for (MomentKind kk : kinds) if (m.kind == kk) kindOk = true;
				if (!kindOk || (detail != 0 && m.detail != detail)) continue;
				if (m.player == pl || (asOther && m.other == pl)) out.push_back(k);
			}
			return out;
		};
		auto teamMoments = [&](MomentKind kind, int tm) {
			std::vector<int> out;
			for (int k = 0; k < M; k++) if (a.moments[k].kind == kind && a.moments[k].team == tm) out.push_back(k);
			return out;
		};
		const float liveSecs = std::max(1.0f, liveCount > 0 ? liveCount * dt : 1.0f);
		const int lobby = a.lobbyLevel >= 0 ? static_cast<int>(std::lround(a.lobbyLevel)) : -1;
		// `stat` ties a finding to a number with a rank baseline. A "work on" finding is dropped when the number is no
		// worse than the middle player at this lobby's level, and the detail gains what's typical there.
		auto add = [&](int pl, int tm, bool good, float weight, std::string claim, std::string detail, std::string tryThis, std::vector<int> moments, int stat = -1) {
			if (stat >= 0 && pl >= 0 && lobby >= 0)
			{
				const float v = BaseValue(a.stats[pl], stat, liveSecs);
				const float* b = kBase[std::clamp(a.teamSize, 1, 3) - 1][lobby][stat];
				const int better = kBaseBetter[stat];
				if (!good && better != 0)
				{
					if (better < 0 ? v <= b[2] : v >= b[2]) return;
					if (better < 0 ? v >= b[4] : v <= b[0]) weight *= 1.3f;
				}
				if (!detail.empty() && detail.back() != ' ') detail += ' ';
				if (kBasePer5[stat])
				{
					const float typical = b[2] * liveSecs / 300.0f;
					detail += typical < 0.5f ? std::format("Typical at {}: under one in a game this long.", LevelName(lobby)) : std::format("Typical at {}: about {:.0f} in a game this long.", LevelName(lobby), typical);
				}
				else detail += std::format("Typical at {}: {:.0f}{}.", LevelName(lobby), b[2], stat == Base_recoverSecs ? " s" : stat == Base_avgSpeed ? "" : "%");
			}
			Takeaway t;
			t.player = pl; t.team = tm; t.good = good; t.weight = weight;
			t.claim = std::move(claim); t.detail = std::move(detail); t.tryThis = std::move(tryThis); t.moments = std::move(moments);
			a.takeaways.push_back(std::move(t));
		};
		const float even = a.teamSize > 0 ? 100.0f / a.teamSize : 100.0f;
		int goalsAgainst[2] = { a.score[1], a.score[0] };

		for (int pl = 0; pl < P; pl++)
		{
			const int tm = team(pl);
			if (tm < 0) continue;
			const MatchPlayerStats& s = a.stats[pl];
			std::vector<int> mates, all;
			for (int o = 0; o < P; o++) { if (o != pl && team(o) == tm) mates.push_back(o); if (o != pl && team(o) >= 0) all.push_back(o); }

			// --- giveaways: how many that hurt, and which kind ---
			{
				const int hurt = s.giveDangerous + s.giveMidfield + s.giveBoomed;
				int mostOther = 0;
				for (int o : all) mostOther = std::max(mostOther, a.stats[o].giveDangerous + a.stats[o].giveMidfield + a.stats[o].giveBoomed);
				if (hurt >= 3 || s.giveDangerous >= 2)
				{
					std::string kinds;
					auto part = [&](int n, const char* what) { if (n > 0) kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ", n, what); };
					part(s.giveDangerous, "from their own third");
					part(s.giveMidfield, "in midfield");
					part(s.giveBoomed, "booted away with nobody ahead");
					const char* tryThis = s.giveDangerous >= s.giveMidfield && s.giveDangerous >= s.giveBoomed ? "In your own third, put it to the side or the corner, or keep it close until a teammate is back."
						: s.giveBoomed >= s.giveMidfield ? "Before you hit it hard, check someone is ahead of the ball. If not, keep it or play it to a corner."
						: "With an opponent in front of you, take a touch to the side first, or leave it for the teammate behind you.";
					std::vector<int> ms = momentsOf({ MomentKind::Giveaway }, pl);
					ms.erase(std::remove_if(ms.begin(), ms.end(), [&](int k) { return a.moments[k].detail == MomentDetail::GiveRelief; }), ms.end());
					add(pl, tm, false, hurt * 1.0f + s.giveDangerous * 1.5f + (hurt > mostOther ? 1.5f : 0.0f),
						std::format("Gave the ball away {} times{}", hurt, hurt > mostOther ? ", the most in the match" : ""), kinds + ".", tryThis, std::move(ms));
				}
				else if (s.touches >= 12 && hurt == 0)
					add(pl, tm, true, 2.0f, "Never gave the ball away cheaply", std::format("{} touches, none straight to an opponent in a bad spot.", s.touches), "", {});
			}

			// --- who did the defending ---
			if (!mates.empty())
			{
				int teamDef = s.defensiveActions;
				for (int o : mates) teamDef += a.stats[o].defensiveActions;
				if (teamDef >= 6)
				{
					const float share = 100.0f * s.defensiveActions / teamDef;
					if (share >= std::min(80.0f, even + 18.0f))
						add(pl, tm, true, 2.0f + share / 25.0f, "Did most of the defending",
							std::format("{} of the team's {} saves and touches in their own third ({:.0f}%), and last back {:.0f}% of the time.", s.defensiveActions, teamDef, share, s.lastBackPct), "", {});
					for (int o : mates)
					{
						const MatchPlayerStats& m = a.stats[o];
						const float oShare = 100.0f * m.defensiveActions / teamDef;
						if (oShare >= std::min(80.0f, even + 18.0f) && s.behindBallPct < m.behindBallPct - 8.0f)
							add(pl, tm, false, 3.0f + (m.behindBallPct - s.behindBallPct) / 6.0f, std::format("Left the defending to {}", name(o)),
								std::format("{} made {} of the team's {} defensive plays. {} was goal side of the ball {:.0f}% of the time against {}'s {:.0f}%{}.",
									name(o), m.defensiveActions, teamDef, name(pl), s.behindBallPct, name(o), m.behindBallPct,
									s.aheadSecs >= 4.0f ? std::format(", and spent {:.0f} s up the pitch while the ball was in their own third", s.aheadSecs) : std::string()),
								"After your touch up the pitch, turn back toward your own half straight away. Don't wait to see how it plays out.", {});
					}
				}
			}

			// --- taking turns: first to the ball far more than an even share, with double commits ---
			if (!mates.empty())
			{
				if (s.firstPct >= even + 12.0f && s.doubleCommits >= 2)
					add(pl, tm, false, 2.0f + s.doubleCommits + (s.firstPct - even) / 8.0f, "Kept going when it was a teammate's ball",
						std::format("Nearest to the ball {:.0f}% of the time (an even share is {:.0f}%), and went for a teammate's ball {} times.", s.firstPct, even, s.doubleCommits),
						"After your touch, rotate out behind your teammate and let them have the next one.", momentsOf({ MomentKind::DoubleCommit }, pl), Base_doubleCommits);
				else if (s.doubleCommits >= 3)
					add(pl, tm, false, 1.5f + s.doubleCommits, std::format("Went for a teammate's ball {} times", s.doubleCommits),
						"Two of you on the same ball leaves nobody for what comes next.", "If a teammate is already closing on it, hold a car length or two back and take what comes off it.",
						momentsOf({ MomentKind::DoubleCommit }, pl), Base_doubleCommits);
			}

			// --- beaten ---
			if (s.beaten >= 2)
			{
				std::vector<int> ms = momentsOf({ MomentKind::Beaten }, pl);
				int lowBoost = 0;
				for (int k : ms) if (a.moments[k].boost >= 0 && a.moments[k].boost < 12.0f) lowBoost++;
				std::string det = s.beatenLast > 0 ? std::format("{} of them as the last player back.", s.beatenLast) : std::string("Each time the ball carrier got past and kept going at the goal.");
				if (lowBoost >= 2) det += std::format(" {} were with under 12 boost.", lowBoost);
				if (s.shadows == 0) det += " Never once backed off with the ball carrier.";
				add(pl, tm, false, 1.5f + s.beaten + s.beatenLast * 1.5f, std::format("Beaten on the challenge {} times", s.beaten), det,
					s.beatenLast > 0 ? "As the last player back, stay between the ball and the goal and back off with it. Challenge once a teammate is behind you, or when they take a heavy touch."
						: "Slow down as you close in, and go once their touch leaves the ball loose.", std::move(ms), Base_beaten);
			}

			// --- 50s ---
			if (s.fifties >= 4)
			{
				const float rate = static_cast<float>(s.fiftiesWon) / s.fifties;
				if (rate >= 0.6f)
					add(pl, tm, true, 1.5f + s.fiftiesWon, std::format("Won {} of {} 50/50s", s.fiftiesWon, s.fifties), "The ball came out their team's way more often than not.", "", momentsOf({ MomentKind::Fifty }, pl, 0, true));
				else if (rate <= 0.25f)
					add(pl, tm, false, 1.5f + (s.fifties - s.fiftiesWon), std::format("Won {} of {} 50/50s", s.fiftiesWon, s.fifties),
						s.lowBoostFifties >= 2 ? std::format("{} of them were taken with under 12 boost.", s.lowBoostFifties) : std::string("The ball kept coming out the other team's way."),
						"Get your nose behind the middle of the ball and hit it a touch later than they do.", momentsOf({ MomentKind::Fifty }, pl, 0, true));
			}

			// --- left the net open ---
			if (s.leftNetOpen >= 2)
				add(pl, tm, false, 1.5f + s.leftNetOpen * 1.2f, std::format("Last to leave an open net {} times", s.leftNetOpen),
					"The other team had the ball in this half with nobody between it and the goal.", "If you're the one furthest back and nobody is behind you, stay goal side until a teammate arrives.",
					momentsOf({ MomentKind::OpenNet }, pl), Base_leftNetOpen);

			// --- boost ---
			{
				const float emptyPct = 100.0f * s.noBoostSecs / liveSecs;
				if (emptyPct >= 14.0f)
					add(pl, tm, false, 1.0f + emptyPct / 6.0f, std::format("On empty for {:.0f} s ({:.0f}% of the match)", s.noBoostSecs, emptyPct),
						std::format("Average boost {:.0f}. Took {} small pads and {} full ones.", s.avgBoost, s.smallPads, s.bigPads),
						"Drive over the small pads on your way back; two of them is enough for most saves.", momentsOf({ MomentKind::NoBoost }, pl), Base_noBoostSecs);
				if (s.boostWasted >= 150.0f)
					add(pl, tm, false, 1.0f + s.boostWasted / 120.0f, std::format("Wasted {:.0f} boost topping up at full pads", s.boostWasted),
						"That's what they already had each time they took a full pad at 60 or more.", "Over 60, leave the full pad for a teammate and take small pads instead.", momentsOf({ MomentKind::WastedBoost }, pl), Base_boostWasted);
				if (s.leftForBoost >= 2)
					add(pl, tm, false, 2.0f + s.leftForBoost * 1.5f, std::format("Went for a full pad as the last player back {} times", s.leftForBoost),
						"Each time the other team had the ball in this half.", "With nobody behind you, defend on what you have and pick up small pads; the corner boost can wait.", momentsOf({ MomentKind::LeftForBoost }, pl));
				if (s.stolenPads >= 3)
					add(pl, tm, true, 1.0f + s.stolenPads * 0.5f, std::format("Took {} full pads in the other team's end", s.stolenPads), "That's boost their defence didn't get.", "", {});
			}

			// --- clears through the middle ---
			if (s.clears >= 3 && s.clearsMiddle * 2 >= s.clears)
				add(pl, tm, false, 1.5f + s.clearsMiddle, std::format("{} of {} clears went through the middle", s.clearsMiddle, s.clears),
					"A second later the ball was still in front of their own goal.", "Clear to the side wall or the corner: a blocked clear there isn't a shot.", momentsOf({ MomentKind::Clear }, pl, MomentDetail::ClearMiddle));

			// --- the rest of the catalogue (Nov 12) ---
			if (s.supersonicBoost >= 100.0f && s.supersonicBoost >= 0.08f * s.boostUsed)
				add(pl, tm, false, 1.0f + s.supersonicBoost / 80.0f, std::format("Burned {:.0f} boost while already at top speed", s.supersonicBoost),
					std::format("That's {:.0f}% of all the boost they used. On the ground at top speed, boost adds nothing.", 100.0f * s.supersonicBoost / std::max(1.0f, s.boostUsed)),
					"Once the supersonic trail shows, let go of boost. Holding forward keeps the speed.", {}, Base_supersonicBoost);
			if (s.tookMateBoost >= 2)
				add(pl, tm, false, 1.5f + s.tookMateBoost * 1.5f, std::format("Took a full pad a teammate was heading for, {} times", s.tookMateBoost),
					"Each time they had 40 or more and the teammate was low.", "If a teammate with less boost is going for the same pad, peel off and take small pads.",
					momentsOf({ MomentKind::TookMateBoost }, pl));
			{
				const int races = s.aerialsWon + s.aerialsLost;
				if (races >= 3 && s.aerialsWon * 3 >= races * 2)
					add(pl, tm, true, 1.5f + s.aerialsWon, std::format("First to {} of {} balls in the air", s.aerialsWon, races), "With an opponent going up for the same ball.", "",
						momentsOf({ MomentKind::Aerial }, pl, 0, true));
				else if (races >= 3 && s.aerialsWon * 3 <= races)
					add(pl, tm, false, 1.5f + s.aerialsLost, std::format("Beaten to {} of {} balls in the air", s.aerialsLost, races), "An opponent got the touch each time.",
						"If they're already on the way up, stay down and get under where it's going to land.", momentsOf({ MomentKind::Aerial }, pl, 0, true));
			}
			if (s.shadows >= 3)
				add(pl, tm, true, 1.0f + s.shadows * 0.8f, std::format("Shadowed the ball carrier {} times", s.shadows),
					"Stayed goal side and backed off with the ball, where a dive in would have left the goal open.", "", momentsOf({ MomentKind::Shadow }, pl));
			if (s.cutRotations >= 2)
				add(pl, tm, false, 1.5f + s.cutRotations * 1.5f, std::format("Chased the ball from the wrong side of it {} times", s.cutRotations),
					"Each time a teammate was goal side and close enough to be the one challenging.",
					"Caught ahead of the ball? Go back round the far side and come in behind your teammate.", momentsOf({ MomentKind::CutRotation }, pl));
			// (Which post they come back to: measured, but not a finding. In 630 ranked replays the share coming back down
			// the ball's side goes UP with rank, 52% at gold to 73% at supersonic legend in 2v2, so it isn't a fault.)

			// --- against the lobby's level: pace, standing still, small pads ---
			if (lobby >= 0)
			{
				const float* sp = kBase[std::clamp(a.teamSize, 1, 3) - 1][lobby][Base_supersonicPct];
				const float* st = kBase[std::clamp(a.teamSize, 1, 3) - 1][lobby][Base_stoppedPct];
				const float* sm = kBase[std::clamp(a.teamSize, 1, 3) - 1][lobby][Base_smallPads];
				const float small5 = BaseValue(s, Base_smallPads, liveSecs);
				if (s.supersonicPct <= sp[0])
					add(pl, tm, false, 2.0f, std::format("Slow for this lobby: supersonic {:.0f}% of the time", s.supersonicPct),
						std::format("Nine in ten {} players are above {:.0f}%; the middle one is at {:.0f}%.", LevelName(lobby), sp[0], sp[2]),
						"Between plays, keep your speed: turn wide over small pads where you'd otherwise brake and turn sharp.", {});
				else if (s.supersonicPct >= sp[4])
					add(pl, tm, true, 1.5f, std::format("Fast for this lobby: supersonic {:.0f}% of the time", s.supersonicPct),
						std::format("The middle {} player is at {:.0f}%.", LevelName(lobby), sp[2]), "", {});
				if (s.stoppedPct >= st[4] && s.stoppedPct >= 3.0f)
					add(pl, tm, false, 1.5f + s.stoppedPct / 3.0f, std::format("Sat nearly still for {:.0f}% of the match", s.stoppedPct),
						std::format("The middle {} player: {:.0f}%.", LevelName(lobby), st[2]), "Waiting is fine, but wait rolling: a slow circle keeps you a second quicker to anything than a standing start.", {});
				if (small5 <= sm[0])
					add(pl, tm, false, 1.5f, std::format("Only {} small pads all match", s.smallPads),
						std::format("The middle {} player picks up about {:.0f} in a game this long.", LevelName(lobby), sm[2] * liveSecs / 300.0f),
						"Learn one line of small pads down each side and take it every time you rotate back.", {});
			}
			// --- positioning against the lobby's level (Nov 13). In ranked replays these are the positioning numbers that
			// move with rank: higher ranks are goal side more when the other team has it, out of the play less, and (2v2,
			// 3v3) less bunched and closer behind the ball when supporting. Other measures (how deep the last player sits,
			// how wide the support is) barely change from gold to SSL, so they aren't findings. ---
			if (lobby >= 0)
			{
				const int ts = std::clamp(a.teamSize, 1, 3) - 1;
				const float* gs = kBase[ts][lobby][Base_goalSideTheirsPct];
				const float* far = kBase[ts][lobby][Base_farFromPlayPct];
				const float* sb = kBase[ts][lobby][Base_supportBack];
				const float* bu = kBase[ts][lobby][Base_bunchedPct];
				auto carLengths = [](float uu) { return std::lround(uu / 118.0f); };
				std::vector<std::string> weak, strong;
				if (s.goalSideTheirsPct <= gs[1]) weak.push_back(std::format("goal side only {:.0f}% of the time the other team had the ball (typical {:.0f}%)", s.goalSideTheirsPct, gs[2]));
				else if (s.goalSideTheirsPct >= gs[3]) strong.push_back(std::format("goal side {:.0f}% of the time the other team had the ball (typical {:.0f}%)", s.goalSideTheirsPct, gs[2]));
				if (s.farFromPlayPct >= far[3]) weak.push_back(std::format("out of the play {:.0f}% of the time (typical {:.0f}%)", s.farFromPlayPct, far[2]));
				if (a.teamSize > 1 && s.supportBack >= sb[3]) weak.push_back(std::format("about {} car lengths behind the ball when supporting (typical {})", carLengths(s.supportBack), carLengths(sb[2])));
				else if (a.teamSize > 1 && s.supportBack > 0.0f && s.supportBack <= sb[1]) strong.push_back(std::format("close behind the ball when supporting, about {} car lengths (typical {})", carLengths(s.supportBack), carLengths(sb[2])));
				if (a.teamSize > 1 && s.bunchedPct >= bu[4]) weak.push_back(std::format("near a teammate {:.0f}% of the time (typical {:.0f}%)", s.bunchedPct, bu[2]));
				auto join = [](const std::vector<std::string>& v) {
					std::string out;
					for (size_t k = 0; k < v.size(); k++) out += (k == 0 ? "" : k + 1 == v.size() ? " and " : ", ") + v[k];
					if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
					return out + ".";
				};
				// the same grade the review shows for the area
				const int posG = s.area[Area_Positioning].grade;
				if (posG >= ReviewGrade::Sloppy && !weak.empty())
					add(pl, tm, false, 3.0f + weak.size() * 0.6f + (posG - ReviewGrade::Sloppy),
						std::format("Positioning: {} for a {} lobby", GradeName(posG), LevelName(lobby)), join(weak),
						s.goalSideTheirsPct <= gs[1] ? "When they win the ball, your first job is getting between it and your net. Turn back before you look for the next touch."
						: s.farFromPlayPct >= far[3] ? "Keep your boost runs short: stay within half a pitch of the ball, and take small pads on the way."
						: s.supportBack >= sb[3] ? "When your teammate has it in their half, follow closer: near enough to take whatever comes off their touch."
						: "Give your teammate room: drop back or go wide instead of sitting next to them.", {});
				else if (posG >= 0 && posG <= ReviewGrade::Clean && !strong.empty())
					add(pl, tm, true, 2.5f, std::format("Positioning: {} for a {} lobby", GradeName(posG), LevelName(lobby)), join(strong), "", {});
				else if (!weak.empty())
					add(pl, tm, false, 1.5f + weak.size() * 0.7f, weak.size() == 1 ? std::string("One part of positioning to tidy up") : std::format("{} parts of positioning to tidy up", weak.size()), join(weak),
						s.goalSideTheirsPct <= gs[1] ? "When they win the ball, your first job is getting between it and your net."
						: s.farFromPlayPct >= far[3] ? "Keep your boost runs short: stay within half a pitch of the ball."
						: s.supportBack >= sb[3] ? "When your teammate has it in their half, follow closer behind the ball."
						: "Give your teammate room: drop back or go wide instead of sitting next to them.", {});
			}

			// --- the touches that moved the scoring chance most ---
			{
				std::vector<int> bad, great;
				for (int k = 0; k < M; k++)
				{
					const Moment& m = a.moments[k];
					if (m.kind != MomentKind::Play || m.player != pl) continue;
					(m.detail == MomentDetail::PlayWhoops || m.detail == MomentDetail::PlayBlunder ? bad : great).push_back(k);
				}
				if (bad.size() >= 2)
					add(pl, tm, false, 1.5f + bad.size() * 1.5f, std::format("{} touches handed the other team a chance", bad.size()),
						"Each one moved the scoring chance a long way against their own team.", "Watch each one and ask what the safe touch was: usually wide, or none at all.", bad);
				if (!great.empty())
					add(pl, tm, true, 1.5f + great.size() * 1.5f, great.size() == 1 ? std::string("One touch that changed the game") : std::format("{} touches that changed the game", great.size()),
						great.size() == 1 ? "It moved the scoring chance a long way their team's way." : "Each one moved the scoring chance a long way their team's way.", "", great);
			}

			// --- goals against ---
			if (goalsAgainst[tm] >= 2 && s.goalsAgainstIn >= 2 && s.goalsAgainstIn * 2 >= goalsAgainst[tm])
			{
				std::vector<int> ms;
				for (const GoalChain& ch : a.chains)
					if (ch.against == tm)
						for (int k : ch.moments) if (a.moments[k].player == pl || a.moments[k].other == pl) { ms.push_back(ch.goalMoment); break; }
				add(pl, tm, false, 3.0f + s.goalsAgainstIn * 1.5f, std::format("Had a part in {} of the {} goals against", s.goalsAgainstIn, goalsAgainst[tm]),
					"One of their mistakes came in the six seconds before each. The goals list has the chain for each one.", "Watch those goals back from six seconds out.", std::move(ms));
			}

			// --- up the pitch, slow back, standing still ---
			if (s.aheadSecs >= 0.07f * liveSecs && !mates.empty())
				add(pl, tm, false, 1.0f + s.aheadSecs / 5.0f, std::format("Up the pitch for {:.0f} s while the ball was in their own third", s.aheadSecs),
					"Waiting forward leaves the team a player short at the back.", "When the ball goes into your third, come back to at least halfway.", {});
			if (s.recoverSecs >= 3.2f)
			{
				float best = 99.0f;
				for (int o : all) if (a.stats[o].recoverSecs > 0) best = std::min(best, a.stats[o].recoverSecs);
				if (best < s.recoverSecs - 0.8f)
					add(pl, tm, false, 1.0f + s.recoverSecs / 2.0f, std::format("Slow to get back goal side: {:.1f} s on average", s.recoverSecs),
						std::format("That's after their team lost the ball. The quickest player in the match took {:.1f} s.", best), "The moment the ball is lost, turn for your own half before you look for the next touch.", {});
			}
			if (s.stoppedPct >= 9.0f)
				add(pl, tm, false, 1.0f + s.stoppedPct / 6.0f, std::format("Nearly stopped {:.0f}% of the time", s.stoppedPct), "On the ground, under walking pace, while the ball was live.",
					"Keep rolling, even slowly, so you can react without starting from a standstill.", {});

			// --- strengths from the scoresheet ---
			if (s.goals >= 2) add(pl, tm, true, 2.0f + s.goals, std::format("Scored {} from {} shots on target", s.goals, std::max(s.shots, s.goals)), "", "", momentsOf({ MomentKind::Goal }, pl));
			if (s.saves >= 3) add(pl, tm, true, 1.5f + s.saves * 0.7f, std::format("Made {} saves", s.saves), "", "", momentsOf({ MomentKind::Save }, pl));
			if (s.passes >= 3) add(pl, tm, true, 1.0f + s.passes * 0.6f, std::format("Found a teammate {} times", s.passes), "Touches a teammate got to next.", "", momentsOf({ MomentKind::Pass }, pl));
		}

		// ---- The team ----
		for (int tm = 0; tm < 2; tm++)
		{
			const char* tn = tm == 0 ? "Blue" : "Orange";
			if (a.kickoffs >= 3)
			{
				const int won = a.kickoffsWon[tm], lost = a.kickoffsWon[1 - tm];
				if (won * 10 >= a.kickoffs * 7) add(-1, tm, true, 1.5f + won, std::format("Came out ahead on {} of {} kickoffs", won, a.kickoffs), "", "", teamMoments(MomentKind::Kickoff, tm));
				else if (lost * 10 >= a.kickoffs * 7)
					add(-1, tm, false, 1.5f + lost, std::format("Lost {} of {} kickoffs", lost, a.kickoffs), "The ball was in this team's half two seconds later.",
						"Whoever isn't going: sit a little further back and read which way it's coming out.", teamMoments(MomentKind::Kickoff, 1 - tm));
			}
			if (a.teamSize > 1 && a.doubleCommits[tm] >= 3)
				add(-1, tm, false, 2.0f + a.doubleCommits[tm], std::format("{} double commits", a.doubleCommits[tm]), "Two players on the same ball, nobody for the next one.",
					"Decide by who is closer and facing it; the other holds back and covers.", teamMoments(MomentKind::DoubleCommit, tm));
			if (a.openNets[tm] >= 2)
				add(-1, tm, false, 2.0f + a.openNetSecs[tm] / 2.0f, std::format("Net left open {} times ({:.0f} s in all)", a.openNets[tm], a.openNetSecs[tm]),
					"The other team had the ball in this half with nobody between it and the goal.", "One player always stays goal side until another is back.", teamMoments(MomentKind::OpenNet, tm));
			if (a.possessionPct[tm] >= 58.0f)
				add(-1, tm, true, 1.0f + (a.possessionPct[tm] - 50.0f) / 5.0f, std::format("Had the ball {:.0f}% of the time", a.possessionPct[tm]), std::format("And it was in the other team's half {:.0f}% of the time.", a.territoryPct[tm]), "", {});
			if (a.territoryPct[tm] <= 40.0f)
				add(-1, tm, false, 1.0f + (50.0f - a.territoryPct[tm]) / 5.0f, std::format("Pinned back: the ball was in this half {:.0f}% of the time", 100.0f - a.territoryPct[tm]),
					"Clears kept coming straight back.", "When you win it at the back, look for the side wall or a teammate, not just distance.", {});
			if (a.counterGoals[1 - tm] >= 2)
				add(-1, tm, false, 2.0f + a.counterGoals[1 - tm] * 1.5f, std::format("Conceded {} goals within six seconds of losing the ball", a.counterGoals[1 - tm]),
					"Caught forward when possession changed.", "Keep one player behind the ball whenever you attack.", {});
			// the rest of the catalogue (Nov 12)
			if (a.shotsStayed[tm] >= 3 && a.shotsNoFollow[tm] * 5 >= a.shotsStayed[tm] * 2)
				add(-1, tm, false, 1.5f + a.shotsNoFollow[tm], std::format("Nobody followed up {} of {} shots", a.shotsNoFollow[tm], a.shotsStayed[tm]),
					"The ball stayed in the other team's end and the nearest teammate was back past halfway.",
					"When a teammate shoots, the next player moves up to around halfway into their end for what comes off the save.", teamMoments(MomentKind::NoFollowUp, tm));
			if (a.bunchedPct[tm] >= 10.0f)
				add(-1, tm, false, 1.0f + a.bunchedPct[tm] / 4.0f, std::format("On top of each other {:.0f}% of the time", a.bunchedPct[tm]),
					"Two teammates within two car lengths. One touch beats both.", "If your teammate fills your screen, you're too close: drop back or go wide.", {});
			// who finds whom: a pair where the passes only go one way
			for (int x = 0; x < P; x++)
				for (int y = x + 1; y < P; y++)
				{
					if (team(x) != tm || team(y) != tm) continue;
					const int xy = a.passTo[x][y], yx = a.passTo[y][x];
					const int giver = xy >= yx ? x : y, taker = xy >= yx ? y : x, more = std::max(xy, yx), less = std::min(xy, yx);
					if (more < 3 || more < 3 * less + 1) continue;
					std::vector<int> ms;
					for (int k = 0; k < M; k++) if (a.moments[k].kind == MomentKind::Pass && a.moments[k].player == giver && a.moments[k].other == taker) ms.push_back(k);
					add(-1, tm, false, 1.0f + (more - less) * 0.6f, std::format("Passes only went one way: {} found {} {} times, and got {} back", name(giver), name(taker), more, less),
						std::format("{} is doing the setting up and {} the finishing.", name(giver), name(taker)),
						std::format("{}: when you have time on the ball and {} is moving up, put it in front of them.", name(taker), name(giver)), std::move(ms));
				}
			if (a.counterGoals[tm] >= 2)
				add(-1, tm, true, 1.5f + a.counterGoals[tm], std::format("Scored {} goals within six seconds of winning the ball", a.counterGoals[tm]), "Quick to turn defence into a shot.", "", {});
			(void)tn;
		}
		std::stable_sort(a.takeaways.begin(), a.takeaways.end(), [](const Takeaway& x, const Takeaway& y) { return x.weight > y.weight; });

		// ---- The review's words (Nov 13): a line under each area, and two sentences for the whole game ----
		if (lobby >= 0)
		{
			const int ts = std::clamp(a.teamSize, 1, 3);
			auto typ = [&](int st) { return kBase[ts - 1][lobby][st][2]; };
			auto typN = [&](int st) { return kBase[ts - 1][lobby][st][2] * liveSecs / 300.0f; };   // a per-five-minutes count, for this game's length
			auto plural = [](int n, const char* one, const char* many) { return std::format("{} {}", n, n == 1 ? one : many); };
			auto carLengths = [](float uu) { return static_cast<int>(std::lround(uu / 118.0f)); };
			for (int pl = 0; pl < P; pl++)
			{
				const int tm = team(pl);
				if (tm < 0) continue;
				MatchPlayerStats& s = a.stats[pl];
				// attack
				{
					std::string w = std::format("{}, {}, {}.", plural(s.shots, "shot", "shots"), plural(s.goals, "goal", "goals"), plural(s.assists, "assist", "assists"));
					w += s.aerialsWon > 0 ? std::format(" First to {} in the air.", plural(s.aerialsWon, "ball", "balls")) : std::string(" No aerials won.");
					s.area[Area_Attack].why = w;
				}
				// defence
				{
					std::string w = std::format("Goal side {:.0f}% on their ball (typical {:.0f}%).", s.goalSideTheirsPct, typ(Base_goalSideTheirsPct));
					w += s.leftNetOpen == 0 ? " Never last out of an open net." : std::format(" Last out of an open net {}.", plural(s.leftNetOpen, "time", "times"));
					if (s.recoverSecs > 0.0f) w += std::format(" {:.1f} s to get back goal side (typical {:.1f}).", s.recoverSecs, typ(Base_recoverSecs));
					s.area[Area_Defence].why = w;
				}
				// ball use
				{
					const int hurt = s.giveDangerous + s.giveMidfield + s.giveBoomed;
					std::string w = hurt == 0 ? std::format("No cheap giveaways in {} touches.", s.touches)
						: std::format("{} straight to an opponent{}.", plural(hurt, "giveaway", "giveaways"), s.giveDangerous > 0 ? std::format(", {} from their own third", s.giveDangerous) : std::string());
					if (a.teamSize > 1) w += s.passes == 0 ? std::string(" No passes found a teammate.") : std::format(" {} found a teammate.", plural(s.passes, "pass", "passes"));
					s.area[Area_BallUse].why = w;
				}
				// positioning
				{
					std::string w = std::format("Out of the play {:.0f}% (typical {:.0f}%).", s.farFromPlayPct, typ(Base_farFromPlayPct));
					if (a.teamSize > 1)
						w = std::format("{} car lengths behind in support (typical {}). Near a teammate {:.0f}% (typical {:.0f}%).", carLengths(s.supportBack), carLengths(typ(Base_supportBack)), s.bunchedPct, typ(Base_bunchedPct));
					s.area[Area_Positioning].why = w;
				}
				// boost
				{
					std::string w = s.boostWasted < 1.0f ? std::string("No boost wasted at full pads.") : std::format("{:.0f} wasted at full pads (typical {:.0f}).", s.boostWasted, typN(Base_boostWasted));
					w += std::format(" {:.0f} s on empty (typical {:.0f}).", s.noBoostSecs, typN(Base_noBoostSecs));
					s.area[Area_Boost].why = w;
				}
				// speed
				{
					std::string w = std::format("Supersonic {:.0f}% (typical {:.0f}%).", s.supersonicPct, typ(Base_supersonicPct));
					w += s.stoppedPct <= typ(Base_stoppedPct) ? " Rarely sat still." : std::format(" Sat nearly still {:.0f}% (typical {:.0f}%).", s.stoppedPct, typ(Base_stoppedPct));
					s.area[Area_Speed].why = w;
				}

				// the two sentences: the best part of the game, then the leak. Each names the one number in that area that
				// stood out most, with what's typical.
				if (s.overall < 0) continue;
				auto signedPct = [&](int st) {
					const float p = PctOf(kBase[ts - 1][lobby][st], BaseValue(s, st, liveSecs));
					return kBaseBetter[st] < 0 ? 100.0f - p : p;
				};
				auto phrase = [&](int st) -> std::string {
					const float v = BaseValue(s, st, liveSecs) * (kBasePer5[st] ? liveSecs / 300.0f : 1.0f);
					const float t = kBasePer5[st] ? typN(st) : typ(st);
					const int n = static_cast<int>(std::lround(v)), tn = static_cast<int>(std::lround(t));
					switch (st)
					{
					case Base_shots: return std::format("{} (typical {})", plural(n, "shot", "shots"), tn);
					case Base_goals: return std::format("{} from {}", plural(n, "goal", "goals"), plural(s.shots, "shot", "shots"));
					case Base_assists: return plural(n, "assist", "assists");
					case Base_goalSideTheirsPct: return std::format("goal side {:.0f}% of the time the other team had it (typical {:.0f}%)", v, t);
					case Base_leftNetOpen: return n == 0 ? std::string("never the last one out of an open net") : std::format("last out of an open net {} (typical {})", plural(n, "time", "times"), tn);
					case Base_beaten: return n == 0 ? std::string("never beaten on the challenge") : std::format("beaten on the challenge {} (typical {})", plural(n, "time", "times"), tn);
					case Base_recoverSecs: return std::format("{:.1f} s to get back goal side after losing the ball (typical {:.1f})", v, t);
					case Base_shadows: return std::format("shadowed the ball carrier {}", plural(n, "time", "times"));
					case Base_giveaways: return std::format("{} (typical {})", plural(n, "giveaway", "giveaways"), tn);
					case Base_giveDangerous: return n == 0 ? std::format("nothing given away in their own third in {} touches", s.touches) : std::format("{} given away in their own third (typical {})", n, tn);
					case Base_passes: return std::format("{} found a teammate (typical {})", plural(n, "pass", "passes"), tn);
					case Base_farFromPlayPct: return std::format("out of the play {:.0f}% of the time (typical {:.0f}%)", v, t);
					case Base_doubleCommits: return std::format("{} (typical {})", plural(n, "double commit", "double commits"), tn);
					case Base_bunchedPct: return std::format("near a teammate {:.0f}% of the time (typical {:.0f}%)", v, t);
					case Base_supportBack: return std::format("{} car lengths behind the ball in support (typical {})", carLengths(v), carLengths(t));
					case Base_boostWasted: return n == 0 ? std::string("no boost wasted at full pads") : std::format("{} boost wasted at full pads (typical {})", n, tn);
					case Base_noBoostSecs: return std::format("{} s on empty (typical {})", n, tn);
					case Base_supersonicBoost: return std::format("{} boost burned at top speed (typical {})", n, tn);
					case Base_smallPads: return std::format("{} (typical {})", plural(n, "small pad", "small pads"), tn);
					case Base_supersonicPct: return std::format("supersonic {:.0f}% of the time (typical {:.0f}%)", v, t);
					case Base_avgSpeed: return std::format("average speed {:.0f} km/h (typical {:.0f})", v * 0.036f, t * 0.036f);
					case Base_stoppedPct: return std::format("sat nearly still {:.0f}% of the time (typical {:.0f}%)", v, t);
					default: return std::string();
					}
				};
				auto standout = [&](int ar, bool bestOne) {   // the number in an area furthest from typical, the way asked
					int pick = -1; float pv = bestOne ? -1.0f : 101.0f;
					for (int k = 0; k < kAreaN[ar]; k++)
					{
						const int st = kAreaStat[ar][k];
						if ((kAreaTeam[ar][k] && ts == 1) || st == Base_posLevel || (st == Base_recoverSecs && s.recoverSecs <= 0.0f)) continue;
						if (bestOne && (st == Base_stoppedPct || st == Base_avgSpeed)) continue;   // not much to praise in either
						if (!bestOne && st == Base_bunchedPct && BaseValue(s, st, liveSecs) < kBase[ts - 1][lobby][st][4]) continue;   // a tight spread: only worth naming when well off
						const float p = signedPct(st);
						if (bestOne ? p > pv : p < pv) { pv = p; pick = st; }
					}
					return pick;
				};
				int best = 0, worst = 0;
				for (int ar = 1; ar < Area_Count; ar++)
				{
					if (s.area[ar].score > s.area[best].score) best = ar;
					if (s.area[ar].score < s.area[worst].score) worst = ar;
				}
				bool topImpact = true;
				for (int o = 0; o < P; o++) if (o != pl && team(o) >= 0 && a.stats[o].chanceAdded > s.chanceAdded) topImpact = false;
				const int hurt = s.giveDangerous + s.giveMidfield + s.giveBoomed;
				// the strong part: the best-scoring area whose standout number is clearly good, not just its area's average
				int bs = -1;
				{
					std::vector<int> order(Area_Count);
					for (int k = 0; k < Area_Count; k++) order[k] = k;
					std::sort(order.begin(), order.end(), [&](int x, int y) { return s.area[x].score > s.area[y].score; });
					for (int ar : order)
					{
						if (s.area[ar].grade > ReviewGrade::Clean) break;
						const int st = standout(ar, true);
						if (st >= 0 && signedPct(st) >= 65.0f) { best = ar; bs = st; break; }
					}
				}
				const int ws = standout(worst, false);
				std::string first, second;
				if (s.impactPct >= 75.0f)
					first = std::string("Big game on the ball: touches that added ") + (topImpact ? "the most chance in the lobby" : std::format("more chance than most {} players", LevelName(lobby)))
						+ (hurt == 0 && s.touches >= 10 ? ", and nothing given away cheaply." : ".");
				else if (bs >= 0)
					first = std::format("{} was the strong part: {}.", AreaName(best), phrase(bs));
				else if (s.impactPct <= 25.0f)
					first = "A quiet game on the ball: few touches moved the scoring chance either way.";
				else
					first = std::format("A steady game for a {} lobby, nothing far off the usual.", LevelName(lobby));
				if ((s.area[worst].grade >= ReviewGrade::Sloppy || s.area[worst].score < 35.0f) && ws >= 0)
					second = ws == Base_goalSideTheirsPct || ws == Base_recoverSecs ? std::format("The leak was getting back: {}.", phrase(ws))
						: std::format("The leak was {}: {}.", Lower(AreaName(worst)), phrase(ws));
				else if (s.area[worst].score < 50.0f && ws >= 0)
					second = std::format("The thing to tidy up: {}.", phrase(ws));
				s.summary = second.empty() ? first : first + " " + second;
			}
		}
	}

	a.ok = true;
	return a;
}

bool Match::MakeShotRep(const MatchData& d, int player, int frameNumber, ShotRep& out, std::string& err)
{
	out = ShotRep{};
	const int N = static_cast<int>(d.frames.size()), P = static_cast<int>(d.players.size());
	if (player < 0 || player >= P || N < 60) { err = "No such player in this match"; return false; }
	const int team = d.players[player].team;
	const float flip = team == 1 ? -1.0f : 1.0f;   // orange is turned around
	auto indexOf = [&](int frame) {
		auto it = std::lower_bound(d.frames.begin(), d.frames.end(), frame, [](const MatchFrame& f, int v) { return f.frame < v; });
		return std::clamp(static_cast<int>(it - d.frames.begin()), 0, N - 1);
	};
	// this player's touches, as places in `frames` (repeats folded)
	std::vector<int> mine, others, othersWho;
	{
		int lastMine = -1000, lastOther = -1000;
		for (const MatchData::Touch& t : d.touches)
		{
			const int idx = indexOf(t.frame);
			if (t.player == player) { if (idx - lastMine > 3) mine.push_back(idx); lastMine = idx; }
			else { if (idx - lastOther > 3) { others.push_back(idx); othersWho.push_back(t.player); } lastOther = idx; }
		}
	}
	// the shot's touch: this player's last touch at or just before the moment
	const int at = indexOf(frameNumber);
	int shot = -1;
	for (int idx : mine) if (idx <= at + 2) shot = idx;
	if (shot < 0 || d.frames[at].time - d.frames[shot].time > 8.0f) { err = "Couldn't find that player's touch for this moment"; return false; }
	const float shotTime = d.frames[shot].time;
	// from: up to five seconds before, but not across a kickoff wait, a goal, or a skip in the replay
	int from = shot;
	while (from > 0 && shotTime - d.frames[from - 1].time <= 5.0f)
	{
		const MatchFrame& a = d.frames[from - 1];
		const MatchFrame& b = d.frames[from];
		if (b.time - a.time > 0.25f || a.score[0] != b.score[0] || a.score[1] != b.score[1] || !a.cars[player].present) break;
		if (a.hasBall && std::fabs(a.bx) < 2.0f && std::fabs(a.by) < 2.0f && std::fabs(a.bvx) < 2.0f && std::fabs(a.bvy) < 2.0f) break;   // on the centre spot
		from--;
	}
	// to: the goal, someone else's touch, or three seconds on (plus a little after, like a rep's recovery)
	int to = shot, event = -1;
	std::string reason = "miss";
	while (to + 1 < N && d.frames[to + 1].time - shotTime <= 5.0f)
	{
		const MatchFrame& a = d.frames[to];
		const MatchFrame& b = d.frames[to + 1];
		if (b.time - a.time > 0.25f || !b.cars[player].present) break;
		if (a.score[0] != b.score[0] || a.score[1] != b.score[1])
		{
			if (b.score[team] > a.score[team]) { out.goal = true; reason = "goal"; }
			event = to;
			break;
		}
		to++;
	}
	if (!out.goal)
	{
		// no goal: it ends at the first touch by anyone else (a save or a block), if there was one
		for (size_t o = 0; o < others.size(); o++)
		{
			const int idx = others[o];
			if (idx > shot + 2 && idx <= to) { event = idx; to = std::min(N - 1, idx + 6); reason = "saved"; out.stoppedBy = othersWho[o]; break; }
		}
	}
	if (event < 0) event = to;
	if (to - from < 20) { err = "Not enough of the replay around that touch"; return false; }
	out.endReason = reason;

	// ---- 120 ticks a second, filled in between the replay's frames ----
	const float t0 = d.frames[from].time, t1 = d.frames[to].time;
	const int ticks = static_cast<int>((t1 - t0) * 120.0f) + 1;
	out.frames.reserve(ticks);
	auto lerp = [](float a, float b, float u) { return a + (b - a) * u; };
	auto lerpRot = [](int a, int b, float u) {   // the short way round (65536 = a full turn)
		int dlt = (b - a) % 65536;
		if (dlt > 32768) dlt -= 65536;
		if (dlt < -32768) dlt += 65536;
		return a + static_cast<int>(std::lround(dlt * u));
	};
	// Where the ball was hit inside a replay frame's gap. Between two frames the ball flies freely unless something
	// hit it; when its speed after doesn't follow from its speed before, it was hit, at about the moment the path
	// forward from the first frame and the path back from the second come closest. Before that moment the ball is
	// carried forward from the first frame, after it carried back from the second: a sharp touch, not a blur.
	constexpr float kGravity = -650.0f;
	std::vector<float> hitAt(static_cast<size_t>(to - from + 1), -1.0f);   // seconds after frame i, or -1
	for (int k = from; k < to; k++)
	{
		const MatchFrame& a = d.frames[k];
		const MatchFrame& b = d.frames[k + 1];
		const float span = b.time - a.time;
		if (span <= 1e-4f || span > 0.25f) continue;
		const float dvx = b.bvx - a.bvx, dvy = b.bvy - a.bvy, dvz = b.bvz - (a.bvz + kGravity * span);
		if (std::sqrt(dvx * dvx + dvy * dvy + dvz * dvz) < 200.0f) continue;
		// a.p + a.v s  =  b.p - b.v (span - s)   ->  least squares for s
		const float rx = b.bx - b.bvx * span - a.bx, ry = b.by - b.bvy * span - a.by, rz = b.bz - b.bvz * span - a.bz;
		const float wx = a.bvx - b.bvx, wy = a.bvy - b.bvy, wz = a.bvz - b.bvz;
		const float ww = wx * wx + wy * wy + wz * wz;
		hitAt[static_cast<size_t>(k - from)] = ww > 1.0f ? std::clamp((rx * wx + ry * wy + rz * wz) / ww, 0.0f, span) : span * 0.5f;
	}
	int i = from;
	bool hasFlip = true;
	for (int k = 0; k < ticks; k++)
	{
		const float t = t0 + k / 120.0f;
		while (i + 1 < to && d.frames[i + 1].time <= t) i++;
		const MatchFrame& a = d.frames[i];
		const MatchFrame& b = d.frames[std::min(i + 1, to)];
		const float span = b.time - a.time, u = span > 1e-4f ? std::clamp((t - a.time) / span, 0.0f, 1.0f) : 0.0f;
		const MatchCar& ca = a.cars[player];
		const MatchCar& cb = b.cars[player];
		Frame f;
		f.tick = k;
		f.t = k / 120.0f;
		f.wallMs = f.t * 1000.0f;
		f.throttle = ca.throttle;
		f.steer = ca.steer;   // the car's own left and right: turning the pitch around doesn't change it
		f.handbrake = ca.handbrake;
		f.carX = lerp(ca.x, cb.x, u) * flip; f.carY = lerp(ca.y, cb.y, u) * flip; f.carZ = lerp(ca.z, cb.z, u);
		f.carPitch = lerpRot(ca.pitch, cb.pitch, u);
		f.carYaw = lerpRot(ca.yaw, cb.yaw, u) + (team == 1 ? 32768 : 0);
		if (f.carYaw > 32767) f.carYaw -= 65536;
		f.carRoll = lerpRot(ca.roll, cb.roll, u);
		f.carVX = lerp(ca.vx, cb.vx, u) * flip; f.carVY = lerp(ca.vy, cb.vy, u) * flip; f.carVZ = lerp(ca.vz, cb.vz, u);
		f.carAVX = lerp(ca.avx, cb.avx, u) * flip; f.carAVY = lerp(ca.avy, cb.avy, u) * flip; f.carAVZ = lerp(ca.avz, cb.avz, u);
		f.boostAmount = lerp(ca.boost, cb.boost, u);
		f.boost = cb.boost < ca.boost - 0.01f;   // boost going down = the button held
		f.onGround = u < 0.5f ? ca.onGround : cb.onGround;
		const MatchCar& near = u < 0.5f ? ca : cb;
		f.onWall = near.onWall >= 0 ? near.onWall != 0 : (near.onGround && near.z > 150.0f);
		if (f.onWall) f.onGround = false;   // a rep keeps the two apart: on the wall isn't "on the ground"
		const float hit = i < to ? hitAt[static_cast<size_t>(i - from)] : -1.0f;
		if (hit >= 0.0f)
		{
			const float s = t - a.time;
			if (s <= hit)
			{
				f.ballX = (a.bx + a.bvx * s) * flip; f.ballY = (a.by + a.bvy * s) * flip; f.ballZ = a.bz + a.bvz * s + 0.5f * kGravity * s * s;
				f.ballVX = a.bvx * flip; f.ballVY = a.bvy * flip; f.ballVZ = a.bvz + kGravity * s;
			}
			else
			{
				const float back = span - s;   // seconds before frame b
				f.ballX = (b.bx - b.bvx * back) * flip; f.ballY = (b.by - b.bvy * back) * flip; f.ballZ = b.bz - b.bvz * back + 0.5f * kGravity * back * back;
				f.ballVX = b.bvx * flip; f.ballVY = b.bvy * flip; f.ballVZ = b.bvz - kGravity * back;
			}
		}
		else
		{
			f.ballX = lerp(a.bx, b.bx, u) * flip; f.ballY = lerp(a.by, b.by, u) * flip; f.ballZ = lerp(a.bz, b.bz, u);
			f.ballVX = lerp(a.bvx, b.bvx, u) * flip; f.ballVY = lerp(a.bvy, b.bvy, u) * flip; f.ballVZ = lerp(a.bvz, b.bvz, u);
		}
		out.frames.push_back(f);
	}
	// The jump button, worked out from what the car did (a replay doesn't keep it, and the game's own "jumped" flags
	// stay off in a replay). Leaving the ground with a kick upward is a jump, held for a fifth of a second. In the
	// air, a second kick upward is a double jump and a sudden spin is a flip: either way a short second press.
	auto tickOf = [&](int idx) { return std::clamp(static_cast<int>(std::lround((d.frames[idx].time - t0) * 120.0f)), 0, ticks - 1); };
	std::vector<char> usedSecond(static_cast<size_t>(to - from + 2), 0);
	bool second = false;   // the second jump is spent until the car lands
	for (int idx = from + 1; idx <= to; idx++)
	{
		const MatchCar& was = d.frames[idx - 1].cars[player];
		const MatchCar& now = d.frames[idx].cars[player];
		const float span = d.frames[idx].time - d.frames[idx - 1].time;
		if (span <= 1e-4f || span > 0.25f) continue;
		const int tk = tickOf(idx), prevTk = tickOf(idx - 1);
		const bool wasDown = was.onGround, nowDown = now.onGround;
		if (nowDown) second = false;
		const float kickUp = now.vz - (was.vz + (wasDown ? 0.0f : kGravity * span));
		const float spinWas = std::sqrt(was.avx * was.avx + was.avy * was.avy + was.avz * was.avz), spinNow = std::sqrt(now.avx * now.avx + now.avy * now.avy + now.avz * now.avz);
		bool touching = false;
		for (int m : mine) if (std::abs(m - idx) <= 1) touching = true;
		if (wasDown && !nowDown && kickUp > 180.0f && was.z < 60.0f)
		{
			for (int k = prevTk + 1; k <= std::min(ticks - 1, prevTk + 24); k++) out.frames[k].jump = true;
		}
		else if (!wasDown && !nowDown && !second && !touching && (kickUp > 200.0f || spinNow - spinWas > 3.0f))
		{
			second = true;
			usedSecond[static_cast<size_t>(idx - from)] = 1;
			for (int k = std::max(0, tk - 4); k < tk; k++) out.frames[k].jump = false;   // a press needs a gap before it
			for (int k = tk; k <= std::min(ticks - 1, tk + 2); k++) out.frames[k].jump = true;
		}
	}
	for (int k = 0; k < ticks; k++)
	{
		Frame& f = out.frames[k];
		const float t = t0 + k / 120.0f;
		int idx = from;
		{
			auto it = std::upper_bound(d.frames.begin() + from, d.frames.begin() + to + 1, t, [](float v, const MatchFrame& m) { return v < m.time; });
			idx = std::clamp(static_cast<int>(it - d.frames.begin()) - 1, from, to);
		}
		const MatchCar& c = d.frames[idx].cars[player];
		if (c.hasFlip >= 0) hasFlip = c.hasFlip != 0;
		else
		{
			if (f.onGround || f.onWall) hasFlip = true;
			else if (usedSecond[static_cast<size_t>(idx - from)]) hasFlip = false;
		}
		f.hasFlip = hasFlip;
	}
	// this player's touches inside the window
	for (int idx : mine)
		if (idx >= from && idx <= to)
		{
			// the gap the hit was in: the one at or just before the game's report with the biggest change
			int tk = std::min(ticks - 1, tickOf(idx) + 1);
			for (int g = std::min(idx, to - 1); g >= std::max(from, idx - 2); g--)
				if (hitAt[static_cast<size_t>(g - from)] >= 0.0f)
				{
					tk = std::min(ticks - 1, static_cast<int>((d.frames[g].time + hitAt[static_cast<size_t>(g - from)] - t0) * 120.0f) + 1);   // a rep's touch lands on the tick after contact
					break;
				}
			Frame& f = out.frames[tk];
			f.touch = true;
			f.touchX = f.ballX; f.touchY = f.ballY; f.touchZ = f.ballZ;
		}
	out.eventTick = std::clamp(tickOf(event), 0, ticks - 1);
	return true;
}
