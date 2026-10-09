#pragma once
// Remembers, for every shot (grouped by shot ID), your fastest goal: when you touched it and when you jumped.
// Grades compare a rep's timing to that. Cached in shot_cache.csv so only new reps get scanned.
// Plain C++, no BakkesMod code.

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct ShotReference
{
	bool  valid = false;
	std::string bestFile;
	float touchSec = 0;   // seconds from the ball starting to move to your shot touch
	float jumpSec = -1;   // seconds to the jump that led into that touch (-1 if none)
	int   attempts = 0;
	int   goals = 0;
};

class ShotBook
{
public:
	// Scans any reps not in the cache yet. Safe to call from any thread.
	void Update(const std::filesystem::path& rlcoachFolder);
	ShotReference Get(const std::string& shotKey);

	// Game-thread versions that never wait: they give up (return false) if a scan is running.
	bool TryGet(const std::string& shotKey, ShotReference& out);
	bool TryAdd(const std::filesystem::path& rlcoachFolder, const std::string& file, const std::string& shotKey,
		bool goal, float touchSec, float jumpSec);

	// Every shot's fastest-goal rep (the timing benchmarks); cleanup never deletes these
	std::unordered_set<std::string> BestFiles();

private:
	ShotReference Lookup(const std::string& shotKey);   // caller holds the lock
	struct Entry { std::string shot; bool goal = false; float touchSec = -1; float jumpSec = -1; };
	std::mutex mtx;
	std::unordered_map<std::string, Entry> entries;   // by rep file
	bool loadedCache = false;
};
