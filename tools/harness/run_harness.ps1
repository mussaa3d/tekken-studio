# Runs the D3D12 stand-in with the built mod. Killed after -TimeoutSeconds so a hang can't block anything.
param(
    [int]$Frames = 60,
    [int]$RecordFrames = 120,
    [string]$Looks = "",        # one digit per stream: 0 picture, 1 green, 2 stage, 3 fighters on black, 4 depth
    [int]$Settle = -1,
    [int]$Samples = 0,
    [int]$Shutter = 0,
    [switch]$Audio,
    [switch]$Screenshots,
    [switch]$UiTest,
    [switch]$Live,              # the preview's real tick and camera code
    [double]$Speed = 0,
    [switch]$Ramp,
    [switch]$Dolly,
    [switch]$ReShade,           # load the stand-in for ReShade first, so the mod's ReShade link runs
    [string]$CaptureSize = "",  # the size to render at while recording, as 2560x1440
    [string]$OutputSize = "",   # the size of the video, as 1280x720
    [int]$TimeoutSeconds = 60
)

$root = Resolve-Path "$PSScriptRoot\..\.."
Set-Location $root
$data = "build\tekken8mvm"
$out = "$root\build\harness_out"
New-Item -ItemType Directory -Force $data | Out-Null
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
Remove-Item -Force "build\harness_shot_*" -ErrorAction SilentlyContinue
Remove-Item -Force "$data\harness_audio.wav" -ErrorAction SilentlyContinue
Set-Content -Path "$data\tekken8mvm.ini" -Value "output_dir = $out`nrecord_fps = 60`n" -Encoding ascii

foreach ($name in "REC", "SHOT", "LOOKS", "SETTLE", "SAMPLES", "SHUTTER", "AUDIO", "UITEST", "PREVIEW", "SPEED", "RAMP", "DOLLY", "RESHADE", "CAPSIZE", "OUTSIZE") { Remove-Item "Env:\TEKKEN8MVM_HARNESS_$name" -ErrorAction SilentlyContinue }
$env:TEKKEN8MVM_HARNESS_REC = "$RecordFrames"
if ($UiTest) { $env:TEKKEN8MVM_HARNESS_UITEST = "1" }
if ($Live) { $env:TEKKEN8MVM_HARNESS_PREVIEW = "1" }
if ($Speed) { $env:TEKKEN8MVM_HARNESS_SPEED = "$Speed" }
if ($Ramp) { $env:TEKKEN8MVM_HARNESS_RAMP = "1" }
if ($Dolly) { $env:TEKKEN8MVM_HARNESS_DOLLY = "1" }
if ($ReShade) { $env:TEKKEN8MVM_HARNESS_RESHADE = "1" }
if ($CaptureSize) { $env:TEKKEN8MVM_HARNESS_CAPSIZE = $CaptureSize }
if ($OutputSize) { $env:TEKKEN8MVM_HARNESS_OUTSIZE = $OutputSize }
if ($Screenshots) { $env:TEKKEN8MVM_HARNESS_SHOT = "$root\build\harness_shot" }
if ($Looks) { $env:TEKKEN8MVM_HARNESS_LOOKS = $Looks }
if ($Settle -ge 0) { $env:TEKKEN8MVM_HARNESS_SETTLE = "$Settle" }
if ($Samples) { $env:TEKKEN8MVM_HARNESS_SAMPLES = "$Samples" }
if ($Shutter) { $env:TEKKEN8MVM_HARNESS_SHUTTER = "$Shutter" }
if ($Audio) { $env:TEKKEN8MVM_HARNESS_AUDIO = "1" }

$process = Start-Process -FilePath ".\build\tekken8mvm_harness.exe" -ArgumentList "build\dinput8.dll", "$Frames" -PassThru
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    "TIMED OUT after $TimeoutSeconds s"
    Stop-Process -Id $process.Id -Force
} else {
    "harness exit: $($process.ExitCode)"
}
Get-Content "$data\tekken8mvm.log" -ErrorAction SilentlyContinue | Where-Object { $_ -match "stopped after|exited with code|took over|failed|couldn't|Harness audio|Harness end|\[uitest\] (FAIL|finished)|ReShade:|Window:|Recorder: the game" } | ForEach-Object { $_.Substring(0, [math]::Min(150, $_.Length)) }
Get-ChildItem $out -ErrorAction SilentlyContinue | ForEach-Object { "{0,10} {1}" -f $_.Length, $_.Name }
if ($Screenshots) {
    Get-ChildItem "build\harness_shot_*.raw" -ErrorAction SilentlyContinue | ForEach-Object {
        ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgra -s 1920x1080 -i $_.FullName -frames:v 1 ($_.FullName -replace '\.raw$', '.png')
        "screenshot: $($_.BaseName).png"
    }
}
