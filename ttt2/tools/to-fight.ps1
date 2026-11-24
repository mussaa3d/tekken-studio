# Test helper: starts the studio's copy of TTT2 in a window, as the launcher does (BlueSpark's pages on), and walks the menus into a
# Practice fight with the keyboard: B closes BlueSpark's page, then OFFLINE MODE, PRACTICE, the fighters and the stage. It keeps
# pressing A until the studio reports the fight's camera (level, 50 degrees, 3 to 8 m back), so it doesn't depend on timing.
#   to-fight.ps1 [-Extra '--switch=1',...]
param([string[]]$Extra = @(), [string]$Playtest = "$env:USERPROFILE\Downloads\Compressed\ttt2-playtest")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$drive = Join-Path $here 'drive-ttt2.ps1'
function Keys([string]$keys, [int]$gap = 700, [int]$hold = 120) { & $drive key -Keys $keys -GapMs $gap -HoldMs $hold -Playtest $Playtest | Out-Null }
function InFight {
    $state = & $drive cmd -Text 'state' -Playtest $Playtest
    $m = ($state | Select-String 'eye (\S+) (\S+) (\S+) target (\S+) (\S+) (\S+) fov (\S+)')
    if (-not $m) { return $false }
    $g = $m.Matches[0].Groups
    $dx = [double]$g[1].Value - [double]$g[4].Value; $dy = [double]$g[2].Value - [double]$g[5].Value; $dz = [double]$g[3].Value - [double]$g[6].Value
    $distance = [math]::Sqrt($dx * $dx + $dz * $dz)
    ([math]::Abs($dy) -lt 400) -and ([math]::Abs([double]$g[7].Value - 50) -lt 0.5) -and ($distance -gt 3000) -and ($distance -lt 8000)
}

Get-Process ttt2-studio -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
& (Join-Path (Split-Path $here) 'run-studio.ps1') -Playtest $Playtest -Windowed -Extra $Extra | Out-Null
Start-Sleep -Seconds 38
Keys 'k'                      # B: BlueSpark's page closes, the game's main menu is under it
Start-Sleep -Seconds 4
Keys 'j'                      # OFFLINE MODE
Start-Sleep -Seconds 4
Keys 's,s,s,s,s' 600 150      # down to PRACTICE (VS BATTLE is skipped while there's one controller)
Keys 'j'
Start-Sleep -Seconds 7
for ($i = 0; $i -lt 40; $i++) {
    if (InFight) { Start-Sleep -Seconds 3; Keys 'j' 1500; if (InFight) { "in the fight"; return } }
    # (A second fighter can't be the same as the first, so the cursor moves on before every other press.)
    if ($i % 2 -eq 1) { Keys 'd' 400 }
    Keys 'j' 1300
}
"never reached a fight"
