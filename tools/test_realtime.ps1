# Test: cloth jumps in real time, as you see it, not through the recorder. Captures the screen while the fight plays at a slow timescale and
# counts single frames with far more big changes than the frames around them. Pose the camera first, greenscreen look on, anti-aliasing off.
param([double]$At = 4.7, [double]$Speed = 0.023, [int]$Seconds = 7, [string]$Crop = "1920:1080:0:0")
$root = Resolve-Path "$PSScriptRoot\.."
$session = "$PSScriptRoot\session.ps1"
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function C($c) {
    for ($i = 0; $i -lt 100 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    & $session -Action cmd -Command $c
    Start-Sleep -Milliseconds 150
}
C "pause 1"; C "speed 1"; C "seek $At"; Start-Sleep -Seconds 3
C "speed $Speed"; C "pause 0"; Start-Sleep -Milliseconds 300
$video = "$root\build\frames\realtime.mkv"
$out = "$root\build\frames\rt"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter *.png | Remove-Item -Force
ffmpeg -v error -y -f lavfi -i "ddagrab=framerate=60:draw_mouse=0" -t $Seconds -vf "hwdownload,format=bgra,crop=$Crop,format=yuv444p" -c:v libx264 -preset ultrafast -qp 0 $video
C "pause 1"; C "speed 1"
ffmpeg -v error -y -i $video -vsync 0 "$out\f%05d.png"
$script = @'
import sys, glob, statistics as S
from PIL import Image, ImageChops
files = sorted(glob.glob(sys.argv[1] + '/*.png'))
last = Image.open(files[0]).convert('L')
d = []
same = 0
for f in files[1:]:
    now = Image.open(f).convert('L')
    h = ImageChops.difference(last, now).histogram()
    v = 1e4 * sum(h[70:]) / sum(h)
    if sum(h[3:]) == 0:
        same += 1
    else:
        d.append(v)
    last = now
hops = [i for i in range(1, len(d) - 1) if d[i] > 2.5 * S.median(d[max(0, i - 10):i + 11]) + 6 and d[i] >= d[i - 1] and d[i] >= d[i + 1]]
print('%d frames (%d repeats): big-change pixels per 10000, typical %.1f, largest %.1f, %d jumps %s gaps %s' % (len(files), same, S.median(d), max(d), len(hops), hops[:14], [b-a for a,b in zip(hops,hops[1:])][:13]))
'@
$script | python -I - $out
