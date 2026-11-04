# Test helper: does what the launcher's cinematic session does, from a script. start | stop | status | log
# Leaves the launcher's own session handling to be tested separately.
param([Parameter(Mandatory)][string]$Action, [int]$Lines = 40)

$root = Resolve-Path "$PSScriptRoot\.."
$game = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8"
$win64 = "$game\Polaris\Binaries\Win64"
$data = "$win64\T8Cine"

switch ($Action) {
    "start" {
        if (Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue) { "the game is already running"; return }
        New-Item -ItemType Directory -Force $data | Out-Null
        Copy-Item "$root\dist\mod\dinput8.dll" "$win64\dinput8.dll" -Force
        $ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
        Set-Content "$data\t8cine.ini" -Value "ffmpeg_path = $ffmpeg`nonline_guard = 1`n" -Encoding ascii
        Set-Content "$data\session.lock" -Value (Get-Date).ToString() -Encoding ascii
        Remove-Item "$data\t8cine.log" -ErrorAction SilentlyContinue
        Start-Process "steam://rungameid/1778820"
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and -not (Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue)) { Start-Sleep -Seconds 2 }
        $p = Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue
        if ($p) { "game process $($p.Id)" } else { "no game process after 60 s" }
    }
    "stop" {
        $p = Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue
        if ($p) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 3 }
        Remove-Item "$win64\dinput8.dll" -ErrorAction SilentlyContinue
        Remove-Item "$data\session.lock" -ErrorAction SilentlyContinue
        "stopped; mod removed: " + (-not (Test-Path "$win64\dinput8.dll"))
    }
    "status" {
        $p = Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue
        if ($p) { "running $($p.Id), responding $($p.Responding), $([math]::Round($p.WorkingSet64 / 1GB, 1)) GB" } else { "not running" }
        "mod installed: " + (Test-Path "$win64\dinput8.dll")
    }
    "log" {
        if (Test-Path "$data\t8cine.log") { Get-Content "$data\t8cine.log" -Tail $Lines | ForEach-Object { $_.Substring(0, [math]::Min(170, $_.Length)) } } else { "no log" }
    }
}
