# Every recording case the stand-in can check. Prints one PASS or FAIL line per case.
$root = Resolve-Path "$PSScriptRoot\..\.."
Set-Location $root
$results = @()

function Check($label, $file, [string[]]$checkArgs) {
    if (-not $file) { $line = "NO FILE" } else { $line = (python tools\harness\check_video.py $file @checkArgs) -join " " }
    "$label : $line"
}

"=== B: two streams, nothing to settle: alternate frames go to each file, in step"
& .\tools\harness\run_harness.ps1 -RecordFrames 60 -Looks "01" -Settle 0 | Where-Object { $_ -match "TIMED|stopped after" }
$files = Get-ChildItem build\harness_out\*.mp4 | Sort-Object Name
foreach ($f in $files) { Check $f.Name $f.FullName @("--step", "2", "--frames", "60") }

"=== C: two streams with 3 settle frames: each capture is 4 renders apart, so 8 per moment"
& .\tools\harness\run_harness.ps1 -RecordFrames 30 -Looks "01" -Settle 3 | Where-Object { $_ -match "TIMED|stopped after" }
$files = Get-ChildItem build\harness_out\*.mp4 | Sort-Object Name
foreach ($f in $files) { Check $f.Name $f.FullName @("--step", "8", "--frames", "30") }

"=== D: motion blur, 4 samples, 360 degree shutter: the bar moves 4 per frame with soft edges"
& .\tools\harness\run_harness.ps1 -RecordFrames 30 -Samples 4 -Shutter 360 | Where-Object { $_ -match "TIMED|stopped after" }
Check "blur" (Get-ChildItem build\harness_out\*.mp4 | Select-Object -First 1).FullName @("--step", "4", "--frames", "30", "--soft-edge")

"=== E: motion blur, 8 samples, 180 degree shutter: half the samples are blended"
& .\tools\harness\run_harness.ps1 -RecordFrames 30 -Samples 8 -Shutter 180 | Where-Object { $_ -match "TIMED|stopped after" }
Check "shutter" (Get-ChildItem build\harness_out\*.mp4 | Select-Object -First 1).FullName @("--step", "8", "--frames", "30", "--soft-edge")

"=== F: a depth stream on its own, which is one render per frame; the harness has no depth buffer, so it shows the plain picture"
& .\tools\harness\run_harness.ps1 -RecordFrames 30 -Looks "4" -Settle 0 | Where-Object { $_ -match "TIMED|stopped after" }
Check "depth" (Get-ChildItem build\harness_out\*.mp4 | Select-Object -First 1).FullName @("--frames", "30", "--no-bar")

"=== G: four streams at once"
& .\tools\harness\run_harness.ps1 -RecordFrames 20 -Looks "0123" -Settle 0 | Where-Object { $_ -match "TIMED|stopped after" }
foreach ($f in (Get-ChildItem build\harness_out\*.mp4 | Sort-Object Name)) { Check $f.Name $f.FullName @("--step", "4", "--frames", "20") }

"=== I: the video written smaller than the picture: 1280 x 720 from a 1920 x 1080 render, every frame there"
& .\tools\harness\run_harness.ps1 -RecordFrames 30 -OutputSize "1280x720" | Where-Object { $_ -match "TIMED|stopped after" }
$file = Get-ChildItem build\harness_out\*.mp4 | Select-Object -First 1
if ($file) {
    $probe = (ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=width,height,nb_read_frames -of csv=p=0 $file.FullName)
    "scaled : $probe " + $(if ($probe -eq "1280,720,30") { "PASS" } else { "FAIL" })
} else { "scaled : NO FILE" }

"=== J: an odd picture size is trimmed to even numbers for the formats that need them (an odd video size is asked for, 1279 x 719)"
& .\tools\harness\run_harness.ps1 -RecordFrames 20 -OutputSize "1279x719" | Where-Object { $_ -match "TIMED|stopped after" }
$file = Get-ChildItem build\harness_out\*.mp4 | Select-Object -First 1
if ($file) {
    $probe = (ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=width,height,nb_read_frames -of csv=p=0 $file.FullName)
    "even : $probe " + $(if ($probe -eq "1278,718,20") { "PASS" } else { "FAIL" })
} else { "even : NO FILE" }

"=== K: a render size the window can't take (the stand-in's picture never changes size): the recording gives up with a reason and the window goes back"
$lines = & .\tools\harness\run_harness.ps1 -Live -Frames 900 -RecordFrames 30 -CaptureSize "2560x1440" -TimeoutSeconds 90
$gaveUp = ($lines | Where-Object { $_ -match "didn't take the size 2560 x 1440" }).Count -gt 0
$back = ($lines | Where-Object { $_ -match "Window: back to" }).Count -gt 0
$timedOut = ($lines | Where-Object { $_ -match "TIMED OUT" }).Count -gt 0
"refused size : gave up with a reason $gaveUp, window back $back, hung $timedOut " + $(if ($gaveUp -and $back -and -not $timedOut) { "PASS" } else { "FAIL" })

"=== L: the editor test with a stand-in for ReShade: found, listed, set and keyed, and recording after its effects"
$lines = & .\tools\harness\run_harness.ps1 -UiTest -Live -ReShade -Frames 2000 -RecordFrames 0 -TimeoutSeconds 180
$fails = (Select-String -Path build\tekken8mvm\tekken8mvm.log -Pattern "\[uitest\] FAIL").Count
$passes = (Select-String -Path build\tekken8mvm\tekken8mvm.log -Pattern "\[uitest\] PASS").Count
"reshade editor test : $passes passed, $fails failed " + $(if ($fails -eq 0 -and $passes -ge 44) { "PASS" } else { "FAIL" })
& .\tools\harness\run_harness.ps1 -Live -ReShade -Frames 500 -RecordFrames 300 -TimeoutSeconds 120 | Where-Object { $_ -match "TIMED|stopped after" }
$file = Get-ChildItem build\harness_out\*.mp4 | Select-Object -First 1
if ($file) {
    $probe = (ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 $file.FullName)
    "reshade recording : $probe frames " + $(if ($probe -eq "300") { "PASS" } else { "FAIL" })
} else { "reshade recording : NO FILE" }

"=== H: audio pass against a test tone"
& .\tools\harness\run_harness.ps1 -RecordFrames 60 -Frames 240 -Audio | Where-Object { $_ -match "TIMED|Harness audio" }
if (Test-Path build\tekken8mvm\harness_audio.wav) {
    python -c "import wave,struct,sys,math; w=wave.open(r'build\tekken8mvm\harness_audio.wav'); n=w.getnframes(); d=w.readframes(n); s=struct.unpack('<%dh'%(len(d)//2),d); rms=math.sqrt(sum(x*x for x in s)/max(len(s),1)); print('audio:',w.getframerate(),'Hz',w.getnchannels(),'ch',round(n/w.getframerate(),2),'s rms',round(rms),'PASS' if rms>200 else 'FAIL')"
} else { "audio: NO FILE" }
