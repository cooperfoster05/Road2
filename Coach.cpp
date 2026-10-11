#include "pch.h"
#include "Coach.h"
#include "Grader.h"
#include "RepLibrary.h"
#include "Units.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace
{
	constexpr float kPi = 3.14159265f;
	constexpr float kGravity = 650.0f;

	// How a check's number is shown on hover
	enum class Shown { Plain, Speed, Percent };

	struct Check
	{
		const char* id;
		ShotType type;
		int order;            // position in the mechanic's chain: lower = earlier
		bool firesAbove;      // true: fires when the value is above the threshold; false: below
		float threshold;
		const char* cue;
		const char* detail;   // {} = the value
		Shown shown;
	};

	// Thresholds start from the coaching advice and were set where Coop's misses and goals split (Oct 11).
	// Sources: r/RocketLeagueSchool, Dot Esports and Dignitas guides, Steam air dribble thread (see design doc).
	const Check kList[Coach::kChecks] = {
		// Flip resets: pop -> approach -> the reset -> the flip -> the shot
		{ "fr_pop_low", ShotType::FlipReset, 1, false, 1550.0f,
			"Pop it higher off the wall so you have time to get under it", "Ball peaked about {} high off your pop", Shown::Plain },
		{ "fr_rushed", ShotType::FlipReset, 2, true, 500.0f,
			"Don't rush the reset: match the ball's speed and float into it", "Closing on the ball at {} right before the reset", Shown::Speed },
		{ "fr_boost_in", ShotType::FlipReset, 2, true, 0.45f,
			"Ease off boost as you reach the ball for the reset", "Boosting {} of the half second before the reset", Shown::Percent },
		{ "fr_low_reset", ShotType::FlipReset, 3, false, 1450.0f,
			"Get the reset higher up: you met the ball too close to the floor", "Got the reset {} high", Shown::Plain },
		{ "fr_flip_far", ShotType::FlipReset, 4, true, 300.0f,
			"Wait until the ball is in reach before you flip", "Flipped with the ball {} away", Shown::Plain },
		{ "fr_slow_shot", ShotType::FlipReset, 5, false, 1300.0f,
			"Carry more speed into the shot", "Hit it at {}", Shown::Speed },
		// Aerials
		{ "ae_hold_boost", ShotType::Aerial, 1, true, 0.80f,
			"Feather your boost instead of holding it", "Held boost for {} of the flight", Shown::Percent },
		// Double taps: setup -> read
		{ "dt_high_board", ShotType::DoubleTap, 1, true, 1200.0f,
			"Hit the backboard lower so it comes back to you sooner", "Ball hit the backboard {} high", Shown::Plain },
		{ "dt_flew_up", ShotType::DoubleTap, 2, true, 950.0f,
			"Don't fly up with the ball: wait under where it comes off the backboard", "You were {} high when it hit the backboard", Shown::Plain },
		// Dribbles: the carry into the flick
		{ "dr_ball_back", ShotType::Dribble, 1, false, 40.0f,
			"Get the ball further forward on your roof before you jump to flick", "Ball was {} ahead of your car's center when you jumped", Shown::Plain },
		// Air dribbles: the touches
		{ "ad_off_center", ShotType::AirDribble, 1, true, 65.0f,
			"Keep the ball centered on your nose", "Ball sat {} off-center at your touches", Shown::Plain },
		{ "ad_hard_touch", ShotType::AirDribble, 1, true, 800.0f,
			"Softer touches: feather boost so you don't knock it away", "Each touch pushed it {} on average", Shown::Speed },
		{ "ad_falling", ShotType::AirDribble, 1, true, 0.74f,
			"Touch the ball on its way up, not as it falls", "{} of your touches came as the ball was falling", Shown::Percent },
	};

	// Car axes from the recorded rotation (Unreal units: 65536 = a full turn)
	void Axes(const Frame& f, float fw[3], float up[3], float rt[3])
	{
		float p = f.carPitch * kPi / 32768.0f, y = f.carYaw * kPi / 32768.0f, r = f.carRoll * kPi / 32768.0f;
		float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
		fw[0] = cp * cy; fw[1] = cp * sy; fw[2] = sp;
		up[0] = -(cr * sp * cy + sr * sy); up[1] = cy * sr - cr * sp * sy; up[2] = cr * cp;
		rt[0] = fw[1] * up[2] - fw[2] * up[1]; rt[1] = fw[2] * up[0] - fw[0] * up[2]; rt[2] = fw[0] * up[1] - fw[1] * up[0];
	}
	// Where the ball is relative to your car: forward, up and sideways (car's own directions)
	void BallOnCar(const Frame& f, float& fwd, float& upv, float& side)
	{
		float fw[3], up[3], rt[3];
		Axes(f, fw, up, rt);
		float d[3] = { f.ballX - f.carX, f.ballY - f.carY, f.ballZ - f.carZ };
		fwd = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
		upv = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
		side = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2];
	}
	float Dist(const Frame& f)
	{
		float dx = f.ballX - f.carX, dy = f.ballY - f.carY, dz = f.ballZ - f.carZ;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	float Speed(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

	std::string Show(const Check& c, float v)
	{
		std::string num = c.shown == Shown::Speed ? Units::Speed(v)
			: c.shown == Shown::Percent ? std::format("{:.0f}%", v * 100.0f)
			: std::format("{:.0f}", v);
		std::string s = c.detail;
		size_t at = s.find("{}");
		if (at != std::string::npos) s.replace(at, 2, num);
		return s;
	}

	std::string CachePath(const std::filesystem::path& root) { return (root / "coach_cache.csv").string(); }
	std::string CacheHeader() { return std::format("file,type,values({}),v{}", Coach::kChecks, Grader::kVersion); }
}

const char* Coach::CheckId(int c) { return (c >= 0 && c < kChecks) ? kList[c].id : ""; }

Coach::Values Coach::Measure(const std::vector<Frame>& fr, const ShotAnalysis& s)
{
	Values v;
	v.fill(-1.0f);
	const int n = static_cast<int>(fr.size());
	const int k = s.touchTick;
	if (!s.valid || k < 2 || k + 3 >= n) return v;

	switch (s.type)
	{
	case ShotType::FlipReset:
	{
		int rt = s.resetTick;
		if (rt < 1) break;
		// The pop: your last touch before leaving the wall or ground on the way to the reset
		int pop = -1;
		for (int i = 1; i <= std::min(s.resetTakeoff + 2, n - 1); i++)
			if (fr[i].touch && !fr[i - 1].touch) pop = i;
		if (pop >= 0 && pop + 4 < n)
		{
			const Frame& a = fr[pop + 4];
			v[0] = a.ballZ + std::max(0.0f, a.ballVZ) * std::max(0.0f, a.ballVZ) / (2.0f * kGravity);
		}
		// Closing speed into the reset: how fast you were coming at the ball, along the line to it
		const Frame& pre = fr[std::max(0, rt - 6)];
		float d = std::max(1.0f, Dist(pre));
		v[1] = ((pre.carVX - pre.ballVX) * (pre.ballX - pre.carX) + (pre.carVY - pre.ballVY) * (pre.ballY - pre.carY)
			+ (pre.carVZ - pre.ballVZ) * (pre.ballZ - pre.carZ)) / d;
		int from = std::max(std::max(0, s.resetTakeoff), rt - 60);
		int boost = 0;
		for (int i = from; i < rt; i++) if (fr[i].boost) boost++;
		v[2] = rt > from ? static_cast<float>(boost) / (rt - from) : -1.0f;
		v[3] = fr[rt].carZ;
		if (s.resetDodge >= 0) v[4] = Dist(fr[s.resetDodge]);
		v[5] = Speed(fr[k].carVX, fr[k].carVY, fr[k].carVZ);
		break;
	}
	case ShotType::Aerial:
	{
		int takeoff = k - 1;
		while (takeoff > 0 && !ShotGeometry::OnSurface(fr[takeoff])) takeoff--;
		takeoff++;
		if (k - takeoff >= 12)
		{
			int boost = 0;
			for (int i = takeoff; i < k; i++) if (fr[i].boost) boost++;
			v[6] = static_cast<float>(boost) / (k - takeoff);
		}
		break;
	}
	case ShotType::DoubleTap:
		if (s.backboardTick >= 0)
		{
			v[7] = s.backboardZ;
			v[8] = fr[s.backboardTick].carZ;
		}
		break;
	case ShotType::Dribble:
		if (s.flickJumpTick >= 0)
		{
			float fwd, upv, side;
			BallOnCar(fr[s.flickJumpTick], fwd, upv, side);
			v[9] = std::max(0.0f, fwd);
		}
		break;
	case ShotType::AirDribble:
	{
		// Your touches in the air before the final one (reports a few ticks apart are one touch)
		std::vector<int> touches;
		for (int i = std::max(1, s.airFirstTouch); i < k; i++)
			if (fr[i].touch && !fr[i - 1].touch && !ShotGeometry::OnSurface(fr[i]) && (touches.empty() || i - touches.back() > 8))
				touches.push_back(i);
		while (!touches.empty() && k - touches.back() <= 8) touches.pop_back();
		if (touches.empty()) break;
		float sideSum = 0;
		int falling = 0;
		for (int t : touches)
		{
			float fwd, upv, side;
			BallOnCar(fr[t - 1], fwd, upv, side);
			sideSum += std::fabs(side);
			if (fr[t - 1].ballVZ < -150.0f) falling++;
		}
		v[10] = sideSum / touches.size();
		v[11] = s.airImpulse;
		v[12] = static_cast<float>(falling) / touches.size();
		break;
	}
	default:
		break;
	}
	return v;
}

std::vector<CoachNote> Coach::Combine(const std::vector<int>& firing, const Values& v, int overall,
	const std::vector<HabitNote>& habits)
{
	std::vector<CoachNote> notes;
	if (overall >= 4) return notes;
	const char* first = overall <= 2 ? "Root cause:" : "Fix first:";
	for (int c : firing)
	{
		if (notes.size() >= 2) break;
		notes.push_back(CoachNote{ notes.empty() ? first : "Also:", kList[c].cue, Show(kList[c], v[c]), notes.empty() });
	}
	for (const HabitNote& h : habits)
	{
		if (notes.size() >= 2) break;
		notes.push_back(CoachNote{ notes.empty() ? first : "Also:", h.cue, h.detail, notes.empty() });
	}
	return notes;
}

void CoachBook::Update(const std::filesystem::path& root)
{
	std::unordered_map<std::string, Entry> cache;
	bool cacheOk = false;
	{
		std::ifstream in(CachePath(root));
		std::string line;
		if (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			cacheOk = line == CacheHeader();
		}
		while (cacheOk && std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::vector<std::string> p;
			std::stringstream ss(line);
			std::string cell;
			while (std::getline(ss, cell, ',')) p.push_back(cell);
			if (p.size() < 2 + static_cast<size_t>(Coach::kChecks)) continue;
			Entry e;
			e.type = std::atoi(p[1].c_str());
			for (int c = 0; c < Coach::kChecks; c++) e.v[c] = std::strtof(p[2 + c].c_str(), nullptr);
			cache[p[0]] = e;
		}
	}
	std::ofstream out;
	if (cacheOk) out.open(CachePath(root), std::ios::app);
	else
	{
		out.open(CachePath(root), std::ios::trunc);
		if (out) out << CacheHeader() << '\n';
	}

	// Tally each check on your rated reps of its mechanic: misses (1-2) vs goals you rated 3+
	std::array<int, Coach::kChecks> nBad{}, nBadFire{}, nGood{}, nGoodFire{};
	for (const RepListItem& item : RepLibrary::ListReps(root))
	{
		if (item.rating <= 0 || item.imported || item.endReason == "reset") continue;
		auto it = cache.find(item.file);
		if (it == cache.end())
		{
			if (!item.hasRecording) continue;
			std::vector<Frame> frames;
			std::string err;
			if (!RepLibrary::LoadRep(root, item.file, frames, err)) continue;
			ShotAnalysis shot = ShotGeometry::Analyze(frames);
			if (!shot.valid) continue;
			Entry e;
			e.type = static_cast<int>(shot.type);
			e.v = Coach::Measure(frames, shot);
			it = cache.emplace(item.file, e).first;
			if (out)
			{
				out << item.file << ',' << e.type;
				for (float x : e.v) out << ',' << std::format("{:.3f}", x);
				out << '\n';
			}
		}
		bool bad = !item.goal && item.rating <= 2;
		bool good = item.goal && item.rating >= 3;
		if (!bad && !good) continue;
		for (int c = 0; c < Coach::kChecks; c++)
		{
			if (static_cast<int>(kList[c].type) != it->second.type) continue;
			float x = it->second.v[c];
			if (x < 0) continue;
			bool fires = kList[c].firesAbove ? x > kList[c].threshold : x < kList[c].threshold;
			if (bad) { nBad[c]++; if (fires) nBadFire[c]++; }
			else { nGood[c]++; if (fires) nGoodFire[c]++; }
		}
	}

	// Backed = enough misses to judge, fires on at least 5 of them, and 1.5x as often as on your goals
	std::array<bool, Coach::kChecks> fresh{};
	for (int c = 0; c < Coach::kChecks; c++)
	{
		if (nBad[c] < 15 || nGood[c] < 10 || nBadFire[c] < 5) continue;
		double badRate = static_cast<double>(nBadFire[c]) / nBad[c];
		double goodRate = static_cast<double>(nGoodFire[c]) / nGood[c];
		fresh[c] = badRate >= 1.5 * goodRate;
	}
	std::lock_guard<std::mutex> lock(mtx);
	backed = fresh;
	badN = nBad;
	badFire = nBadFire;
	goodN = nGood;
	goodFire = nGoodFire;
	ready = true;
}

std::vector<std::string> CoachBook::Summary()
{
	std::vector<std::string> out;
	std::lock_guard<std::mutex> lock(mtx);
	if (!ready) return out;
	for (int c = 0; c < Coach::kChecks; c++)
	{
		out.push_back(std::format("{} {}: misses {}/{}, goals {}/{}  - {}", backed[c] ? "[on] " : "[off]", kList[c].cue,
			badFire[c], badN[c], goodFire[c], goodN[c],
			backed[c] ? "backed by your reps" : badN[c] < 15 ? "not enough rated misses yet" : "your reps don't back it"));
	}
	return out;
}

std::vector<int> CoachBook::Firing(const ShotAnalysis& shot, const Coach::Values& v)
{
	std::vector<int> out;
	std::lock_guard<std::mutex> lock(mtx);
	if (!ready || !shot.valid) return out;
	for (int c = 0; c < Coach::kChecks; c++)
	{
		if (!backed[c] || kList[c].type != shot.type || v[c] < 0) continue;
		bool fires = kList[c].firesAbove ? v[c] > kList[c].threshold : v[c] < kList[c].threshold;
		if (fires) out.push_back(c);
	}
	std::stable_sort(out.begin(), out.end(), [](int a, int b) { return kList[a].order < kList[b].order; });
	return out;
}

std::vector<CoachNote> CoachBook::Notes(const std::vector<Frame>& frames, const ShotAnalysis& shot, int overall, HabitBook& habits)
{
	if (overall >= 4) return {};
	Coach::Values v = Coach::Measure(frames, shot);
	std::vector<int> firing = Firing(shot, v);
	std::vector<HabitNote> own = habits.Notes(shot, Habits::Measure(frames, shot));
	return Coach::Combine(firing, v, overall, own);
}
