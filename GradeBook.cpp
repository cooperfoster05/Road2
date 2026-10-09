#include "pch.h"
#include "GradeBook.h"
#include "RepLibrary.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
	std::vector<std::string> Split(const std::string& line)
	{
		std::vector<std::string> out;
		std::string cur;
		std::stringstream ss(line);
		while (std::getline(ss, cur, ',')) out.push_back(cur);
		if (!line.empty() && line.back() == ',') out.push_back("");
		return out;
	}
	float F(const std::string& s) { return std::strtof(s.c_str(), nullptr); }
}

GradeRecord GradeBook::Make(const RepGrade& g, const ShotAnalysis& shot, const RepSegments& segs,
	const std::string& shotKey, const std::string& refFile, bool bailed)
{
	GradeRecord r;
	r.overall = bailed ? 0 : g.overall;
	r.shotKey = shotKey;
	r.refFile = refFile;
	if (shot.valid)
	{
		r.aimErr = shot.aimErrDeg;
		r.missDist = shot.missDist;
		r.shotSpeed = shot.shotSpeed;
		r.crossZ = shot.reachesGoalLine ? shot.crossZ : -1.0f;
		r.lateBy = shot.lateBy;
	}
	for (const SegmentGrade& sg : g.segs)
	{
		if (sg.grade == Grade::None || sg.segIndex < 0 || sg.segIndex >= static_cast<int>(segs.segs.size())) continue;
		if (!r.segGrades.empty()) r.segGrades += ';';
		r.segGrades += std::format("{}={}", Segmenter::Name(segs.segs[sg.segIndex].kind), Grader::Name(sg.grade));
	}
	return r;
}

void GradeBook::Load(const std::filesystem::path& root)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (loaded) return;
	}
	std::unordered_map<std::string, GradeRecord> fromDisk;
	std::ifstream in(root / "auto_grades.csv");
	std::string line;
	std::getline(in, line); // header
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		auto p = Split(line);
		if (p.size() < 10) continue;
		GradeRecord r;
		r.overall = std::atoi(p[1].c_str());
		r.shotKey = p[2];
		r.refFile = p[3];
		r.aimErr = F(p[4]); r.missDist = F(p[5]); r.shotSpeed = F(p[6]); r.crossZ = F(p[7]); r.lateBy = F(p[8]);
		r.segGrades = p[9];
		fromDisk[p[0]] = r;
	}
	std::lock_guard<std::mutex> lock(mtx);
	for (auto& [file, r] : fromDisk) grades.emplace(file, r);   // anything graded meanwhile wins
	loaded = true;
}

void GradeBook::Save(const std::filesystem::path& root)
{
	std::lock_guard<std::mutex> saveLock(saveMtx);
	std::unordered_map<std::string, GradeRecord> copy;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (!dirty || !loaded) return;
		copy = grades;
		dirty = false;
	}
	auto path = root / "auto_grades.csv";
	auto tmp = root / "auto_grades.csv.tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << "file,overall,shot_key,ref_file,aim_err,miss_dist,shot_speed,cross_z,late_by,seg_grades\n";
		for (const auto& [file, r] : copy)
			out << std::format("{},{},{},{},{:.2f},{:.1f},{:.0f},{:.0f},{:.3f},{}\n", file, r.overall, r.shotKey, r.refFile,
				r.aimErr, r.missDist, r.shotSpeed, r.crossZ, r.lateBy, r.segGrades);
		if (!out) { std::lock_guard<std::mutex> lock(mtx); dirty = true; return; }
	}
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec) { std::lock_guard<std::mutex> lock(mtx); dirty = true; }
}

void GradeBook::Refresh(const std::filesystem::path& root, ShotBook& book)
{
	std::lock_guard<std::mutex> refreshLock(refreshMtx);
	Load(root);
	for (const RepListItem& item : RepLibrary::ListReps(root))
	{
		bool bailed = item.endReason == "reset";
		ShotReference ref = book.Get(item.shotKey);
		{
			std::lock_guard<std::mutex> lock(mtx);
			auto it = grades.find(item.file);
			if (it != grades.end() && (bailed || it->second.refFile == ref.bestFile)) continue;
			if (bailed || !item.hasRecording)
			{
				// Bailed reps aren't graded; a cleaned-up rep keeps whatever grade it had
				if (it == grades.end()) { GradeRecord r; r.shotKey = item.shotKey; grades[item.file] = r; dirty = true; }
				continue;
			}
		}

		std::vector<Frame> frames;
		std::string err;
		if (!RepLibrary::LoadRep(root, item.file, frames, err)) continue;
		ShotAnalysis shot = ShotGeometry::Analyze(frames);
		RepSegments segs = Segmenter::Split(frames, item.eventTick);
		RepGrade g = Grader::GradeRep(frames, segs, shot, item.goal, item.floorBeforeGoal, ref);
		GradeRecord rec = Make(g, shot, segs, item.shotKey, ref.bestFile, false);

		std::lock_guard<std::mutex> lock(mtx);
		grades[item.file] = rec;
		dirty = true;
	}
	Save(root);
}

bool GradeBook::TrySet(const std::string& file, const GradeRecord& rec)
{
	std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
	if (!lock.owns_lock()) return false;
	grades[file] = rec;
	dirty = true;
	return true;
}

void GradeBook::Copy(std::unordered_map<std::string, int>& out)
{
	std::lock_guard<std::mutex> lock(mtx);
	out.clear();
	for (const auto& [file, e] : grades) out[file] = e.overall;
}

bool GradeBook::TryCopy(std::unordered_map<std::string, int>& out)
{
	std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
	if (!lock.owns_lock()) return false;
	out.clear();
	for (const auto& [file, e] : grades) out[file] = e.overall;
	return true;
}
