# Builds the Road2 tester / release zip (Nov 4). Only a fixed list of files goes in, so your own reps, ratings,
# replays and ballchasing key can never end up in it. Build Release x64 in Visual Studio first, then from the repo
# folder in PowerShell:
#     powershell -ExecutionPolicy Bypass -File tools\package.ps1
# Writes dist\Road2_v<version>.zip (and leaves the unzipped copy in dist\Road2_v<version>\ to look at).
param([string]$Out = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$data = Join-Path $env:APPDATA "bakkesmod\bakkesmod\data\Road2"
if (-not (Test-Path $data)) { $data = Join-Path $env:APPDATA "bakkesmod\bakkesmod\data\RLCoach" }   # before the plugin's first run as Road2
if ($Out -eq "") { $Out = Join-Path $repo "dist" }

# The version, from version.h
$vh = Get-Content (Join-Path $repo "version.h") -Raw
$v = @("MAJOR", "MINOR", "PATCH", "BUILD") | ForEach-Object { if ($vh -match "#define VERSION_$_ (\d+)") { $matches[1] } else { "0" } }
$version = "$($v[0]).$($v[1]).$($v[2]).$($v[3])"
# The name players see: v1.0 (the build number stays inside the DLL)
$short = if ([int]$v[2] -gt 0) { "$($v[0]).$($v[1]).$($v[2])" } else { "$($v[0]).$($v[1])" }
$name = "Road2_v$short"
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
# (Nov 10) The zip mirrors the BakkesMod folder (plugins\Road2.dll, data\Road2\...), so one extract into
# %APPDATA%\bakkesmod\bakkesmod puts everything where it belongs. The first testers extracted the old flat zip into
# data and the plugin never showed up.
$dataOut = Join-Path $stage "data\Road2"
$pluginOut = Join-Path $stage "plugins"

function Copy-One($from, $toDir) {
    if (-not (Test-Path $from)) { throw "Missing: $from (run this with your own data folder in place, not a test install)" }
    New-Item -ItemType Directory -Force -Path $toDir | Out-Null
    Copy-Item $from $toDir
}

# 1. The plugin
$dll = Join-Path $repo "plugins\Road2.dll"
Copy-One $dll $pluginOut
$age = (Get-Date) - (Get-Item $dll).LastWriteTime
if ($age.TotalHours -gt 12) { Write-Host "Note: Road2.dll was built $([int]$age.TotalHours) hours ago. Rebuild first if you changed anything." }

# 2. The data folder: fonts, the lesson models (from the repo), the Combine's situations
Get-ChildItem (Join-Path $repo "fonts") -File | Where-Object { $_.Extension -in ".ttf", ".txt" } | ForEach-Object { Copy-One $_.FullName (Join-Path $dataOut "fonts") }
Get-ChildItem (Join-Path $repo "lessons") -Filter "*_model.csv" -File | ForEach-Object { Copy-One $_.FullName (Join-Path $dataOut "lessons") }
foreach ($f in "challenges.csv", "save_situations.csv", "pass_situations.csv", "takes.csv") { Copy-One (Join-Path $data "combine\$f") (Join-Path $dataOut "combine") }
# Nexto (CC BY-NC-SA 4.0: its weights and license go together)
foreach ($f in "nexto.bin", "LICENSE.txt") { Copy-One (Join-Path $repo "nexto\$f") (Join-Path $dataOut "nexto") }
# The Road2 Bot's look (copy data\Road2\bot_loadout.txt into the repo once you like it)
$look = Join-Path $repo "bot_loadout.txt"
if (Test-Path $look) { Copy-One $look $dataOut } else { Write-Host "Note: no bot_loadout.txt in the repo, so testers' bot gets the plain Octane." }

# 3. The papers
foreach ($f in "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md") { Copy-One (Join-Path $repo $f) $dataOut }
@"
Road2 v${short}: install
========================
1. Install BakkesMod (bakkesmod.com) and check that F2 opens its menu in Rocket League. Then close Rocket League.
2. Paste   %APPDATA%\bakkesmod\bakkesmod   into File Explorer's address bar and press Enter.
   You should see folders called "plugins" and "data".
3. Extract EVERYTHING in this zip into that folder (drag the zip's "plugins" and "data" folders in, and say yes
   if Windows asks to merge). That's the whole install.
   Check: ...\bakkesmod\bakkesmod\plugins\Road2.dll and ...\bakkesmod\bakkesmod\data\Road2\lessons both exist.
4. Start Rocket League, press F6 and type:   plugin load road2
   To load it every time: F2, Plugins, Plugin Manager, tick Road2.
5. Bind road2_viewer to a key (F2, Bindings) and press it. The welcome card walks you through the rest.
   (Road2 reads the arena's shape from your own Rocket League files by itself: nothing else to download.)

Feedback: what confused you, anything that graded strangely (star the rep, Export starred reps, send the file from
the exports folder), screenshots, and if it crashed: %APPDATA%\bakkesmod\bakkesmod\bakkesmod.log
More in data\Road2\README.md.
"@ | Out-File (Join-Path $stage "Road2_INSTALL.txt") -Encoding utf8

# 4. Safety check: nothing personal in it
$bad = Get-ChildItem $stage -Recurse -File | Where-Object {
    $_.Name -match "ballchasing|key|ratings|reps_index|auto_grades|kept|progress|reviewed|bests|_cache|\.rlc$|\.replay$" -and $_.Name -notmatch "_model\.csv$"
}
if ($bad) { $bad | ForEach-Object { Write-Host "Not allowed in the zip: $($_.FullName)" }; throw "Stopped: personal files found" }

# 5. Zip it
$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
$files = (Get-ChildItem $stage -Recurse -File).Count
Write-Host ("Built {0}: {1} files, {2:N1} MB" -f $zip, $files, ((Get-Item $zip).Length / 1MB))
