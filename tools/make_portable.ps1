# Makes the portable zip: the tool on its own, with no installer and no ffmpeg. Unzip it anywhere and run tekken-studio.exe.
# It keeps its settings, skins and replay vault in its own folder. For recording it uses ffmpeg from PATH, or an ffmpeg folder put
# next to it (ffmpeg\bin\ffmpeg.exe).
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$version = ((Get-Content "$root\installer\tekken8mvm.iss" | Select-String '#define AppVersion "(.+)"').Matches[0].Groups[1].Value)
$name = "tekken-studio-$version-portable"
$stage = Join-Path $env:TEMP $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\mod", "$stage\licenses" | Out-Null

Copy-Item "$root\dist\tekken-studio.exe" $stage
Copy-Item "$root\dist\mod\dinput8.dll" "$stage\mod"
Copy-Item "$root\README.md" $stage
# Tekken Studio for TTT2: the scripts and sources the launcher's TTT2 tab builds from.
New-Item -ItemType Directory -Force "$stage\ttt2\src" | Out-Null
Copy-Item "$root\ttt2\build-studio.ps1", "$root\ttt2\run-studio.ps1" "$stage\ttt2"
Copy-Item "$root\ttt2\src\*" "$stage\ttt2\src"
Copy-Item "$root\LICENSE" "$stage\LICENSE.txt"
# The licences of the libraries built into the tool, which ask to be passed on with it.
$licences = @{
    "third_party\imgui\LICENSE.txt"                     = "Dear ImGui.txt"
    "third_party\minhook\LICENSE.txt"                   = "MinHook.txt"
    "third_party\reshade\LICENSE.md"                    = "ReShade add-on API.txt"
    "third_party\fonts\Rubik-OFL.txt"                   = "Rubik font.txt"
    "third_party\fonts\FontAwesome-LICENSE.txt"         = "Font Awesome Free.txt"
    "third_party\fonts\IconFontCppHeaders-licence.txt"  = "IconFontCppHeaders.txt"
}
foreach ($entry in $licences.GetEnumerator()) { Copy-Item (Join-Path $root $entry.Key) (Join-Path "$stage\licenses" $entry.Value) }

New-Item -ItemType Directory -Force "$root\release" | Out-Null
$zip = "$root\release\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Each entry is written by hand with forward slashes in its path, so the mod folder survives any unzip tool. PowerShell 5's
# Compress-Archive and .NET Framework's ZipFile write backslashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File) {
        $entry = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Remove-Item $stage -Recurse -Force
Write-Host "Portable: $zip"
