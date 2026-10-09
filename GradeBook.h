#pragma once
// Auto grades (1-5) for every saved rep, for the rep list. Kept in memory and re-graded when the
// benchmark it was graded against (your fastest goal on that shot) changes.
// Plain C++, no BakkesMod code.

#include "ShotBook.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

class GradeBook
{
public:
	// Grades any rep that's new or whose benchmark changed. Slow the first time; run it on a worker thread.
	void Refresh(const std::filesystem::path& rlcoachFolder, ShotBook& book);

	// Never waits (for the game and render threads); returns false if a refresh holds the lock.
	bool TrySet(const std::string& file, int overall, const std::string& shotKey, const std::string& refFile);
	bool TryCopy(std::unordered_map<std::string, int>& out);

private:
	struct Entry { int overall = 0; std::string shotKey; std::string refFile; };
	std::mutex mtx;
	std::unordered_map<std::string, Entry> grades;
};
