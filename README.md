# RL Coach

A game review for Rocket League training, like a chess.com review. RL Coach records your reps in training packs
and free play, splits each one into stages (approach, takeoff, flight, contact, result, recovery), grades every
stage, points out the one thing to fix first, replays the rep with lines showing where you should have hit the ball,
and lets you practice any moment of it on the spot.

Works offline only (free play and custom training), through BakkesMod. Module 1 covers aerial shots.

## Install

1. Install [BakkesMod](https://bakkesmod.com) and make sure it injects (launch Rocket League the way BakkesMod's
   setup tells you to).
2. Install RL Coach (from its BakkesPlugins page, or copy `RLCoach.dll` into
   `%APPDATA%\bakkesmod\bakkesmod\plugins` and run `plugin load rlcoach` in the F6 console).
3. Press F2, open the Plugins tab, and find RL Coach.

### One-time setup: arena files (for ball prediction)

RL Coach predicts where the ball goes using RocketSim, which needs the arena's collision shapes. They can't ship
with the plugin, so you make them once:

1. Download `RLArenaCollisionDumper.exe` from [its GitHub page](https://github.com/ZealanL/RLArenaCollisionDumper).
2. Put it in `%APPDATA%\bakkesmod\bakkesmod\data\RLCoach`.
3. Start Rocket League, go into free play on a standard arena (DFH Stadium works), then run the exe.
   It creates a `collision_meshes` folder next to itself.
4. In RL Coach's settings, open "Ball prediction" and click "Load arena files".

Everything else works without this step; you just won't see predicted ball paths.

## Using it

- **Record:** tick "Record reps" (or bind `rlcoach_toggle_recording` to a button). Recording starts off every session.
  A rep starts when you move after a reset and ends on a goal, the ball hitting the floor, or a timeout.
- **Rate reps** with 1-5 (number row, or numpad if you switch it): 1 Missed, 2 Close, 3 Bad shot but went in,
  4 Good shot went in, 5 Great shot went in. Press during a rep to rate that rep.
- **Review:** open the rep viewer (bind `rlcoach_viewer`). Double-click a rep to watch it. The timeline shows each stage
  and its grade; click to jump. Yellow ring = where you needed to hit the ball, green ring = where you did.
- **Practice:** in the viewer, click Practice next to any stage, or "Practice from here". The game freezes at that
  moment until you move, then you play it out. Adjust game speed and lead-in from the viewer. Bind
  `rlcoach_practice_retry` and `rlcoach_practice_exit` for controller use.
- **Controller overlay:** pick PS4 or Xbox under "Controller overlay" and set your button layout once.

## Your data

Everything lives in `%APPDATA%\bakkesmod\bakkesmod\data\RLCoach`:

| What | Where | Kept |
| --- | --- | --- |
| Rep summaries | `reps_index.csv` | forever |
| Your ratings | `ratings.csv` | forever |
| Auto grades | `auto_grades.csv` | forever |
| Full recordings | `sessions\*.rlc` (one compressed file per session) | 2 days, or 180 days if you rated it, or forever if starred; 1 GB cap |
| Starred reps | `kept.csv` | forever |

The keep times and cap are adjustable under "Storage and sharing". Updating the plugin never touches this folder.

**Sharing reps:** star the reps you want to share, click "Export starred reps", and send the file from the
`exports` folder. To watch a friend's reps, rename their file to their name (e.g. `jake.rlc`), drop it in the
`imports` folder, and click "Import shared reps". Imported reps are starred and labelled, and they never count as
your benchmarks.

## Commands

| Command | Does |
| --- | --- |
| `rlcoach_toggle_recording` | Recording on/off |
| `rlcoach_rate 1`-`5` | Rate the last rep |
| `rlcoach_viewer` | Open/close the rep viewer |
| `rlcoach_practice_retry` / `rlcoach_practice_exit` | Practice controls |
| `rlcoach_hud_toggle_compact` | Full or compact panel |

## For the developer: building and releasing

- Build: open `RLCoach.slnx` in Visual Studio, Release x64. The BakkesMod SDK comes from
  `%APPDATA%\bakkesmod\bakkesmod\bakkesmodsdk` (see `BakkesMod.props`). Each build bumps `VERSION_BUILD` in `version.h`.
- Versioning: bump `VERSION_MINOR` for a new feature or module (ground shots = 1.1.0), `VERSION_PATCH` for fixes.
  When grading rules change, also bump `Grader::kVersion` so saved auto grades are redone.
- Releasing on BakkesPlugins: they want the source code (not just the DLL) so they can review and build it. Zip the
  repo without `build/`, `plugins/`, `.vs/` and `_to_delete/`, and upload it as a new version of the plugin.
- Data format: recordings are CSV inside `RLC1` chunks; old formats are upgraded automatically on load. Keep it that
  way: a new version must always read older data.
- `tools/pad_prototype.py` renders the controller overlay shapes for quick visual changes; it writes `PadShapes.h`.

## Credits and licenses

- [RocketSim](https://github.com/ZealanL/RocketSim) (MIT) with Bullet Physics (zlib license), in `third_party/RocketSim`.
- [miniz](https://github.com/richgel999/miniz) (MIT), in `third_party/miniz`.
- Built on the BakkesMod SDK and the BakkesMod plugin template.
