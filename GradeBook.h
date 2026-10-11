#pragma once
// Auto grades (1-5) for every saved rep, for the rep list. Saved in auto_grades.csv so they survive
// after a rep's recording is cleaned up, and re-graded when the benchmark it was graded against
// (your fastest goal on that shot) changes, as long as the recording still exists.
// Plain C++, no BakkesMod code.

#include "Grader.h"
#include "ShotBook.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

// What's kept per rep. The extra numbers are for stats and re-tuning once the recording is gone.
struct GradeRecord
{
	int   overall = 0;          // 0 = not graded (bailed rep)
	std::string shotKey;
	std::string refFile;        // the fastest-goal rep it was compared to
	float aimErr = -1;          // degrees
	float missDist = -1;        // units outside the goal, 0 = on target
	float shotSpeed = -1;
	float crossZ = -1;          // height crossing the goal line
	float lateBy = 0;           // seconds after the earliest intercept
	std::string segGrades;      // e.g. "Approach=Clean;Takeoff=Sloppy;..."
	int   version = Grader::kVersion;   // grader rules it was made with
	int   shotType = 0;         // ShotType: 0 aerial, 1 ground, 2 double tap, 3 dribble, 4 air dribble
};

class GradeBook
{
public:
	static GradeRecord Make(const RepGrade& g, const ShotAnalysis& shot, const RepSegments& segs,
		const std::string& shotKey, const std::string& refFile, bool bailed);

	// Grades any rep that's new or whose benchmark changed, then saves. Slow the first time; run it on a worker thread.
	void Refresh(const std::filesystem::path& rlcoachFolder, ShotBook& book);

	void Load(const std::filesystem::path& rlcoachFolder);   // once; later calls do nothing
	void Save(const std::filesystem::path& rlcoachFolder);   // only writes if something changed

	// Never waits (for the game and render threads); returns false if a refresh holds the lock.
	bool TrySet(const std::string& file, const GradeRecord& rec);
	// Makes the next Refresh redo every rep of this shot (e.g. after it's marked as a double-tap drill)
	void Invalidate(const std::string& shotKey);
	struct Brief { int overall = 0; int shotType = 0; };
	bool TryCopy(std::unordered_map<std::string, Brief>& out);
	void Copy(std::unordered_map<std::string, int>& out);   // waits briefly if a refresh is storing a grade

private:
	std::mutex mtx;            // guards the map
	std::mutex refreshMtx;     // one refresh at a time
	std::mutex saveMtx;
	std::unordered_map<std::string, GradeRecord> grades;
	bool loaded = false;
	bool dirty = false;
};
