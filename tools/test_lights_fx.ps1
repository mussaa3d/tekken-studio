# Test: paused replay, free camera. Takes a screenshot's worth of settings one after another, waiting between so pictures can be taken.
param([string]$Step)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
Send-Lines ($Step -split ';' | ForEach-Object { $_.Trim() })
Start-Sleep -Milliseconds 1200
