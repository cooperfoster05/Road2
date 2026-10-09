#include "pch.h"
#include "GradeBook.h"
#include "Grader.h"
#include "RepLibrary.h"

#include <vector>

void GradeBook::Refresh(const std::filesystem::path& root, ShotBook& book)
{
	for (const RepListItem& item : RepLibrary::ListReps(root))
	{
		ShotReference ref = book.Get(item.shotKey);
		{
			std::lock_guard<std::mutex> lock(mtx);
			auto it = grades.find(item.file);
			if (it != grades.end() && it->second.refFile == ref.bestFile) continue;
		}

		std::vector<Frame> frames;
		std::string err;
		if (!RepLibrary::LoadRep(root / "reps" / item.file, frames, err)) continue;
		ShotAnalysis shot = ShotGeometry::Analyze(frames);
		RepSegments segs = Segmenter::Split(frames, item.eventTick);
		RepGrade g = Grader::GradeRep(frames, segs, shot, item.goal, item.floorBeforeGoal, ref);
		int overall = item.endReason == "reset" ? 0 : g.overall;   // bailed reps aren't graded

		std::lock_guard<std::mutex> lock(mtx);
		grades[item.file] = Entry{ overall, item.shotKey, ref.bestFile };
	}
}

bool GradeBook::TrySet(const std::string& file, int overall, const std::string& shotKey, const std::string& refFile)
{
	std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
	if (!lock.owns_lock()) return false;
	grades[file] = Entry{ overall, shotKey, refFile };
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
