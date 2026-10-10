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
	int   doubleTaps = 0;     // attempts that were double taps
	bool  doubleTapDrill = false;   // you marked this shot as a double-tap drill (a goal without one = a 1)
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
		bool goal, float touchSec, float jumpSec, bool doubleTap);

	// Every shot's fastest-goal rep (the timing benchmarks); cleanup never deletes these
	std::unordered_set<std::string> BestFiles();

	// Double-tap drill marks, per shot (kept in drills.csv). Never wait on a scan, so they're safe from the menus.
	void SetDoubleTapDrill(const std::filesystem::path& rlcoachFolder, const std::string& shotKey, bool on);
	bool IsDoubleTapDrill(const std::string& shotKey);

	// The same pack shot can land on neighbouring shot IDs (positions are rounded to 50); treat those as one shot
	static bool SameShot(const std::string& a, const std::string& b);

private:
	ShotReference Lookup(const std::string& shotKey);   // caller holds the lock
	struct Entry { std::string shot; bool goal = false; float touchSec = -1; float jumpSec = -1; bool doubleTap = false; };
	std::mutex mtx;
	std::unordered_map<std::string, Entry> entries;   // by rep file
	bool loadedCache = false;

	std::mutex drillMtx;
	std::unordered_map<std::string, bool> drills;   // shot key -> marked (newest line in drills.csv wins)
	bool loadedDrills = false;
	void LoadDrills(const std::filesystem::path& rlcoachFolder);   // caller holds drillMtx
	bool DrillLocked(const std::string& shotKey);                  // caller holds drillMtx
};
