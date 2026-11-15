# Test: records a stretch of a playing replay once, at whatever picture sizes and streams -Setup asks for, waits for the files and says
# what's in them: the size, the number of frames, and whether the game window went back afterwards.
#   test_record_size.ps1 -In 29 -Out 30 -Setup "capsize 3840 2160;outsize 1920 1080;stream 0 1 0;stream 1 1 1"
# capsize is the size the game renders at (0 0 = the window's own), outsize the size of the video (0 0 = as rendered).
param([double]$In = 29, [double]$Out = 30, [string]$Setup = "", [int]$TimeoutSeconds = 240)
$d = $PSScriptRoot
$outDir = "$(Resolve-Path "$PSScriptRoot\..")\build\game_out" # where session.ps1 points the recorder
$before = Get-Date
& "$d\t8log.ps1" "pause 1;seek $($In - 0.5)" -Wait 3 | Out-Null
& "$d\t8log.ps1" ($Setup + ";range $In $Out 1;pause 0") -Wait 1 | Out-Null
$started = $false; $state = @()
for ($i = 0; $i -lt $TimeoutSeconds; $i++) {
    Start-Sleep -Seconds 1
    $state = & "$d\t8log.ps1" "" -Wait 0.05 -Show "rec ", "rec_error", "picture"
    $line = $state -join ' | '
    if ($line -match 'active 1') { $started = $true }
    if ($line -match 'rec_error \S') { break }
    if ($started -and $line -match 'active 0 finishing 0') { break }
}
& "$d\t8log.ps1" "pause 1" -Wait 1.5 | Out-Null
$state
(& "$d\t8log.ps1" "" -Wait 0.5 -Show picture) -join ' '
$log = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm\tekken8mvm.log"
Get-Content $log | Select-Object -Last 60 | Where-Object { $_ -match "Window:|Recorder: (stopped|the game|ffmpeg exited|couldn)|Depth: reading" } | ForEach-Object { $_.Substring(0, [math]::Min(200, $_.Length)) }
Get-ChildItem $outDir -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $before -and $_.Extension -in ".mp4", ".mov" } | ForEach-Object {
    $probe = ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=width,height,nb_read_frames -of csv=p=0 $_.FullName
    "{0}: {1} (width,height,frames), {2:N1} MB" -f $_.Name, $probe, ($_.Length / 1MB)
}
# The range is left switched off: armed, it would start a recording by itself the next time the clock passed through it.
& "$d\t8log.ps1" "range 0 10 0" -Wait 0.5 | Out-Null
