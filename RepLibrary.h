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
	int   recoveryRating = 0;   // your recovery rating: 1 poor, 2 eh, 3 good, 0 = not rated
	int   eventTick = -1; // tick where the shot ended (goal, floor, timeout); -1 if unknown
	bool  floorBeforeGoal = false;
	bool  kept = false;          // starred: never cleaned up
	bool  hasRecording = true;   // false once the recording is cleaned up (summary and grades stay)
	std::string mode;            // "training", "freeplay", or "import:<from>" for reps someone shared with you
	bool  imported = false;
	std::string importedFrom;    // who/what file it came from
};

namespace RepLibrary
{
	// Loads one rep's recording by name (from its session bundle, or an old loose file).
	// Returns false and fills err if it can't be read.
	bool LoadRep(const std::filesystem::path& rootFolder, const std::string& name, std::vector<Frame>& out, std::string& err);

	// Turns a rep's CSV text into frames
	bool ParseRep(const std::string& csv, std::vector<Frame>& out, std::string& err);

	// Lists every rep in reps_index.csv with its latest rating and keep mark, newest first.
	std::vector<RepListItem> ListReps(const std::filesystem::path& rootFolder);
}
