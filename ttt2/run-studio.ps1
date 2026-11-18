# Starts Tekken Studio for TTT2: the studio's copy of the game, always offline.
#
#   powershell -ExecutionPolicy Bypass -File run-studio.ps1 -Playtest C:\ttt2 [-Windowed] [-Extra '--some_switch=1']
#
# It uses the switches BlueSpark's own Play last wrote (rex\userdata\play-switches.txt), with everything online taken out: no
# sign-in, no network layer, no matchmaking, no log upload. The studio is for watching and filming replays, never for playing
# online, and BlueSpark's own ttt2.exe is the only one that goes online.
param(
    [Parameter(Mandatory)][string]$Playtest,
    [switch]$Windowed,
    [ValidateSet('simple', 'studio')][string]$Mode = 'simple',   # simple: the playback bar only; studio: the editor open
    [switch]$NoBlueSpark,   # the game's own main menu, without BlueSpark's pages over it
    [string[]]$Extra = @()
)
$ErrorActionPreference = 'Stop'
# (Started with powershell -File, a list of switches arrives as one string with commas: taken apart again here.)
$Extra = @($Extra | ForEach-Object { $_ -split ',(?=--)' } | Where-Object { $_ })
$rex = Join-Path $Playtest 'rex'
$exe = Join-Path $rex 'out\build\tekken-studio\ttt2-studio.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "The studio isn't built yet ($exe). Run build-studio.ps1 first." }
if (Get-Process ttt2, ttt2-studio -ErrorAction SilentlyContinue) { throw 'TTT2 is already running. Close it first: the studio and the game share your saves.' }

$data = Join-Path $env:LOCALAPPDATA 'TTT2Native'
$gameFolder = Join-Path $rex 'game-folder.txt'
$game = if (Test-Path -LiteralPath $gameFolder) { (Get-Content -LiteralPath $gameFolder -Raw).Trim() } else { Join-Path $data 'game' }

# Anything that signs in, connects, matches or uploads.
$online = '^--(live_|ttt2_live_|ttt2_fight_splice|ttt2_env_sync|ttt2_log_autoupload|ttt2_speculative_connect|ttt2_menu_queue|ttt2_training_queue_rows|' +
          'ttt2_direct_entry|ttt2_bluespark_update_banner|ttt2_voip|ttt2_queue_|ttt2_net_|ttt2_online_api|ttt2_upnp|ttt2_turn_)'
$switches = @()
$file = Join-Path $rex 'userdata\play-switches.txt'
if (Test-Path -LiteralPath $file) {
    $switches = @(Get-Content -LiteralPath $file | Where-Object { $_ -like '--*' -and $_ -notmatch $online })
    if ($NoBlueSpark) { $switches = @($switches | Where-Object { $_ -notlike '--ttt2_bluespark*' }) }
}
$arguments = @(
    "--game_data_root=$game",
    "--update_data_root=$(Join-Path $data 'update')",
    "--user_data_root=$(Join-Path $rex 'userdata')",
    "--profile_data_root=$(Join-Path $data 'profile')",
    "--hid_mappings_file=$(Join-Path $rex 'gamecontrollerdb.txt')",
    "--fullscreen=$(if ($Windowed) { 'false' } else { 'true' })",
    '--fullscreen_exclusive=false',
    '--ttt2_update_notice=false'   # the one thing the game would still fetch: BlueSpark's list of releases
) + $switches + $Extra
$quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
Set-Content -LiteralPath (Join-Path (Split-Path $exe) 'studio-launch.txt') -Value ($quoted -join "`r`n") -Encoding ascii
$studioData = Join-Path (Split-Path $exe) 'studio'
New-Item -ItemType Directory -Force $studioData | Out-Null
Set-Content -LiteralPath (Join-Path $studioData 'start-mode.txt') -Value $Mode -Encoding ascii -NoNewline
$process = Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory (Split-Path $exe) -PassThru
"studio process $($process.Id)"
