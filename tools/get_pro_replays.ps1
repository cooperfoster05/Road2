# Downloads pro replays from ballchasing.com into RLCoach's data folder (pro_replays\), for measuring how pros
# set up and finish mechanics. Your API key stays on your PC: put it in ballchasing_key.txt in the RLCoach data
# folder (one line, nothing else). Run from PowerShell:
#     powershell -ExecutionPolicy Bypass -File tools\get_pro_replays.ps1
#     powershell -ExecutionPolicy Bypass -File tools\get_pro_replays.ps1 -Count 100
#     powershell -ExecutionPolicy Bypass -File tools\get_pro_replays.ps1 -Title "air dribble" -AnyPlayer   (clips by title, anyone)
# Free accounts can download about 200 replays an hour, so this waits a little between files and skips any it
# already has (safe to run again).
param([int]$Count = 50, [string]$Playlist = "", [string]$Title = "", [switch]$AnyPlayer)

$data = Join-Path $env:APPDATA "bakkesmod\bakkesmod\data\RLCoach"
$keyFile = Join-Path $data "ballchasing_key.txt"
if (-not (Test-Path $keyFile)) { Write-Host "Put your ballchasing API key in $keyFile first."; exit 1 }
$key = (Get-Content $keyFile -Raw).Trim()
$headers = @{ Authorization = $key }

try { $me = Invoke-RestMethod -Uri "https://ballchasing.com/api/" -Headers $headers; Write-Host "Key works (account: $($me.name))." }
catch { Write-Host "ballchasing didn't accept the key: $($_.Exception.Message)"; exit 1 }

$out = Join-Path $data $(if ($Title -ne "") { "clip_replays" } else { "pro_replays" })
New-Item -ItemType Directory -Force -Path $out | Out-Null

$url = "https://ballchasing.com/api/replays?sort-by=replay-date&sort-dir=desc&count=" + [Math]::Min(200, $Count)
if (-not $AnyPlayer) { $url += "&pro=true" }
if ($Title -ne "") { $url += "&title=" + [uri]::EscapeDataString($Title) }
if ($Playlist -ne "") { $url += "&playlist=$Playlist" }
$got = 0
$index = Join-Path $out "index.csv"
if (-not (Test-Path $index)) { "id,date,playlist,title,blue,orange" | Out-File $index -Encoding utf8 }
while ($url -and $got -lt $Count) {
    $page = Invoke-RestMethod -Uri $url -Headers $headers
    foreach ($r in $page.list) {
        if ($got -ge $Count) { break }
        $file = Join-Path $out ($r.id + ".replay")
        if (Test-Path $file) { $got++; continue }
        try {
            Invoke-WebRequest -Uri "https://ballchasing.com/api/replays/$($r.id)/file" -Headers $headers -OutFile $file
            $blue = ($r.blue.players | ForEach-Object { $_.name }) -join ' '
            $orange = ($r.orange.players | ForEach-Object { $_.name }) -join ' '
            "$($r.id),$($r.date),$($r.playlist_id),""$($r.replay_title)"",""$blue"",""$orange""" | Out-File $index -Append -Encoding utf8
            $got++
            Write-Host "$got/$Count  $($r.replay_title)"
        }
        catch { Write-Host "Skipped $($r.id): $($_.Exception.Message)" }
        Start-Sleep -Milliseconds 1200
    }
    $url = $page.next
}
Write-Host "Done: $got replays in $out"
