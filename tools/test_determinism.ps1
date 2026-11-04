# Test: a rewound replay has to play out exactly as the original did. Plays forward to a moment and notes where the fighters are,
# then rewinds to several earlier places and runs forward to the same moment. Each time they must be in the same place.
param([double]$At = 40, [string]$From = "25,12,37,5,30,39")
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
function Read-State {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    try { Get-Content "$data\state.txt" -ErrorAction Stop } catch { Start-Sleep -Milliseconds 60; Get-Content "$data\state.txt" }
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
Send-Lines @("filter 0", "pause 1", "speed 1")
Start-Sleep -Milliseconds 300
foreach ($start in ($From -split ',')) {
    Send-Lines "seek $start"
    Start-Sleep -Milliseconds 120
    Wait-Seek | Out-Null
    Send-Lines "seek $At"
    Start-Sleep -Milliseconds 120
    $s = Wait-Seek
    "via $start : " + (Field $s 'clock') + " | " + (Field $s 'p1') + " | " + (Field $s 'p2')
}
