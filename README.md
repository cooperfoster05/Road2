# Road2

Game review for Rocket League mechanics, the way chess.com reviews a game. Road2 records your reps in free play and
custom training, splits each one into its parts (approach, takeoff, flight, contact, recovery...), grades every part,
finds the real mistake, and shows you two better versions: the **smallest fix** (the least you'd have to change) and
the **engine line** (the best line it can find in a physics simulation). Then it teaches you, one skill at a time.

Free and open source, and it always will be. It runs offline only (free play and custom training), through
BakkesMod.

> **Early testing build.** Things will break. If something looks wrong, see "Sending feedback" at the bottom.

## What's in it

- **Review.** Every rep gets a grade from 1 to 5 for each part and overall, with the one thing to fix first. The
  viewer replays the rep with your path, the ball's path and the ring where you needed to hit it, and lets you
  practice any moment of it on the spot ("Practice from here").
- **Smallest fix and engine line.** For aerials, power shots, dribbles and flicks, double taps, air dribbles, ceiling
  shots and flip resets, Road2 simulates changes to your rep and shows the smallest one that turns it into a goal, and
  the best line it can find. You can watch either one and practice it.
- **Learn.** A skill tree of lessons, from foundations (aerial, power shot, dribble, half flip, powerslide turn,
  recovery, air roll, tornado spin, off the wall, backboard read, front flick) up to flip resets and ceiling shots,
  plus four kickoffs. Each lesson is split into steps you
  watch, then try, at slower game speed if you want, until you clear them. Tier 1 and 2 skills you already have can be
  tested out of in one go.
- **The Combine.** Scored sets of 10 real situations from ranked and pro replays: saves, recoveries, wall and
  backboard reads, first touches, 50/50s, kickoffs and shadow defense, against a bot that plays the other player's
  real moves. 50s, kickoffs and shadow defense can also be played against Nexto (the RLGym bot) from the same real
  positions; kickoffs against Nexto are called during the 3-2-1 (speedflip, delay, fake or spanish). Score on a goalie
  starts you just before a real take (on the ground or off the wall), with Nexto defending. Each set has a code, so friends can play the same one.

## Install

1. Install [BakkesMod](https://bakkesmod.com) and make sure it injects: launch Rocket League and check that F2 opens
   BakkesMod's menu.
2. Close Rocket League. Paste `%APPDATA%\bakkesmod\bakkesmod` into File Explorer's address bar; you should see
   folders called `plugins` and `data`. Extract everything in the Road2 zip into that folder (its own `plugins` and
   `data` folders merge into yours). You end up with `...\plugins\Road2.dll` and `...\data\Road2\lessons`,
   `...\data\Road2\fonts` and so on.
3. In Rocket League press F6 and type `plugin load road2`. To load it every time, press F2, open the Plugins tab,
   find Road2 in the plugin manager and tick it. Road2's settings are under F2, Plugins, Road2.

### The arena files (automatic)

The smallest fix, the engine line, predicted ball paths and the Combine run on
[RocketSim](https://github.com/ZealanL/RocketSim), which needs the standard arena's collision shape. Road2 reads it
from your own Rocket League install the first time it loads (about a quarter of a second) and saves it as 16 small
files in `data\Road2\collision_meshes\soccar`. Nothing of the game's is shipped with Road2, and there is nothing to
download or run. The Simulator tile in the settings (F2, Plugins, Road2) should say Ready.

If it can't (a card in the settings says why, with a Try again button), the older way still works: put
[RLArenaCollisionDumper.exe](https://github.com/ZealanL/RLArenaCollisionDumper/releases/download/v1.0.0/RLArenaCollisionDumper.exe)
in `%APPDATA%\bakkesmod\bakkesmod\data\Road2`, run it while you're in free play on a standard arena, then click
Try again.

Recording, grading, reviewing and Learn work without this step.

## Using it

- **Open Road2:** bind `road2_viewer` to a key or button in BakkesMod's Bindings tab. That window has your reps, the
  review, Learn and the Combine.
- **Record:** switch on "Record reps" in the settings (or on the welcome card the first time you open Road2), or bind
  `road2_toggle_recording`. Recording starts off every session. A rep starts when you move after a reset and ends on a goal, the ball hitting the floor, or a timeout.
- **Rate reps (optional):** 1-5 on the number row (or numpad, if you switch it): 1 Missed, 2 Close, 3 Bad shot but
  went in, 4 Good shot went in, 5 Great shot went in. Your ratings teach the grader what you count as good. Turn
  "Rate with the number keys" off to free up those keys.
- **Review a rep:** double-click it in the list. The timeline shows each part and its grade; click a part to jump
  there. The tabs show your shot, the smallest fix and the engine line.
- **Practice:** click Practice on any part, or "Practice from here". The game freezes at that moment until you
  move, then you play it out. Bind `road2_practice_retry` and `road2_practice_exit` for a controller.
- **Controller overlay:** pick PS4 or Xbox under "Controller overlay" and set your button layout once.

Grades get sharper the more you play the same shot: timing is measured against your own fastest goal on it.

## Your data

Everything lives in `%APPDATA%\bakkesmod\bakkesmod\data\Road2`:

| What | Where | Kept |
| --- | --- | --- |
| Rep summaries | `reps_index.csv` | forever |
| Your ratings | `ratings.csv` | forever |
| Auto grades | `auto_grades.csv` | forever |
| Full recordings | `sessions\*.rlc` (one compressed file per session) | 2 days; 30 days if it auto-graded 4 or 5 or you opened its review; 180 days if you rated it; forever if starred; 1 GB cap |
| Starred reps | `kept.csv` | forever |
| Lesson progress | `lessons\progress.csv` | forever |

The keep times and the cap are under "Storage and sharing". Updating the plugin never touches your own files, but
an update's `Road2` folder replaces the lesson models and Combine situations, so unzip it over the old one.

**Sharing reps:** star the reps you want to share, click "Export starred reps", and send the file from the
`exports` folder. To watch a friend's reps, rename their file to their name (e.g. `jake.rlc`), drop it in the
`imports` folder, and click "Import shared reps". Imported reps are starred and labelled, and they never count as
your benchmarks.

## Sending feedback

What helps most:

- What confused you, what felt wrong, and any grade you disagree with (say what you thought it should be).
- For a rep that graded strangely: star it, click "Export starred reps", and send the file from `exports`.
- Screenshots of anything that looks off.
- If the game crashed or the plugin stopped working: `%APPDATA%\bakkesmod\bakkesmod\bakkesmod.log`.

## Commands

| Command | Does |
| --- | --- |
| `road2_viewer` | Open/close the Road2 window |
| `road2_toggle_recording` | Recording on/off |
| `road2_rate 1`-`5` | Rate the last rep (`road2_rate_recovery 1`-`3` rates the recovery) |
| `road2_practice_retry` / `road2_practice_exit` | Practice controls |
| `road2_hud_toggle_compact` | Full or compact panel |

## For the developer: building and releasing

- Build: open `RLCoach.slnx` in Visual Studio, Release x64 (it builds `plugins\Road2.dll`; the project and source
  files keep their old RLCoach names, players never see those). The BakkesMod SDK comes from
  `%APPDATA%\bakkesmod\bakkesmod\bakkesmodsdk` (see `BakkesMod.props`). Each build bumps `VERSION_BUILD` in `version.h`.
- Versioning: bump `VERSION_MINOR` for a new feature, `VERSION_PATCH` for fixes. When grading rules change, also bump
  `Grader::kVersion` so saved auto grades are redone.
- A tester zip mirrors the BakkesMod folder: `plugins\Road2.dll` and a `data\Road2` folder with only `fonts`, `lessons\*_model.csv` and the
  Combine's `combine\challenges.csv`, `save_situations.csv`, `pass_situations.csv` and `takes.csv`, Nexto's `nexto` folder
  and `bot_loadout.txt` (`tools\package.ps1` builds exactly that). Never zip the whole data folder:
  it has your reps, ratings, replays and API key.
- The Road2 Bot's look: turn on Developer tools, make a preset in your garage, then F6 `road2_bot_capture "<preset name>"`
  (or `road2_bot_capture me` for what you're wearing, paints included). It copies the
  preset's orange side (paints included) to `data\Road2\bot_loadout.txt`; copy that file into the repo so
  `package.ps1` ships it to testers.
- Nexto's weights: `nexto/nexto.bin`, made from the original `nexto-model.pt` by `tools/nexto/export.py`.
- Developer tools (accuracy test, timing, set-state test) are under Tools in the settings, behind "Developer tools".
  The developer console commands (`road2_test_setstate`, `road2_bot_*`) only exist while it's on.
  `road2_welcome_done 0` brings the first-run card back.
- Updating from a pre-v1 RLCoach build: on its first run Road2 renames `data\RLCoach` to `data\Road2`, carries the old
  `rlcoach_` settings and key binds over to `road2_`, and unloads and deletes an old `RLCoach.dll`.
- Releasing on BakkesPlugins: they want the source code (not just the DLL) so they can review and build it. Zip the
  repo without `build/`, `plugins/`, `.vs/` and `_to_delete/`, and upload it as a new version of the plugin.
- Data format: recordings are CSV inside `RLC1` chunks; old formats are upgraded automatically on load. Keep it that
  way: a new version must always read older data.
- Model reps: `tools/learn/move_model.cpp` and `tools/learn/orient_model.cpp` (movement and air control),
  `tools/combine/kickoff_model.cpp` (kickoffs), all in RocketSim. `tools/combine/*.py` pull the Combine's situations
  from replays (`tools/get_*_replays.ps1` download them from ballchasing.com with your own API key).
- `tools/pad_prototype.py` renders the controller overlay shapes for quick visual changes; it writes `PadShapes.h`.

## Credits and licenses

Road2 is MIT licensed (`LICENSE`). It's built on:

- [RocketSim](https://github.com/ZealanL/RocketSim) (MIT) with Bullet Physics (zlib license), in `third_party/RocketSim`.
- [miniz](https://github.com/richgel999/miniz) (MIT), in `third_party/miniz`.
- Fonts: Inter, JetBrains Mono and Barlow Condensed, all under the SIL Open Font License (`fonts/OFL*.txt`).
- [Nexto](https://github.com/Rolv-Arild/Necto), the RLGym community bot by Rolv, Soren and contributors, under
  CC BY-NC-SA 4.0 (`nexto/LICENSE.txt`). Road2 runs its network itself, so no RLBot or Python is needed.
- Replay situations from [ballchasing.com](https://ballchasing.com).
- The BakkesMod SDK and the BakkesMod plugin template.

Details in `THIRD_PARTY_NOTICES.md`.

Road2 is a free, unofficial fan project, made under Epic's
[Fan Content Policy](https://www.epicgames.com/site/fan-art-policy). Portions of the materials used are trademarks
and/or copyrighted works of Epic Games, Inc. All rights reserved by Epic. This material is not official and is not
endorsed by Epic.
