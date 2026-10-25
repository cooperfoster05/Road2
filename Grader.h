#pragma once
// Turns a rep's measurements into grades: one per segment plus an overall 1-5 call that
// mirrors how you rate your own reps. Thresholds were tuned on your rated reps (see design doc).
// Plain C++, no BakkesMod code.

#include "RepData.h"
#include "Segmenter.h"
#include "ShotBook.h"
#include "ShotGeometry.h"
#include <string>
#include <vector>

enum class Grade { None, Calculated, Clean, Solid, Sloppy, Costly, OpenNet, Throw };

struct SegmentGrade
{
	int   segIndex = -1;          // index into RepSegments::segs
	Grade grade = Grade::None;
	std::string why;
};

struct RepGrade
{
	int   overall = 0;            // 1-5, same scale as your ratings
	std::string overallWhy;
	std::vector<SegmentGrade> segs;
	int   biggestFix = -1;        // index into segs: the earliest segment graded Sloppy or worse
};

namespace Grader
{
	// Bump whenever the rules or cutoffs change, so saved auto grades get redone
	inline constexpr int kVersion = 17;

	RepGrade GradeRep(const std::vector<Frame>& frames, const RepSegments& segs, const ShotAnalysis& shot,
		bool wentIn, bool bouncedIn, const ShotReference& ref);

	// Double taps (Coop, Oct 24): how quick the play was, 0 to about 1.15. Mostly the whole play (leaving the ground
	// to the shot), then how hard the shot was, then how quick the read off the wall; a tighter angle is a bonus.
	float DoubleTapPace(const ShotAnalysis& shot);

	const char* Name(Grade g);
	const char* OverallName(int overall);
}
