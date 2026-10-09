#pragma once
// Where rep recordings live on disk. Plain C++, no BakkesMod code.
//
// Each play session's reps are packed into one compressed file: sessions/session_<date>_<time>.rlc
// Inside, every rep is a chunk: "RLC1", name length (2 bytes), name, raw size (4), packed size (4), packed CSV.
// Chunks are only ever added to the end, so a crash mid-save costs at most the rep being written.
// Old loose files in reps/ still load until they're moved into a bundle.
// Every function is safe to call from any thread.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace RepStore
{
	// One rep's CSV text, from a bundle or an old reps/ file
	bool Read(const std::filesystem::path& root, const std::string& name, std::string& csv, std::string& err);
	bool Has(const std::filesystem::path& root, const std::string& name);

	// Packs a rep's CSV into a bundle (made if it doesn't exist yet)
	bool Append(const std::filesystem::path& root, const std::filesystem::path& bundle,
		const std::string& name, const std::string& csv, std::string& err);

	struct RecInfo
	{
		std::string name;
		std::filesystem::path file;   // bundle, or the loose file for old reps
		uint64_t bytes = 0;           // space it takes on disk
		bool legacy = false;
	};
	std::vector<RecInfo> List(const std::filesystem::path& root);
	uint64_t TotalBytes(const std::filesystem::path& root);

	// Deletes these reps' recordings (bundles are rewritten without them; an emptied bundle is deleted).
	// Never touches `skipBundle` (the session being recorded). Returns how many were removed.
	int Remove(const std::filesystem::path& root, const std::unordered_set<std::string>& names,
		const std::filesystem::path& skipBundle);

	// Moves old loose reps/*.csv files into one bundle per day. Names in `drop` are deleted instead
	// (bailed reps keep only their summary). Returns how many files were handled.
	int MigrateLegacy(const std::filesystem::path& root, const std::unordered_set<std::string>& drop,
		const std::atomic<bool>* stop = nullptr);
}
