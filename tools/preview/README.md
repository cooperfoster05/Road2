# Preview tool

Draws Road2's real window code into PNGs without the game, so UI changes can be checked by eye.
It is a developer tool for Linux (g++ 13); it is not part of the plugin and Visual Studio ignores it.

How it works: the plugin's sources are compiled as they are and linked with ImGui and a small software
renderer (`harness.cpp`). BakkesMod itself isn't linked: any call into it does nothing and returns 0.
`scenes.inc` sets up each screen (rep list, a review, the Combine page, the skill tree, a lesson) from real
data in a copy of the data folder.

Layout it expects: `src/` (the repo), `sdk/sdkfix` (the BakkesMod SDK with lower-case paths), `sdk/shim`,
`data/` (a copy of data\Road2 with fonts, reps_index.csv, auto_grades.csv, one session, lessons, combine\bests.csv).

    ./build.sh src/*.cpp     # compile what changed
    ./link.sh                # build ./prev
    ./shot.sh combine        # -> shots/combine.png   (scenes: list, review, reviewgood, best, bestgood, fix, tree, lesson, combine)
