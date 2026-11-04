# Test: is a moment the same however you get there? Grabs the same tick by several routes and reports how much of the picture differs
# from the first. Run with a fixed camera posed on a fighter and anti-aliasing off (r.AntiAliasingMethod 0).
param([double]$At = 45)
$root = Resolve-Path "$PSScriptRoot\.."
$session = "$PSScriptRoot\session.ps1"
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function C($c) {
    for ($i = 0; $i -lt 100 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    & $session -Action cmd -Command $c
    Start-Sleep -Milliseconds 120
}
function Clock { [double](((& $session -Action cmd -Command "state" | Where-Object { $_ -match '^clock ' }) -split ' ')[1]) }
function Grab($name) {
    $before = Get-ChildItem "$root\build\game_out" -Directory -ErrorAction SilentlyContinue | ForEach-Object Name
    C "codec 4"; C "rec start"; Start-Sleep -Seconds 1; C "rec stop"; Start-Sleep -Seconds 2
    $folder = Get-ChildItem "$root\build\game_out" -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
    $out = "$root\build\frames\cloth_$name.png"
    Copy-Item (Get-ChildItem $folder.FullName -Filter *.png | Select-Object -Last 1).FullName $out -Force
    Remove-Item $folder.FullName -Recurse -Force
    Write-Host ("{0}: clock {1:n4}" -f $name, (Clock))
    $out
}
function PlayUpTo($speed) {
    # Starts half a second early, plays at this speed, stops just short and steps onto the tick.
    C "pause 1"; C "speed 1"; C "seek $($At - 0.5)"; Start-Sleep -Seconds 3
    C "speed $speed"; C "pause 0"
    for ($i = 0; $i -lt 2000; $i++) {
        if ((Clock) -ge $At - 0.08) { break }
    }
    C "pause 1"; C "speed 1"; Start-Sleep -Milliseconds 300
    for ($i = 0; $i -lt 40 -and (Clock) -lt $At - 0.001; $i++) { C "step"; Start-Sleep -Milliseconds 150 }
}
$shots = @()
C "pause 1"; C "speed 1"
C "seek $At"; Start-Sleep -Seconds 3; $shots += Grab "seek1"
C "seek $($At + 20)"; Start-Sleep -Seconds 3; C "seek $At"; Start-Sleep -Seconds 3; $shots += Grab "seek2"
C "seek $($At - 30)"; Start-Sleep -Seconds 3; C "seek $At"; Start-Sleep -Seconds 3; $shots += Grab "seek3"
foreach ($speed in 0.25, 0.1, 0.05) {
    PlayUpTo $speed
    $shots += Grab "speed$speed"
}
$script = @'
import sys
from PIL import Image, ImageChops
files = sys.argv[1:]
base = Image.open(files[0]).convert('L')
for name in files[1:]:
    h = ImageChops.difference(base, Image.open(name).convert('L')).histogram()
    print('%-12s differs from the first by %.3f%%' % (name.split('cloth_')[-1], 100 * sum(h[9:]) / sum(h)))
'@
$script | python -I - @shots
