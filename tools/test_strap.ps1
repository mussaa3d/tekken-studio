# Test: a close look at how a fighter's costume moves at a slow timescale. Records frames with the fighter on greenscreen and writes how much
# changes from each frame to the next as a row of numbers, so a jump at each fight step shows up as a spike, plus a sheet of frames.
param([double]$At = 45, [double]$Speed = 0.01, [string]$Name = "strap", [int]$Frames = 260)
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
import sys, glob
from PIL import Image, ImageChops
folder, out = sys.argv[1], sys.argv[2]
files = sorted(glob.glob(folder + '/*.png'))
last = Image.open(files[0]).convert('L')
series = []
for f in files[1:]:
    now = Image.open(f).convert('L')
    hist = ImageChops.difference(last, now).histogram()
    series.append(sum(i * n for i, n in enumerate(hist)) / sum(hist))
    last = now
ordered = sorted(series)
median = ordered[len(ordered) // 2]
print('%d frames, typical change %.4f, largest %.4f, %d frames over 3x typical' % (len(files), median, ordered[-1], sum(1 for v in series if v > 3 * median)))
excess = sorted((series[i] - sorted(series[max(0, i - 6):i + 7])[len(series[max(0, i - 6):i + 7]) // 2], i) for i in range(len(series)))[-5:]
print('biggest jumps above their neighbours: ' + ', '.join('%.3f at frame %d' % e for e in reversed(excess)))
picks = [files[i * (len(files) - 1) // 7] for i in range(8)]
frames = [Image.open(f).convert('RGB') for f in picks]
green = Image.new('RGB', frames[0].size, (0, 255, 0))
box = ImageChops.difference(frames[0], green).getbbox()
crops = [f.crop(box) for f in frames]
w = 480
h = w * crops[0].height // crops[0].width
sheet = Image.new('RGB', (w * 4, h * 2))
for i, c in enumerate(crops):
    sheet.paste(c.resize((w, h)), ((i % 4) * w, (i // 4) * h))
sheet.save(out)
'@
$script | python -I - $folder.FullName "$root\build\frames\$Name.png"
Remove-Item $folder.FullName -Recurse -Force
