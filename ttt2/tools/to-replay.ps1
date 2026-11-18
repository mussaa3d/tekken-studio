# Test helper: starts the studio's copy of TTT2 in a window and has BlueSpark play the newest replay in its replays folder, through
# the studio's own "replay" command (the same call its Replays tab makes). Waits until the replay's fight is up.
param([string[]]$Extra = @(), [string]$Playtest = "C:\Users\ms3ody\Downloads\Compressed\ttt2-playtest")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$drive = Join-Path $here 'drive-ttt2.ps1'
$replay = Get-ChildItem (Join-Path $Playtest 'rex\userdata\replays') -Filter '*.ttt2replay' -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $replay) { "no replay yet: play a Versus match first"; return }
Get-Process ttt2-studio -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
& (Join-Path (Split-Path $here) 'run-studio.ps1') -Playtest $Playtest -Windowed -Extra $Extra | Out-Null
Start-Sleep -Seconds 38
& $drive cmd -Text "replay $($replay.Name)" -Playtest $Playtest
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    $state = & $drive cmd -Text 'state' -Playtest $Playtest
    if (($state -match 'replay 1 ') -and ($state -match 'inplay 1')) { "the replay's fight is up"; return }
}
"the replay never reached its fight"
