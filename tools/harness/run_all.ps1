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

"=== H: audio pass against a test tone"
& .\tools\harness\run_harness.ps1 -RecordFrames 60 -Frames 240 -Audio | Where-Object { $_ -match "TIMED|Harness audio" }
if (Test-Path build\tekken8mvm\harness_audio.wav) {
    python -c "import wave,struct,sys,math; w=wave.open(r'build\tekken8mvm\harness_audio.wav'); n=w.getnframes(); d=w.readframes(n); s=struct.unpack('<%dh'%(len(d)//2),d); rms=math.sqrt(sum(x*x for x in s)/max(len(s),1)); print('audio:',w.getframerate(),'Hz',w.getnchannels(),'ch',round(n/w.getframerate(),2),'s rms',round(rms),'PASS' if rms>200 else 'FAIL')"
} else { "audio: NO FILE" }
