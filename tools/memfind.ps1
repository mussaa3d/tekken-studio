# Test: finds text in the game's memory through the mod's command channel and prints the results.
param([string[]]$Text, [int]$Wait = 14)
$data = "C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\tekken8mvm"
$lines = foreach ($t in $Text) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($t)
    "memfind " + (($bytes | ForEach-Object { $_.ToString('X2') }) -join '')
}
Set-Content "$data\commands.txt" -Value $lines -Encoding ascii
Start-Sleep -Seconds ($Wait * $Text.Count)
Get-Content "$data\tekken8mvm.log" -Tail 80 | Select-String 'memfind' | ForEach-Object { $_.Line.Substring(15) }
