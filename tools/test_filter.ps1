# Test: plays a stretch of a replay and prints how far the camera is from the fighters, with the camera filter off and on.
param([double]$From = 26, [double]$To = 40)
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
foreach ($filter in 0, 1) {
    "--- filter $filter"
    Send-Lines @("filter $filter", "mode game", "hud 1", "pause 0", "speed 1", "seek $From")
    Start-Sleep -Milliseconds 800
    $rows = @()
    while ($true) {
        $s = Read-State
        $clock = [double]((Field $s 'clock') -replace 'clock ', '')
        if ($clock -ge $To) { break }
        $v = (Field $s 'view') -split ' '
        $p1 = (Field $s 'p1') -split ' '
        $p2 = (Field $s 'p2') -split ' '
        if ($p1.Count -ge 4 -and $p2.Count -ge 4) {
            $mx = ([double]$p1[1] + [double]$p2[1]) / 2; $my = ([double]$p1[2] + [double]$p2[2]) / 2
            $d = [math]::Sqrt(([double]$v[1] - $mx) * ([double]$v[1] - $mx) + ([double]$v[2] - $my) * ([double]$v[2] - $my))
            $sep = [math]::Sqrt(([double]$p1[1] - [double]$p2[1]) * ([double]$p1[1] - [double]$p2[1]) + ([double]$p1[2] - [double]$p2[2]) * ([double]$p1[2] - [double]$p2[2]))
            $rows += ("{0,6:N2} cam {1,5:N0} sep {2,4:N0} fov {3,4:N0} {4} {5}" -f $clock, $d, $sep, [double]$v[7], (Field $s 'filter'), (Field $s 'drama'))
        }
        Start-Sleep -Milliseconds 350
    }
    $rows
}
Send-Lines @("filter 0", "hud 0", "pause 1")
