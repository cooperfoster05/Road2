# tools/match: rank baselines and the scoring-chance model

Builds `MatchModels.inc` and `MatchBaseStats.inc` (repo root), which `Match.cpp` compiles in. Re-run it when a
measurement in `Match.cpp` changes or a new one is added, so the baselines measure the same thing the plugin does.

What it makes:
- **Rank baselines**: for 1v1, 2v2 and 3v3 and each rank from gold to supersonic legend, the 10th, 25th, 50th, 75th
  and 90th percentile of 29 numbers (giveaways, double commits, boost at top speed, ...). Counts are per five minutes
  of live play.
- **Level estimate**: a straight-line fit from the numbers that climb with rank to a rank (0 gold .. 5 SSL). One game
  is a rough read: for a whole lobby, within one rank about 4 times in 5 (`level.py` checks it).
- **Scoring chance**: a small neural net (21 inputs, 32 and 16 hidden) giving the chance a team scores in the next
  10 seconds from the ball, the nearest car on each side, and who is covering the goal. The inputs come from
  `Match::ChanceFeatures`, so training and the plugin can't drift apart.

## Steps

1. **Convert replays to scans** (needs Python with `subtr_actor` and numpy; it's in `data\Road2\tools_env`):
   `python convert.py subset.csv <ranked_replays folder> scans [seconds]`
   `subset.csv` lines are `id,rank,playlist`; the replay is `<folder>\<rank>\<id>.replay`. Safe to stop and run
   again; it skips what's done. Writes gzip'd scans in the same format as an in-game scan.
   Touches: replay files only report a touch when the touching team changes, so the rest are found from jumps in
   the ball's speed with a car close by. Checked against the game's own touch reports on an in-game scan: 97% of
   what it finds is real, and it missed none.
2. **Run the match review over them**: build `bulk` with
   `g++ -std=c++20 -O2 -I../.. bulk.cpp ../../Match.cpp -o bulk`, then `python run.py` (scans in `scans\`).
3. **Fit the scoring-chance model**: `python train.py` (pandas, scikit-learn).
4. **Write the tables**: `python gen.py`.
5. The cut-offs for the quick-chat labels (`kPlayCalculated` and the rest in `Match.cpp`) are percentiles of every
   touch's swing, printed by step 2's `W` rows: rebuild `bulk` after step 4, run step 2 again, and take the 99.5th,
   96.5th, 3.5th and 0.5th percentiles.

## The Nov 13 run

630 ranked replays from the ones `get_ranked_replays.ps1` downloaded: per rank, 40 doubles, 40 standard and 25 duels.
44 were left out (under two minutes of play, or uneven teams). The model's held-back score: AUC 0.73; its
percentages hold up (when it says 26%, 26% scored).
