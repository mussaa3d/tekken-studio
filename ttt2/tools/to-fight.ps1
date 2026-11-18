# Test helper: starts the studio's copy of TTT2 in a window and walks the game's own menus into an Arcade fight with the keyboard.
# It keeps pressing A until the studio reports the fight's camera (the eye about 4.6 m from what it looks at), so it doesn't depend
# on how long each screen takes.
#   to-fight.ps1 [-Extra '--switch=1',...]
param([string[]]$Extra = @(), [string]$Playtest = "C:\Users\ms3ody\Downloads\Compressed\ttt2-playtest")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$drive = Join-Path $here 'drive-ttt2.ps1'
$logs = Join-Path $Playtest 'rex\out\build\tekken-studio\logs'
function Keys([string]$keys, [int]$gap = 700) { & $drive key -Keys $keys -GapMs $gap -Playtest $Playtest | Out-Null }
function InFight {
    $state = & $drive cmd -Text 'state' -Playtest $Playtest
    $m = ($state | Select-String 'eye (\S+) (\S+) (\S+) target (\S+) (\S+) (\S+) fov (\S+)')
    if (-not $m) { return $false }
    $g = $m.Matches[0].Groups
    $dx = [double]$g[1].Value - [double]$g[4].Value; $dy = [double]$g[2].Value - [double]$g[5].Value; $dz = [double]$g[3].Value - [double]$g[6].Value
    $distance = [math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
    # The fight's own camera: level, 50 degrees, 3 to 8 m back.
    ([math]::Abs($dy) -lt 400) -and ([math]::Abs([double]$g[7].Value - 50) -lt 0.5) -and ($distance -gt 3000) -and ($distance -lt 8000)
}

Get-Process ttt2-studio -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
& (Join-Path (Split-Path $here) 'run-studio.ps1') -Playtest $Playtest -Windowed -NoBlueSpark -Extra $Extra | Out-Null
$up = $false
for ($i = 0; $i -lt 180 -and -not $up; $i++) {
    Start-Sleep -Milliseconds 500
    $log = Get-ChildItem $logs -Filter 'ttt2_*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    $up = $log -and (Select-String -LiteralPath $log.FullName -Pattern "No intro: the main menu's bar up" -Quiet)
}
if (-not $up) { "the main menu never came"; return }
Start-Sleep -Seconds 3
Keys 'j'                      # OFFLINE MODE
Start-Sleep -Seconds 3
Keys 'j'                      # ARCADE BATTLE
Start-Sleep -Seconds 7
Keys 'j,d,j' 900              # two fighters
for ($i = 0; $i -lt 40; $i++) {
    if (InFight) { Start-Sleep -Seconds 2; if (InFight) { "in the fight"; return } }
    # (A second fighter can't be the same as the first, so the cursor moves on before every other press.)
    if ($i % 2 -eq 0) { Keys 'd' 500 }
    Keys 'j' 1300
}
"never reached a fight"
