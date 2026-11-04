# Test helper: writes the game's clock and round counters to a file a few times a second, to see how they behave.
param([string]$Out, [int]$Seconds = 60)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\T8Cine"
$end = (Get-Date).AddSeconds($Seconds)
"" | Set-Content $Out
while ((Get-Date) -lt $end) {
    Remove-Item "$data\state.txt" -ErrorAction SilentlyContinue
    Set-Content "$data\commands.txt" -Value "state" -Encoding ascii
    $deadline = (Get-Date).AddSeconds(2)
    while ((Get-Date) -lt $deadline -and -not (Test-Path "$data\state.txt")) { Start-Sleep -Milliseconds 30 }
    if (Test-Path "$data\state.txt") {
        $s = Get-Content "$data\state.txt"
        $line = ($s | Where-Object { $_ -match '^(clock|steps|counters_p1|counters_p2) ' }) -join " | "
        "$((Get-Date).ToString('HH:mm:ss.ff')) $line" | Add-Content $Out
    }
    Start-Sleep -Milliseconds 250
}
