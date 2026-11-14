# Runs after every build: hands the new Road2.dll to BakkesMod (unload, copy, load).
# The copy fails if the game hasn't let go of the old plugin yet, so this tries again for a few seconds
# before giving up.
param([string]$Patch, [string]$Dll)
for ($i = 1; $i -le 8; $i++) {
    & $Patch $Dll
    if ($LASTEXITCODE -eq 0) { exit 0 }
    Write-Host "Road2: the old plugin file was still in use, trying again ($i of 8)..."
    Start-Sleep -Milliseconds 1500
}
Write-Host "Road2: gave up. The new plugin is built at $Dll but was not copied into BakkesMod."
exit 1
