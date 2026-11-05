# Test: does cloth jump in single frames at a slow timescale? Records with the fighters on greenscreen and counts frames that change far more
# than the frames around them. Pose the camera first, with anti-aliasing off (r.AntiAliasingMethod 0).
param([double]$At = 4.7, [double]$Speed = 0.023, [int]$Frames = 260, [string]$Name = "hops")
$root = Resolve-Path "$PSScriptRoot\.."
$session = "$PSScriptRoot\session.ps1"
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function C($c) {
    for ($i = 0; $i -lt 100 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    & $session -Action cmd -Command $c
    Start-Sleep -Milliseconds 150
}
function Field($name) { & $session -Action cmd -Command "state" | Where-Object { $_ -match "^$name " } }
C "pause 1"; C "speed 1"; C "stream 0 1 1"; C "look 1"; C "codec 4"; C "seek $At"; Start-Sleep -Seconds 3
$before = Get-ChildItem "$root\build\game_out" -Directory -ErrorAction SilentlyContinue | ForEach-Object Name
C "speed $Speed"; C "pause 0"; C "rec start"
for ($i = 0; $i -lt 900; $i++) {
    Start-Sleep -Milliseconds 200
    if ((Field 'rec') -match 'frames (\d+)' -and [int]$Matches[1] -ge $Frames) { break }
}
C "rec stop"; C "pause 1"; C "speed 1"; Start-Sleep -Seconds 3
$folder = Get-ChildItem "$root\build\game_out" -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $folder) { "no frames were recorded"; return }
$script = @'
import sys, glob, statistics as S
from PIL import Image, ImageChops
files = sorted(glob.glob(sys.argv[1] + '/*.png'))
last = Image.open(files[0]).convert('L')
d = []
for f in files[1:]:
    now = Image.open(f).convert('L')
    h = ImageChops.difference(last, now).histogram()
    d.append(1e4 * sum(h[70:]) / sum(h))
    last = now
med = S.median(d)
hops = [i for i in range(1, len(d) - 1) if d[i] > 2.5 * S.median(d[max(0, i - 10):i + 11]) + 6 and d[i] >= d[i - 1] and d[i] >= d[i + 1]]
print('%d frames: big-change pixels per 10000, typical %.1f, largest %.1f, %d jumps %s gaps %s' % (len(files), med, max(d), len(hops), hops[:14], [b-a for a,b in zip(hops,hops[1:])][:13]))
'@
$script | python -I - $folder.FullName
Remove-Item $folder.FullName -Recurse -Force
