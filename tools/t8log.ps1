# Test helper: sends commands to the running mod, waits, then prints chosen lines of its state and the lines its log gained that match.
# Like t8.ps1, with the log as well, which is where most test commands answer.
#   t8log.ps1 "pause 1;prop 0 1 SM_Animal_alpaca -330 -160 2773 0 90 0 0.25" -Wait 1 -Show prop,actor -Match "Spawner|Actors"
param([string]$Command = "", [double]$Wait = 0.6, [string[]]$Show = @(), [string]$Match = "", [int]$Tail = 400, [int]$Width = 230)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
$log = "$data\tekken8mvm.log"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
$before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
if ($Command) { Send-Lines ($Command -split ';' | ForEach-Object { $_.Trim() }) }
Start-Sleep -Milliseconds ([int]($Wait * 1000))
if ($Show.Count -gt 0) {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    if (Test-Path "$data\state.txt") {
        $pattern = '^(' + ($Show -join '|') + ')'
        Get-Content "$data\state.txt" | Where-Object { $_ -match $pattern }
    } else { "no state: the game thread isn't ticking the mod" }
}
if ($Match) {
    $lines = Get-Content $log
    $new = if ($lines.Count -gt $before) { $lines[$before..($lines.Count - 1)] } else { @() }
    $new | Where-Object { $_ -match $Match -and $_ -notmatch '\[test\] state' } | Select-Object -Last $Tail | ForEach-Object { $_.Substring(0, [math]::Min($Width, $_.Length)) }
}
$p = Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue
if (-not $p) { "THE GAME IS NOT RUNNING" }
