# Road2 bot trainer (ground-shot trial)

A bot that learns ground shots in the RocketSim physics sim, rewarded by the plugin's own grader. If its lines beat the engine's on the held-out benchmark, it can become another line-maker for "Engine line" (design doc, Phase 2b).

## How it learns

1. **Warm start.** It first copies your own inputs on your ground shots, so it starts out driving like you instead of mashing buttons.
2. **Practice.** Then it plays your ground-shot starts thousands of times a second: from when the ball starts moving up to half a second before your touch. Your shots are also used mirrored left to right, with the start jiggled a little each time. The reward is the grade the plugin gives the shot (a 5 is worth the most). Anything that isn't a real ground shot under the rulebook scores below a miss: banking off the ceiling or a wall, hovering low before the touch, a tip, or not touching the ball at all.
3. **Benchmark.** Every half hour it runs the 61 held-out ground shots it never trains on, under the engine's rules. A line counts only if it scores, beats your grade, is the same mechanic, and holds up from 6 nudged starts with the timing a tick or two off (4 of 6). The best checkpoint so far is kept as `best.bin`.

It decides every 2 ticks (60 times a second), with 9 steps each of steer, pitch and yaw, 5 of throttle and roll, and the jump, boost and handbrake buttons.

## Build (once)

Open `trainer\RLCoachTrainer.slnx` in Visual Studio, pick **Release | x64**, then **Build > Build Solution**. The exe lands in `build\trainer\RLCoachTrainer.exe`. The plugin's own solution is unchanged.

## Run

- **`StartTraining.bat`** starts training, or picks up where it left off. It runs for 8 hours by default (`StartTraining.bat 10` for 10 hours). It uses all cores but two at below-normal priority, so the PC stays usable, and it keeps Windows from sleeping while it runs. Close the window or press Ctrl+C to stop. It saves every 10 minutes.
- **`CheckBot.bat`** runs the best checkpoint on the held-out shots and puts it side by side with the engine.

Everything it writes goes to `%APPDATA%\bakkesmod\bakkesmod\data\RLCoach\bot\`:

| File | What it is |
| --- | --- |
| `status.txt` | One line: how far along it is |
| `eval_log.csv` | The held-out benchmark every half hour (the number that matters) |
| `train_log.csv` | Every training batch: touch, goal, 4–5 and beat-you rates |
| `latest.bin`, `best.bin`, `ckpt_*.bin` | Checkpoints |
| `train_config.txt` | Settings; edit and restart to change them |
| `enginebench.csv` | The engine on the same held-out reps (for the side-by-side) |

## Commands

```
RLCoachTrainer train       [--hours H] [--threads N]   train (resumes from latest.bin)
RLCoachTrainer evalbot     [--ckpt FILE] [--lines 32]  the bot on the held-out ground shots
RLCoachTrainer enginebench                             the engine on the same reps (slow: a few seconds per rep)
RLCoachTrainer check                                   replays your own inputs to check the sim is set up right
```

`--data DIR` points at a different data folder (the default is the RLCoach one).

## Held-out lists

`heldout_<mechanic>.txt` are the fixed benchmark reps for all six mechanics: 61 aerials, 61 ground shots and 30 of each other mechanic. They are the original lists from Oct 13, 2026 that every engine number in the design doc is measured on (restored Oct 23; a new draw that night overlapped only 6 of the 61 ground reps, so the bot would have trained on the benchmark). They live in the repo now so they can't get lost again. Nothing here trains on them.

## Files

| File | What it does |
| --- | --- |
| `Main.cpp` | The commands above |
| `Trainer.cpp` | Warm start, PPO training, checkpoints, the held-out benchmark |
| `Bot.cpp` | What the bot sees and what it can press |
| `Net.cpp` | The neural network, its backprop and the Adam optimizer (plain C++, AVX2 when available) |
| `LineSim.cpp` | Plays a line exactly like an engine line: same start, end rules, grading and lock |
| `Data.cpp` | Loads your reps, the held-out lists, mirroring |
| `../EngineSim.h` | The engine's own physics reset and tick, shared so bot lines and engine lines play out identically |
| `CMakeLists.txt` | Cloud/Linux build (not used on Windows) |
