# Test: runs through a replay and lists where the game's camera leaves its normal distance from the fighters.
param([double]$From = 5, [double]$To = 112, [double]$Speed = 2)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\T8Cine"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 200 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
function Read-State {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    try { Get-Content "$data\state.txt" -ErrorAction Stop } catch { Start-Sleep -Milliseconds 50; Get-Content "$data\state.txt" }
}
function Field($s, $name) { ($s | Where-Object { $_ -match "^$name " }) }
Send-Lines @("filter 0", "mode game", "hud 1", "pause 0", "speed $Speed", "seek $From")
Start-Sleep -Milliseconds 800
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    $s = Read-State
    $clock = [double]((Field $s 'clock') -replace 'clock ', '')
    if ($clock -ge $To) { break }
    $v = (Field $s 'view') -split ' '
    $p1 = (Field $s 'p1') -split ' '
    $p2 = (Field $s 'p2') -split ' '
    if ($p1.Count -ge 4 -and $p2.Count -ge 4) {
        $mx = ([double]$p1[1] + [double]$p2[1]) / 2; $my = ([double]$p1[2] + [double]$p2[2]) / 2
        $d = [math]::Sqrt(([double]$v[1] - $mx) * ([double]$v[1] - $mx) + ([double]$v[2] - $my) * ([double]$v[2] - $my))
        $fov = [double]$v[7]
        if ($d -lt 300 -or $d -gt 480 -or $fov -ne 65) {
            "{0,6:N2} cam {1,5:N0} fov {2,4:N0} {3}" -f $clock, $d, $fov, (Field $s 'drama')
        }
    }
    Start-Sleep -Milliseconds 120
}
Send-Lines @("speed 1", "hud 0", "pause 1")
