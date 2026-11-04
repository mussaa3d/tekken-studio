# Test: presses each hotkey the way a keyboard does and reads the mod's state after it.
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\T8Cine"
function Read-State {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Set-Content "$data\commands.txt" -Value "state" -Encoding ascii
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Start-Sleep -Milliseconds 30
    Get-Content "$data\state.txt"
}
function Show($label) {
    $s = Read-State
    $pick = ($s | Where-Object { $_ -match '^(menu|paused|mode|speed|steps|rec active|keys) ' }) -join ' | '
    "$label : $pick"
}
function Press($key) {
    & "$PSScriptRoot\press.ps1" -Keys $key
    Start-Sleep -Milliseconds 300
}
Show "start"
Press insert; Show "insert"
Press insert; Show "insert again"
Press f8; Show "f8 pause"
$before = (Read-State | Where-Object { $_ -match '^steps ' })
Press period; Press period
$after = (Read-State | Where-Object { $_ -match '^steps ' })
"period x2: $before -> $after"
Press f8; Show "f8 resume"
Press lbracket; Show "[ slower"
Press rbracket; Show "] faster"
Press f5; Show "f5 free"
Press k; Show "k key"
Press f5; Show "f5 off"
Press f6; Show "f6 dolly"
Press f6; Show "f6 off"
Press f9; Start-Sleep -Milliseconds 500; "f9 pressed (HUD hidden: see the screenshot)"
