# Test helper: sends commands to the running mod, one call each, and prints chosen lines of its state.
#   t8.ps1 "seek 12;pause 1" -Show clock,position -Wait 1.5
param([string]$Command = "", [string[]]$Show = @("clock", "position", "paused", "rec "), [double]$Wait = 0.2)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\T8Cine"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 100 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
if ($Command) { Send-Lines ($Command -split ';' | ForEach-Object { $_.Trim() }) }
Start-Sleep -Milliseconds ([int]($Wait * 1000))
Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
Send-Lines "state"
for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
if (Test-Path "$data\state.txt") {
    $pattern = '^(' + ($Show -join '|') + ')'
    (Get-Content "$data\state.txt" | Where-Object { $_ -match $pattern }) -join ' | '
} else { "no state" }
