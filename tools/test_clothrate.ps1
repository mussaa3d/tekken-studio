# Test: does cloth move at the timescale? Records a stretch at a slow speed on the greenscreen look and writes a sheet of evenly spaced frames,
# plus how much of the picture changes from each frame to the next. Pose the camera first. Anti-aliasing should be off (r.AntiAliasingMethod 0).
param([double]$At = 8, [double]$Speed = 0.01, [string]$Name = "rate", [int]$Frames = 300, [int]$Shots = 8)
$root = Resolve-Path "$PSScriptRoot\.."
$session = "$PSScriptRoot\session.ps1"
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function C($c) {
    for ($i = 0; $i -lt 100 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    & $session -Action cmd -Command $c
    Start-Sleep -Milliseconds 150
}
function Field($name) { & $session -Action cmd -Command "state" | Where-Object { $_ -match "^$name " } }
C "pause 1"; C "speed 1"; C "stream 0 1 1"; C "look 1"; C "codec 4"
C "seek $At"; Start-Sleep -Seconds 3
$before = Get-ChildItem "$root\build\game_out" -Directory -ErrorAction SilentlyContinue | ForEach-Object Name
C "speed $Speed"; C "pause 0"; C "rec start"
for ($i = 0; $i -lt 900; $i++) {
    Start-Sleep -Milliseconds 200
    if ((Field 'rec') -match 'frames (\d+)' -and [int]$Matches[1] -ge $Frames) { break }
}
C "rec stop"; C "pause 1"; C "speed 1"; Start-Sleep -Seconds 3
"clock after: " + (Field 'clock')
$folder = Get-ChildItem "$root\build\game_out" -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $folder) { "no frames were recorded"; return }
$script = @'
import sys, glob
from PIL import Image, ImageChops
folder, out, shots = sys.argv[1], sys.argv[2], int(sys.argv[3])
files = sorted(glob.glob(folder + '/*.png'))
picks = [files[i * (len(files) - 1) // (shots - 1)] for i in range(shots)]
frames = [Image.open(f).convert('RGB') for f in picks]
green = Image.new('RGB', frames[0].size, (0, 255, 0))
box = None
for f in frames:
    b = ImageChops.difference(f, green).getbbox()
    box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
w = 480
crops = [f.crop(box) for f in frames]
h = w * crops[0].height // crops[0].width
sheet = Image.new('RGB', (w * 4, h * ((shots + 3) // 4)))
for i, c in enumerate(crops):
    sheet.paste(c.resize((w, h)), ((i % 4) * w, (i // 4) * h))
sheet.save(out)
steps = []
step = 1
last = Image.open(files[0]).convert('L')
for f in files[step::step]:
    now = Image.open(f).convert('L')
    hist = ImageChops.difference(last, now).histogram()
    steps.append(100 * sum(hist[9:]) / sum(hist))
    last = now
ordered = sorted(steps)
print('%d frames. Change from each frame to the next: typical %.3f%%, largest %.3f%%' % (len(files), ordered[len(ordered) // 2], ordered[-1]))
'@
$script | python -I - $folder.FullName "$root\build\frames\$Name.png" $Shots
Remove-Item $folder.FullName -Recurse -Force
