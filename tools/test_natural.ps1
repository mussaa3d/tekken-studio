# Test: does playing on at normal speed from a landing give the same result as landing at the later moment directly?
param([double]$At = 40)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\T8Cine"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
function Read-State {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    try { Get-Content "$data\state.txt" -ErrorAction Stop } catch { Start-Sleep -Milliseconds 80; Get-Content "$data\state.txt" }
}
function Field($s, $name) { ($s | Where-Object { $_ -match "^$name " }) }
function Wait-Seek {
    for ($i = 0; $i -lt 300; $i++) {
        Start-Sleep -Milliseconds 80
        $s = Read-State
        if ((Field $s 'position') -match 'seeking 0') { return $s }
    }
    return Read-State
}
function Pair($s) { (Field $s 'clock') + " | " + (Field $s 'p1') + " | " + (Field $s 'p2') }
Send-Lines @("filter 0", "pause 1", "speed 1", "seek $At")
Start-Sleep -Milliseconds 150
"seek directly:      " + (Pair (Wait-Seek))
Send-Lines "seek 10"; Start-Sleep -Milliseconds 150; Wait-Seek | Out-Null
Send-Lines "seek $At"; Start-Sleep -Milliseconds 150
"seek directly again:" + (Pair (Wait-Seek))
foreach ($back in 1, 3, 6) {
    $from = $At - $back
    Send-Lines "seek $from"; Start-Sleep -Milliseconds 150; Wait-Seek | Out-Null
    # One game step per rendered frame, like normal playback, up to the moment.
    Send-Lines "step $($back * 60)"
    for ($i = 0; $i -lt 600; $i++) {
        Start-Sleep -Milliseconds 100
        $s = Read-State
        if ([double]((Field $s 'clock') -replace 'clock ', '') -ge $At) { break }
    }
    Start-Sleep -Milliseconds 300
    "$back s of steps:      " + (Pair (Read-State))
}
