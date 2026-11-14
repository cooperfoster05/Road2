// Runs the match review over one scan and prints what the baselines and the scoring-chance model are built from.
//   bulk scan.csv rank playlist samples.bin  -> stats rows on stdout (P rows per player, T rows per team)
// samples.bin gets appended: per sample, kChanceFeatures floats then a label float (1 = that team scored within 10 s)
// then a float for the rank number.
#include "Match.h"
#include <cstdio>
#include <cstring>
#include <cmath>
static int RankNo(const char* r) { const char* n[] = { "gold","platinum","diamond","champion","grand-champion","supersonic-legend" }; for (int i = 0; i < 6; i++) if (!strcmp(r, n[i])) return i; return -1; }
int main(int argc, char** argv)
{
	if (argc < 5) return 2;
	MatchData d; std::string err;
	if (!Match::Load(argv[1], d, err)) { fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
	const MatchAnalysis a = Match::Analyze(d);
	if (!a.ok) { fprintf(stderr, "analyze: %s\n", a.error.c_str()); return 1; }
	const int P = (int)a.players.size(), N = (int)d.frames.size();
	int count[2] = { 0, 0 };
	for (auto& p : a.players) if (p.team == 0 || p.team == 1) count[p.team]++;
	const float mins = a.seconds / 60.0f;
	if (mins < 2.0f || count[0] != count[1] || count[0] < 1 || count[0] > 3) { fprintf(stderr, "skip: %.1f min, %d v %d\n", mins, count[0], count[1]); return 1; }
	for (int i = 0; i < P; i++)
	{
		const MatchPlayerStats& s = a.stats[i];
		const int tm = a.players[i].team;
		if (tm != 0 && tm != 1) continue;
		printf("P,%s,%s,%s,%d,%d,%d,%.1f", d.replayName.c_str(), argv[2], argv[3], a.teamSize, tm, a.score[tm] > a.score[1 - tm] ? 1 : 0, a.seconds);
#define I(x) printf(",%d", s.x)
#define F(x) printf(",%.2f", s.x)
		I(touches); I(shots); I(saves); I(goals); I(assists); F(onBallSecs); F(avgBoost); F(noBoostSecs); F(boostCollected); F(boostWasted); I(bigPads); I(smallPads);
		F(supersonicPct); F(airPct); F(avgSpeed); F(defPct); F(midPct); F(offPct); F(behindBallPct); F(closestPct); F(lastBackPct);
		I(fifties); I(fiftiesWon); I(passes); I(giveaways); I(clears); I(demosTaken); I(giveRelief); I(giveMidfield); I(giveDangerous); I(giveBoomed); I(clearsMiddle);
		I(doubleCommits); I(beaten); I(beatenLast); I(leftNetOpen); I(leftForBoost); I(stolenPads); I(lowBoostFifties); I(defensiveActions); I(kickoffsTaken);
		I(goalsAgainstIn); F(aheadSecs); F(stoppedPct); F(recoverSecs); F(boostUsed); F(supersonicBoost); I(tookMateBoost); I(aerialsWon); I(aerialsLost);
		I(shadows); I(cutRotations); I(returns); I(returnsBallSide); F(bunchedPct);
		F(firstSharePct); F(lastSharePct); F(goalSideTheirsPct); F(supportDist); F(supportBack); F(lastDefDepth); F(lastAttDepth); F(flatPct); F(wideSupportPct); F(farFromPlayPct); F(posLevel);
		F(level); F(chanceAdded); F(impact); I(playsGood); I(playsBad);
		printf("\n");
	}
	for (int tm = 0; tm < 2; tm++)
		printf("T,%s,%s,%s,%d,%d,%d,%.1f,%.1f,%.1f,%d,%d,%d,%.1f,%d,%d,%d,%d,%.2f,%d\n", d.replayName.c_str(), argv[2], argv[3], a.teamSize, tm, a.score[tm] > a.score[1 - tm] ? 1 : 0, a.seconds,
			a.possessionPct[tm], a.territoryPct[tm], a.kickoffs, a.kickoffsWon[tm], a.doubleCommits[tm], a.openNetSecs[tm], a.openNets[tm], a.counterGoals[tm],
			a.shotsStayed[tm], a.shotsNoFollow[tm], a.bunchedPct[tm], a.score[tm]);
	for (int i = 0; i < P; i++)
	{
		const MatchPlayerStats& s = a.stats[i];
		if (a.players[i].team != 0 && a.players[i].team != 1) continue;
		printf("R,%s,%d,%d,%d,%.1f", argv[2], a.teamSize, (int)std::lround(a.lobbyLevel), s.overall, s.overallScore);
		for (int k = 0; k < Area_Count; k++) printf(",%d", s.area[k].grade);
		printf("\n");
	}
	printf("W,%s,%d", argv[2], a.teamSize); for (float w : a.touchSwings) printf(",%.3f", w); printf("\n");
	// samples for the scoring-chance model: twice a second of live play, from each team's side
	FILE* out = fopen(argv[4], "ab");
	if (!out) return 0;
	std::vector<int> goalIdx[2];
	for (int i = 1; i < N; i++) for (int tm = 0; tm < 2; tm++) if (d.frames[i].score[tm] > d.frames[i - 1].score[tm]) goalIdx[tm].push_back(i);
	const float rank = (float)RankNo(argv[2]);
	float lastT = -10.0f;
	for (int i = 0; i < N; i++)
	{
		if (!a.live[i] || !d.frames[i].hasBall || d.frames[i].time - lastT < 0.5f) continue;
		lastT = d.frames[i].time;
		for (int tm = 0; tm < 2; tm++)
		{
			float row[Match::kChanceFeatures + 2];
			if (!Match::ChanceFeatures(d, i, tm, a.possTeam[i], row)) continue;
			float label = 0.0f;
			for (int g : goalIdx[tm]) if (g > i && d.frames[g].time - d.frames[i].time <= 10.0f) label = 1.0f;
			row[Match::kChanceFeatures] = label;
			row[Match::kChanceFeatures + 1] = rank;
			fwrite(row, sizeof(float), Match::kChanceFeatures + 2, out);
		}
	}
	fclose(out);
	return 0;
}
