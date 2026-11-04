# Test: runs a whole replay fast and prints every cinematic the game reports, with the round starts for comparison.
param([double]$Speed = 3, [int]$MaxSeconds = 120)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
$before = (Get-Content "$data\tekken8mvm.log").Count
Send-Lines @("filter 0", "mode game", "hud 0", "pause 0", "speed $Speed", "seek 1")
$end = (Get-Date).AddSeconds($MaxSeconds)
$last = -1; $still = 0
while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds 2
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 100 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 30 }
    $s = Get-Content "$data\state.txt" -ErrorAction SilentlyContinue
    $clock = [double](($s | Where-Object { $_ -match '^clock ' }) -replace 'clock ', '')
    if ($clock -eq $last) { if (++$still -gt 2) { break } } else { $still = 0 }
    $last = $clock
}
"ended at $last"
Send-Lines @("speed 1")
(Get-Content "$data\tekken8mvm.log") | Select-Object -Skip $before | Select-String 'Drama|Seek:' | ForEach-Object { $_.Line.Substring(15) }
