# Downloads ranked replays from ballchasing.com, spread evenly across ranks, into RLCoach's data folder
# (ranked_replays\<rank>\). Used to calibrate the Combine and match analysis by rank. Your API key stays on your PC:
# it's read from ballchasing_key.txt in the RLCoach data folder and never printed. Run from PowerShell:
#     powershell -ExecutionPolicy Bypass -File tools\get_ranked_replays.ps1
#     powershell -ExecutionPolicy Bypass -File tools\get_ranked_replays.ps1 -PerRank 60     (a shorter run)
# It takes turns between ranks and playlists, so stopping it early still leaves an even spread. Free accounts can
# download about 200 replays an hour, so it paces itself to stay under that and waits it out if ballchasing says
# slow down. Safe to stop and run again: it skips replays it already has.
param(
    [int]$PerRank = 120,                                   # per rank, per playlist
    [string[]]$Playlists = @("ranked-doubles", "ranked-standard"),
    [string[]]$Ranks = @("gold", "platinum", "diamond", "champion", "grand-champion", "supersonic-legend"),
    [int]$PaceSeconds = 17
)

$data = Join-Path $env:APPDATA "bakkesmod\bakkesmod\data\RLCoach"
$keyFile = Join-Path $data "ballchasing_key.txt"
if (-not (Test-Path $keyFile)) { Write-Host "Put your ballchasing API key in $keyFile first."; exit 1 }
$key = (Get-Content $keyFile -Raw).Trim()
$headers = @{ Authorization = $key }
try { $me = Invoke-RestMethod -Uri "https://ballchasing.com/api/" -Headers $headers; Write-Host "Key works (account: $($me.name))." }
catch { Write-Host "ballchasing didn't accept the key: $($_.Exception.Message)"; exit 1 }

$root = Join-Path $data "ranked_replays"
New-Item -ItemType Directory -Force -Path $root | Out-Null
$index = Join-Path $root "index.csv"
if (-not (Test-Path $index)) { "id,rank,playlist,date,blue,orange" | Out-File $index -Encoding utf8 }

function Get-Page($url) {
    for ($try = 0; $try -lt 6; $try++) {
        try { return Invoke-RestMethod -Uri $url -Headers $headers }
        catch {
            $code = $_.Exception.Response.StatusCode.value__
            if ($code -eq 429) { Write-Host "ballchasing says slow down; waiting a minute..."; Start-Sleep -Seconds 60 }
            else { Write-Host "List failed ($code): $($_.Exception.Message)"; Start-Sleep -Seconds 10 }
        }
    }
    return $null
}

# One queue per rank and playlist: the list of replays to fetch (refilled a page at a time)
$queues = @()
foreach ($rank in $Ranks) {
    foreach ($pl in $Playlists) {
        $dir = Join-Path $root $rank
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $url = "https://ballchasing.com/api/replays?playlist=$pl&min-rank=$rank-1&max-rank=$rank-3&sort-by=replay-date&sort-dir=desc&count=200"
        if ($rank -eq "supersonic-legend") { $url = "https://ballchasing.com/api/replays?playlist=$pl&min-rank=supersonic-legend&max-rank=supersonic-legend&sort-by=replay-date&sort-dir=desc&count=200" }
        $queues += [pscustomobject]@{ Rank = $rank; Playlist = $pl; Dir = $dir; Next = $url; Items = [System.Collections.ArrayList]::new(); Got = 0 }
    }
}

$total = $PerRank * $queues.Count
$done = 0
Write-Host "Fetching up to $PerRank replays for each of $($queues.Count) rank/playlist pairs ($total in all, about $([Math]::Ceiling($total * $PaceSeconds / 3600)) hours)."
while ($true) {
    $progress = $false
    foreach ($q in $queues) {
        if ($q.Got -ge $PerRank) { continue }
        if ($q.Items.Count -eq 0 -and $q.Next) {
            $page = Get-Page $q.Next
            if ($page) { foreach ($r in $page.list) { [void]$q.Items.Add($r) }; $q.Next = $page.next } else { $q.Next = $null }
        }
        if ($q.Items.Count -eq 0) { continue }
        $r = $q.Items[0]; $q.Items.RemoveAt(0)
        $file = Join-Path $q.Dir ($r.id + ".replay")
        if (Test-Path $file) { $q.Got++; $done++; $progress = $true; continue }
        for ($try = 0; $try -lt 4; $try++) {
            try {
                Invoke-WebRequest -Uri "https://ballchasing.com/api/replays/$($r.id)/file" -Headers $headers -OutFile $file
                $blue = ($r.blue.players | ForEach-Object { $_.name }) -join ' '
                $orange = ($r.orange.players | ForEach-Object { $_.name }) -join ' '
                "$($r.id),$($q.Rank),$($q.Playlist),$($r.date),""$blue"",""$orange""" | Out-File $index -Append -Encoding utf8
                $q.Got++; $done++
                Write-Host "$done/$total  $($q.Rank) $($q.Playlist)"
                break
            }
            catch {
                $code = $_.Exception.Response.StatusCode.value__
                if (Test-Path $file) { Remove-Item $file -ErrorAction SilentlyContinue }
                if ($code -eq 429) { Write-Host "ballchasing says slow down; waiting a minute..."; Start-Sleep -Seconds 60 }
                else { Write-Host "Skipped $($r.id) ($code)"; break }
            }
        }
        $progress = $true
        Start-Sleep -Seconds $PaceSeconds
    }
    if (-not $progress) { break }
}
Write-Host "Done: $done replays in $root"
foreach ($q in $queues) { Write-Host ("  {0,-18} {1,-16} {2}" -f $q.Rank, $q.Playlist, $q.Got) }
