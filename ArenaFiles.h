#pragma once
// The arena's collision shape, read from the player's own Rocket League files (Nov 11). Plain C++, no BakkesMod code.
//
// The simulator (RocketSim) needs the standard arena as 16 small mesh files. They used to come from a separate
// program, RLArenaCollisionDumper, which searches the running game's memory and stopped working for some players.
// The same 16 meshes sit in the game's own stadium file on disk (the cooked physics data, in compressed chunks that
// need no key), so Road2 reads them from there: nothing to download, nothing of the game's is shipped with Road2,
// and it doesn't depend on the game's memory layout. Each mesh is checked against RocketSim's list of the real
// meshes' hashes, so what's written is exactly what the dumper would have made.

#include <filesystem>
#include <string>

namespace ArenaFiles
{
	// Writes the 16 files to <meshesFolder>/soccar. cookedFolder is the game's TAGame/CookedPCConsole.
	// True when all 16 were found and written; message says what happened either way (one line, for the log and the UI).
	bool FromGameFiles(const std::filesystem::path& cookedFolder, const std::filesystem::path& meshesFolder, std::string& message);
}
