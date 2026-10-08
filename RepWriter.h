#pragma once
// Saves reps and ratings to disk as CSV. Plain C++, no BakkesMod code.

#include "RepData.h"
#include <filesystem>

class RepWriter
{
public:
	explicit RepWriter(std::filesystem::path rootFolder);

	// Writes one rep file plus a line in reps_index.csv. Fills summary.fileName and savedAt.
	// Returns false if the write failed.
	bool SaveRep(const std::vector<Frame>& frames, RepSummary& summary);

	// Appends a rating line to ratings.csv (the newest line for a file wins)
	bool SaveRating(const std::string& repFile, int rating);

	std::filesystem::path Folder() const { return root; }

private:
	std::filesystem::path root;
	int counter = 0;
};
