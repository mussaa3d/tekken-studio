# Test: records a few seconds of a paused moment as PNGs and reports how much of the picture changed. A still world gives close to zero.
# Set r.TemporalAASamples 1 first, or the renderer's jitter shows up as change. -Setup runs test commands first, separated by ;
param([string]$Setup = "", [int]$Seconds = 3)
$root = Resolve-Path "$PSScriptRoot\.."
$session = "$PSScriptRoot\session.ps1"
foreach ($line in ($Setup -split ';' | Where-Object { $_.Trim() })) {
    & $session -Action cmd -Command $line.Trim()
    Start-Sleep -Milliseconds 300
}
Start-Sleep -Seconds 1
$before = Get-ChildItem "$root\build\game_out" -Directory -ErrorAction SilentlyContinue | ForEach-Object Name
& $session -Action cmd -Command "codec 4"; Start-Sleep -Milliseconds 300
& $session -Action cmd -Command "rec start"; Start-Sleep -Seconds $Seconds
& $session -Action cmd -Command "rec stop"; Start-Sleep -Seconds 3
$folder = Get-ChildItem "$root\build\game_out" -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $folder) { "no frames were recorded"; return }
$script = @'
import sys, glob
from PIL import Image, ImageChops
files = sorted(glob.glob(sys.argv[1] + '/*.png'))
a, b = Image.open(files[3]).convert('L'), Image.open(files[-2]).convert('L')
histogram = ImageChops.difference(a, b).histogram()
print('%d frames, %.3f%% of the picture changed' % (len(files), 100 * sum(histogram[5:]) / sum(histogram)))
'@
$script | python -I - $folder.FullName
Remove-Item $folder.FullName -Recurse -Force
