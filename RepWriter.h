#pragma once
// Saves reps, ratings and keep marks. Plain C++, no BakkesMod code.
// Saving happens on a background thread so the game never waits on the disk:
// SaveRep names the rep right away and queues the actual write.

#include "RepData.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class RepWriter
{
public:
	explicit RepWriter(std::filesystem::path rootFolder);
	~RepWriter();   // finishes anything still queued

	// Fills summary.fileName and savedAt now, then writes in the background: the recording goes into
	// this session's bundle (bailed "reset" reps keep only their summary) and a line goes in reps_index.csv.
	bool SaveRep(const std::vector<Frame>& frames, RepSummary& summary);

	// Appends a rating line to ratings.csv (the newest line for a file wins)
	bool SaveRating(const std::string& repFile, int rating);

	// Adds a summary line for a rep whose recording was already stored (used by import)
	void AddIndexRow(const RepSummary& summary);

	// Marks a rep as kept (never cleaned up) or not, in kept.csv (newest line wins)
	void SetKept(const std::string& repFile, bool kept);

	// Waits for queued writes and stops the background thread
	void Shutdown();

	std::filesystem::path Folder() const { return root; }
	std::filesystem::path SessionBundle() const { return bundle; }
	int FailedSaves() const { return failed; }
	bool Busy();

private:
	void Run();
	void Enqueue(std::function<void()> job);

	std::filesystem::path root;
	std::filesystem::path bundle;   // this session's file; created on the first save
	int counter = 0;
	std::atomic<int> failed{ 0 };

	std::mutex qmtx;
	std::condition_variable cv;
	std::deque<std::function<void()>> queue;
	bool stopping = false;
	bool working = false;
	std::thread worker;
};
