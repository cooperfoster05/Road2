#pragma once
// The mechanic rulebook: what counts as doing each mechanic, the steps it's made of, and the words the cues use.
// One place, read by the engine, the what-if and (later) Learn mode, so every mechanic plays by the same rules:
// an engine line or a what-if only counts if it does the mechanic you attempted, the way you attempted it.
// Plain C++, no BakkesMod code.
#include "RepData.h"
#include "Segmenter.h"
#include "ShotGeometry.h"
#include <string>
#include <vector>

// One step of a mechanic: which segment it is, what passing it means, and how cues name it
struct MechanicStep
{
	SegKind seg;
	const char* name;        // "Reset", "Carry"... (what cues and lessons call it)
	const char* pass;        // what passing the step means, in plain words
};

struct MechanicSpec
{
	ShotType type;
	const char* name;                 // "Flip reset"
	const char* lock;                 // what a line must do to count as this mechanic, in plain words
	std::vector<MechanicStep> steps;  // in order
};

namespace Mechanics
{
	const MechanicSpec& Spec(ShotType t);

	// The lock. Does this line do the mechanic you attempted, the way you attempted it?
	// yourFrames / lineFrames: whole reps (your real one; the line = your rep up to its start, then the sim).
	// lineFrames may be empty (a check that needs them is then skipped). why gets the reason when it doesn't.
	bool SameMechanic(const ShotAnalysis& yours, const std::vector<Frame>& yourFrames,
		const ShotAnalysis& line, const std::vector<Frame>& lineFrames, int start, std::string* why = nullptr);

	// How many of the mechanic's steps this rep passed, in order (stops at the first one it didn't).
	// The step search uses it to drop branches early; robustness uses it to say which step breaks.
	int StepsPassed(const MechanicSpec& spec, const ShotAnalysis& a, bool wentIn);

	// Fastest shot a line may hit and still count (a slow tip or dink doesn't), by mechanic
	float MinShotSpeed(ShotType t);
}
