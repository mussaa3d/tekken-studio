# Test: in a fight, measures how much of the game runs at each speed, and that a step is one frame. The game writes its camera
# several times for each fight frame it runs (BlueSpark runs frames ahead), so "camera writes a game frame" at 1x is the yardstick,
# and at 0.25x it should be a quarter of that.
param([string]$Playtest = "$env:USERPROFILE\Downloads\Compressed\ttt2-playtest")
$drive = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'drive-ttt2.ps1'
function Cmd([string]$text) { & $drive cmd -Text $text -Playtest $Playtest }
function Num {
    $st = Cmd 'state'
    $w = [double](($st | Select-String 'camera_writes (\d+)').Matches[0].Groups[1].Value)
    $f = [double](($st | Select-String 'testframe (\d+)').Matches[0].Groups[1].Value)
    $c = [double](($st | Select-String 'clock (\S+)').Matches[0].Groups[1].Value)
    , @($w, $f, $c)
}
function Rate([string]$label, [int]$seconds) {
    $a = Num; Start-Sleep -Seconds $seconds; $b = Num
    "{0}: {1:N2} camera writes a game frame over {2} frames; the studio's clock +{3:N2} s" -f $label, (($b[0] - $a[0]) / [math]::Max($b[1] - $a[1], 1)), ($b[1] - $a[1]), ($b[2] - $a[2])
}
if (-not (Get-Process ttt2-studio -ErrorAction SilentlyContinue)) { "the studio isn't running"; return }
Cmd 'pause 0;speed 1' | Out-Null
Rate '1x' 3
Cmd 'speed 0.5' | Out-Null; Rate '0.5x' 3
Cmd 'speed 0.25' | Out-Null; Rate '0.25x' 3
Cmd 'speed 0.05' | Out-Null; Rate '0.05x' 4
Cmd 'speed 1;pause 1' | Out-Null
Start-Sleep -Milliseconds 600
$a = Num; Start-Sleep -Milliseconds 800; $b = Num
"paused: {0} camera writes in 0.8 s, clock +{1:N3} s" -f ($b[0] - $a[0]), ($b[2] - $a[2])
Cmd 'step' | Out-Null; Start-Sleep -Milliseconds 600; $c = Num
"one step: {0} camera writes, clock +{1:N3} s" -f ($c[0] - $b[0]), ($c[2] - $b[2])
Cmd 'step;step;step' | Out-Null; Start-Sleep -Milliseconds 800; $d = Num
"three steps: {0} camera writes, clock +{1:N3} s" -f ($d[0] - $c[0]), ($d[2] - $c[2])
Cmd 'pause 0' | Out-Null
