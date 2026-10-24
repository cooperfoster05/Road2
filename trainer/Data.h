#pragma once
// Coop's recorded reps for the trainer and the benchmarks: loading, the held-out lists, mirroring.
// Plain C++, no BakkesMod code.
#include "../RepData.h"
#include "../ShotBook.h"
#include "../ShotGeometry.h"
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

struct Rep
{
	std::string file;
	std::vector<Frame> fr;
	bool wentIn = false;
	bool floorBeforeGoal = false;
	int  eventTick = -1;
	ShotAnalysis real;      // your shot
	int  realGrade = 0;     // graded now, with today's grader
	ShotReference ref;
	int  sContact = 1;      // the engine's default start: half a second before your touch
	int  sMech = 1;         // the start of the mechanic (for a ground shot: the ball starts moving)
	bool mirrored = false;
};

namespace Data
{
	// The six held-out lists (fixed, in the repo so they survive): trainer/heldout_<mechanic>.txt
	std::filesystem::path HeldoutFile(const std::filesystem::path& repoTrainerDir, ShotType t);
	std::unordered_set<std::string> ReadList(const std::filesystem::path& file);

	// Every rep in reps_index.csv whose recording still exists and whose shot is this type.
	// which: 0 = all, 1 = held-out only, 2 = everything but the held-out.
	std::vector<Rep> Load(const std::filesystem::path& dataRoot, ShotBook& book, ShotType type,
		const std::unordered_set<std::string>& heldout, int which, int threads);

	// Left-right mirror of a rep (x -> -x). The physics is symmetric, so it's a new, equally real start.
	std::vector<Frame> Mirror(const std::vector<Frame>& fr);
	void Finish(Rep& r, ShotBook& book, const std::string& shotKey);   // analysis, grade, starts

	const char* TypeFile(ShotType t);   // "ground", "aerial", ...
}
