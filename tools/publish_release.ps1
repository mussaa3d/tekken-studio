# Publishes the current installer and portable zip as a GitHub release of the tekken-studio repo. If the release is already
# there, its files are replaced.
# Usage:  powershell -File tools\publish_release.ps1 [-Notes "What changed"]
# Needs the GitHub CLI signed in (gh auth login). The tool offers the newest release as an update.
param([string]$Notes = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$repo = "tekken-studio"
$owner = (gh api user --jq .login)
$version = ((Get-Content "$root\installer\tekken8mvm.iss" | Select-String '#define AppVersion "(.+)"').Matches[0].Groups[1].Value)
$setup = "$root\release\tekken-studio-$version-setup.exe"
$portable = "$root\release\tekken-studio-$version-portable.zip"
foreach ($file in $setup, $portable) {
    if (-not (Test-Path $file)) { throw "No $file. Run installer\build_installer.cmd first." }
}

# Nothing private goes out: no signing key, token or source in the installer.
$bytes = [IO.File]::ReadAllBytes($setup)
$text = [Text.Encoding]::ASCII.GetString($bytes)
foreach ($bad in "BEGIN EC PRIVATE KEY", "BEGIN PRIVATE KEY", "signing_key") {
    if ($text.Contains($bad)) { throw "The installer contains '$bad'. Not publishing." }
}

$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$sumFile = "$setup.sha256"
Set-Content -Path $sumFile -Value "$hash  tekken-studio-$version-setup.exe" -Encoding ascii -NoNewline

gh release view "v$version" --repo "$owner/$repo" *> $null
if ($LASTEXITCODE -eq 0) {
    gh release upload "v$version" $setup $sumFile $portable --clobber --repo "$owner/$repo"
} else {
    if (-not $Notes) { $Notes = "Tekken Studio v$version beta. Expect rough edges, and please report what breaks. Open the tool and press Update now, or install this over the old version. The portable zip is the tool on its own: unzip it anywhere and run tekken-studio.exe." }
    gh release create "v$version" $setup $sumFile $portable --repo "$owner/$repo" --title "Tekken Studio v$version beta" --notes $Notes
}
if ($LASTEXITCODE -ne 0) { throw "GitHub didn't take the release (see above)." }
Write-Host "Published v$version to https://github.com/$owner/$repo/releases/tag/v$version"
