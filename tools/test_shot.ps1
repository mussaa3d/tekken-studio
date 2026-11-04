# Test: builds a three-key camera path around a moment in a playing replay, previews it, then records it from the first frame.
param([double]$At = 30, [double]$Length = 4, [string]$Streams = "", [string]$Extra = "")
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
function State {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Get-Content "$data\state.txt"
}
function Get-Field($state, $name) { ($state | Where-Object { $_ -match "^$name " }) }
function Wait-Seek {
    for ($i = 0; $i -lt 200; $i++) {
        Start-Sleep -Milliseconds 100
        $s = State
        if ((Get-Field $s 'position') -match 'seeking 0') { return $s }
    }
}
Send-Lines @("pause 1", "hud 1", "menu 0", "seek $At")
Start-Sleep -Milliseconds 200
$s = Wait-Seek
"at: " + (Get-Field $s 'clock') + " | " + (Get-Field $s 'p1') + " | " + (Get-Field $s 'p2')
$p1 = ((Get-Field $s 'p1') -split ' ')[1..3] | ForEach-Object { [double]$_ }
$p2 = ((Get-Field $s 'p2') -split ' ')[1..3] | ForEach-Object { [double]$_ }
$mx = ($p1[0] + $p2[0]) / 2; $my = ($p1[1] + $p2[1]) / 2; $mz = ($p1[2] + $p2[2]) / 2 + 60
$radius = 320
function Add-Key($time, $angle, $fov) {
    $rad = $angle * [math]::PI / 180
    $x = $mx + $radius * [math]::Cos($rad); $y = $my + $radius * [math]::Sin($rad); $z = $mz + 30
    $yaw = $angle + 180
    Send-Lines @("seek $time")
    Start-Sleep -Milliseconds 150
    Wait-Seek | Out-Null
    Send-Lines @("pose $x $y $z -5 $yaw 0 $fov", "addkey")
    Start-Sleep -Milliseconds 300
}
Send-Lines "clearkeys"
Add-Key $At 20 50
Add-Key ($At + $Length / 2) 80 40
Add-Key ($At + $Length) 150 55
$s = State
"keys: " + (Get-Field $s 'keys')
if ($Extra) { Send-Lines ($Extra -split ';' | ForEach-Object { $_.Trim() }); Start-Sleep -Milliseconds 300 }
"-- preview"
$t0 = Get-Date
Send-Lines "preview"
Start-Sleep -Milliseconds 300
$s = Wait-Seek
"landed: " + (Get-Field $s 'clock') + " mode " + (Get-Field $s 'mode') + " paused " + (Get-Field $s 'paused')
for ($i = 0; $i -lt 200; $i++) {
    Start-Sleep -Milliseconds 100
    $s = State
    if ((Get-Field $s 'paused') -eq 'paused 1') { break }
}
"preview took $([int]((Get-Date) - $t0).TotalSeconds) s, stopped at: " + (Get-Field $s 'clock') + " " + (Get-Field $s 'paused')
"-- record"
if ($Streams) { Send-Lines ($Streams -split ';' | ForEach-Object { $_.Trim() }); Start-Sleep -Milliseconds 300 }
$t0 = Get-Date
Send-Lines "recordshot"
Start-Sleep -Milliseconds 400
$started = $false
for ($i = 0; $i -lt 2400; $i++) {
    Start-Sleep -Milliseconds 200
    $s = State
    $rec = Get-Field $s 'rec'
    if ($rec -match 'active 1') { $started = $true }
    if ($started -and $rec -match 'active 0 finishing 0') { break }
}
"recording took $([int]((Get-Date) - $t0).TotalSeconds) s: $rec"
"file: " + (Get-Field $s 'rec_file')
"error: " + (Get-Field $s 'rec_error')
"clock: " + (Get-Field $s 'clock') + " " + (Get-Field $s 'paused')
Send-Lines @("mode game", "hud 0")
