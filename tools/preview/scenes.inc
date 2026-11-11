#include "src/Units.h"
#include "viewer_statics.inc"
static ImVec2 MousePos(const std::string&) { return ImVec2(-100, -100); }
static void SetupMore(RLCoach& p, const std::string& scene);
static bool Has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

// ViewerLoad's own work, without the game
static void LoadPure(RLCoach& p, const std::string& file)
{
	const RepListItem* item = nullptr;
	for (int i = 0; i < (int)p.repList.size(); i++) if (p.repList[i].file == file) { item = &p.repList[i]; p.selectedRep = i; }
	if (!item) { fprintf(stderr, "no rep %s\n", file.c_str()); return; }
	std::vector<Frame> loaded; std::string err;
	if (!RepLibrary::LoadRep(p.writer->Folder(), item->file, loaded, err)) { fprintf(stderr, "load: %s\n", err.c_str()); return; }
	p.viewerFrames = std::move(loaded);
	p.viewerRep = *item;
	p.viewerShot = ShotGeometry::Analyze(p.viewerFrames);
	p.viewerSegs = Segmenter::Split(p.viewerFrames, item->eventTick);
	p.uiWhatIfStart = WhatIf::StartTick(p.viewerFrames);
	ShotReference ref;
	p.shotBook.TryGet(item->shotKey, ref);
	p.viewerGrade = Grader::GradeRep(p.viewerFrames, p.viewerSegs, p.viewerShot, item->goal, item->floorBeforeGoal, ref);
	p.uiViewerFile = item->file;
	p.uiSegs = p.viewerSegs.segs;
	p.uiSegTotal = (int)p.viewerFrames.size();
	p.uiGrades.clear();
	for (int gi = 0; gi < (int)p.viewerGrade.segs.size(); gi++)
	{
		const SegmentGrade& gs = p.viewerGrade.segs[gi];
		p.uiGrades.push_back(RLCoach::UiGrade{ gs.segIndex, Grader::Name(gs.grade), gs.why, gi == p.viewerGrade.biggestFix });
	}
	p.uiOverallGrade = p.viewerGrade.overall;
	p.uiOverallName = p.viewerGrade.overall > 0 ? Grader::OverallName(p.viewerGrade.overall) : "";
	p.uiOverallWhy = p.viewerGrade.overallWhy;
	p.uiMechanic = p.viewerShot.valid ? ReviewMechanic((int)p.viewerShot.type) : "REP";
	p.uiStats = ReviewStats(p.viewerShot, item->goal, ref);
	p.uiNotes = p.coachBook.Notes(p.viewerFrames, p.viewerShot, p.viewerGrade.overall, p.habitBook);
	p.viewerActive = true;
	p.uiViewerActive = true;
	p.uiViewerPlaying = false;
	p.uiViewerTotal = (int)p.viewerFrames.size();
	p.uiViewerPlayhead = p.uiViewerTotal * 0.46f;
	p.uiReviewPending = false;
	// the two searches, as found
	WhatIfResult fix;
	fix.status = WhatIfStatus::Found; fix.startTick = (int)(p.uiViewerTotal * 0.4f); fix.realGrade = p.viewerGrade.overall; fix.grade = 4; fix.wentIn = true;
	fix.cue = "Jump 0.08 s sooner"; fix.gradeWhy = "Goal, upper half of the net, fast (2810 uu/s)"; fix.held = 5; fix.heldOf = 6;
	WhatIfResult best = fix;
	best.point = WhatIfPoint::Engine; best.grade = 5; best.startTick = (int)(p.uiViewerTotal * 0.25f);
	best.cue = "Engine (your line, sharpened, a cleaner 4: higher and harder): hit it a bit more on its right side and get a bit more under it";
	best.gradeWhy = "Goal, upper half of the net, fast (3131 uu/s)"; best.held = 4; best.heldOf = 6;
	best.steps = { {"Takeoff", 2, 4, "", "", "", true}, {"Flight", 4, 4}, {"Contact", 1, 4} };
	p.lineResult[0] = fix; p.lineResult[1] = best; p.lineFile[0] = p.lineFile[1] = item->file;
}

static void Setup(RLCoach& p, const std::string& scene)
{
	*p.viewerRange = 2;
	p.RefreshRepList();
	for (int i = 0; i < 600 && p.gradeThreadBusy; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	SetupMore(p, scene);
	if (Has(scene, "review") || Has(scene, "best") || Has(scene, "fix"))
		LoadPure(p, Has(scene, "good") ? "rep_2026-11-08_22-49-07_276.csv" : "rep_2026-11-08_22-26-00_013.csv");
}
static void SetupMore(RLCoach& p, const std::string& scene)
{
	if (Has(scene, "tree")) { p.uiLearnMode = true; p.LearnOpen(); }
	if (Has(scene, "lesson")) { p.uiLearnMode = true; p.LearnOpen(); p.LearnOpenLesson("front_flick"); }
	if (Has(scene, "set"))
	{
		p.uiCombineMode = true; p.CombineOpen();
		RLCoach::UiCombine& u = p.uiCombine;
		u.running = true; u.done = Has(scene, "done"); u.code = "SV5-FEF7797C"; u.levelName = "Legend"; u.event = "SAVES"; u.item = "Shot"; u.eventCode = "SV";
		u.level = 5; u.shots = 10; u.maxPoints = 30;
		const int pts[10] = { 0, 2, 2, 3, 0, 2, 3, 0, 2, 3 };
		const int n = u.done ? 10 : 4;
		u.shot = u.done ? 10 : 5; u.points = 0; u.best = 19;
		for (int i = 0; i < 10; i++)
		{
			u.pointsEach.push_back(i < n ? pts[i] : -1);
			u.outcomes.push_back(i < n ? (pts[i] == 0 ? "Conceded" : pts[i] == 3 ? "Saved and cleared" : "Saved") : "");
			if (i < n) u.points += pts[i];
		}
		if (u.done) u.medal = "BRONZE";
		p.uiDoneAt = -10.0;
		p.uiDoneCode = u.code;
	}
	if (Has(scene, "combine")) { p.uiCombineMode = true; p.CombineOpen(); p.uiCombineLevel = 5; }
}
static void Frame(RLCoach& p, const std::string& scene, int f)
{
	if (f == 2) { if (Has(scene, "best")) p.uiTab = 2; if (Has(scene, "fix")) p.uiTab = 1; }
	if (Has(scene, "settings"))
	{
		ImGui::SetNextWindowPos(ImVec2(40, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(1000, 1040), ImGuiCond_Always);
		ImGui::Begin("Bakkesmod settings");
		p.RenderSettings();
		ImGui::End();
		return;
	}
	p.Render();
}
