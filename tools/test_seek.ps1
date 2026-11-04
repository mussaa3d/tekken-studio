# Test: seeks a playing replay back and forward from the mod's command channel and checks where it lands. Also prints where the
# fighters are, since a rewound replay has to play out exactly as the original did.
param([double[]]$Targets = @(20, 25, 15, 5, 60, 45, 90, 3, 110))
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\T8Cine"
function Send-Lines($lines) {
    for ($i = 0; $i -lt 100 -and (Test-Path "$data\commands.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
}
function State {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Send-Lines "state"
    for ($i = 0; $i -lt 300 -and -not (Test-Path "$data\state.txt"); $i++) { Start-Sleep -Milliseconds 20 }
    $s = Get-Content "$data\state.txt"
    $clock = [double](($s | Where-Object { $_ -match '^clock ' }) -replace 'clock ', '')
    $position = ($s | Where-Object { $_ -match '^position ' })
    [pscustomobject]@{ Clock = $clock; Position = $position }
}
function Wait-Seek {
    for ($i = 0; $i -lt 200; $i++) {
        Start-Sleep -Milliseconds 100
        $s = State
        if ($s.Position -match 'seeking 0') { return $s }
    }
    return State
}
Send-Lines "pause 1"
Start-Sleep -Milliseconds 300
$start = State
"paused at $($start.Clock)  [$($start.Position)]"
foreach ($target in $Targets) {
    $now = (State).Clock
    $t0 = Get-Date
    Send-Lines "seek $target"
    Start-Sleep -Milliseconds 120
    $landed = Wait-Seek
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    $off = [math]::Round(($landed.Clock - $target) * 60, 1)
    "from $now to $target : landed $($landed.Clock), off by $off frames, took $ms ms  [$($landed.Position)]"
}
