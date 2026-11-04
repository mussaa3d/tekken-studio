# Test helper: does what the launcher's cinematic session does, from a script. start | stop | status | log
# Leaves the launcher's own session handling to be tested separately.
param([Parameter(Mandatory)][string]$Action, [int]$Lines = 40, [string]$Command = "")

$root = Resolve-Path "$PSScriptRoot\.."
$game = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8"
$win64 = "$game\Polaris\Binaries\Win64"
$data = "$win64\tekken8mvm"

switch ($Action) {
    "start" {
        if (Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue) { "the game is already running"; return }
        New-Item -ItemType Directory -Force $data | Out-Null
        Copy-Item "$root\dist\mod\dinput8.dll" "$win64\dinput8.dll" -Force
        $ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
        Set-Content "$data\tekken8mvm.ini" -Value "ffmpeg_path = $ffmpeg`nonline_guard = 1`ntest_commands = 1`ntrace_saves = 1`noutput_dir = $root\build\game_out`n" -Encoding ascii
        Remove-Item "$data\commands.txt", "$data\state.txt" -ErrorAction SilentlyContinue
        Set-Content "$data\session.lock" -Value (Get-Date).ToString() -Encoding ascii
        Remove-Item "$data\tekken8mvm.log" -ErrorAction SilentlyContinue
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
    "cmd" {
        # Everything after -Action cmd comes through -Command, one line per command. Waits for the state file when asked for it.
        Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
        Set-Content "$data\commands.txt" -Value ($Command -split ';' | ForEach-Object { $_.Trim() }) -Encoding ascii
        if ($Command -match 'state') {
            $deadline = (Get-Date).AddSeconds(8)
            while ((Get-Date) -lt $deadline -and -not (Test-Path "$data\state.txt")) { Start-Sleep -Milliseconds 100 }
            if (Test-Path "$data\state.txt") { Get-Content "$data\state.txt" } else { "no state: the game thread isn't ticking the mod" }
        }
    }
    "log" {
        if (Test-Path "$data\tekken8mvm.log") { Get-Content "$data\tekken8mvm.log" -Tail $Lines | ForEach-Object { $_.Substring(0, [math]::Min(170, $_.Length)) } } else { "no log" }
    }
}
