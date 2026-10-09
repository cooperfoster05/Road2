#pragma once
// Reads saved reps back from disk. Plain C++, no BakkesMod code.

#include "RepData.h"
#include <filesystem>
#include <string>
#include <vector>

struct RepListItem
{
	std::string file;
	std::string savedAt;
	std::string endReason;
	std::string shotKey;
	float duration = 0;
	int   touches = 0;
	bool  goal = false;
	int   rating = 0;   // 0 = not rated
	int   eventTick = -1; // tick where the shot ended (goal, floor, timeout); -1 if unknown
	bool  floorBeforeGoal = false;
};

namespace RepLibrary
{
	// Loads one rep file. Returns false and fills err if it can't be read.
	bool LoadRep(const std::filesystem::path& file, std::vector<Frame>& out, std::string& err);

	// Lists every rep in reps_index.csv with its latest rating, newest first.
	std::vector<RepListItem> ListReps(const std::filesystem::path& rootFolder);
}
