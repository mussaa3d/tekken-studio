# Publishes the current installer as a GitHub release of the tekken8mvm repo.
# Usage:  powershell -File tools\publish_release.ps1 [-Notes "What changed"]
# Needs the GitHub CLI signed in (gh auth login). The tool offers the newest release as an update.
param([string]$Notes = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$repo = "tekken8mvm"
$owner = (gh api user --jq .login)
$version = ((Get-Content "$root\installer\tekken8mvm.iss" | Select-String '#define AppVersion "(.+)"').Matches[0].Groups[1].Value)
$setup = "$root\release\tekken8mvm-$version-setup.exe"
if (-not (Test-Path $setup)) { throw "No installer at $setup. Run installer\build_installer.cmd first." }

# Nothing private goes out: no signing key, token or source in the installer.
$bytes = [IO.File]::ReadAllBytes($setup)
$text = [Text.Encoding]::ASCII.GetString($bytes)
foreach ($bad in "BEGIN EC PRIVATE KEY", "BEGIN PRIVATE KEY", "signing_key") {
    if ($text.Contains($bad)) { throw "The installer contains '$bad'. Not publishing." }
}

$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$sumFile = "$setup.sha256"
Set-Content -Path $sumFile -Value "$hash  tekken8mvm-$version-setup.exe" -Encoding ascii -NoNewline

if (-not $Notes) { $Notes = "tekken8mvm v$version beta (early access). Expect rough edges, and please report what breaks. Open the tool and press Update now, or install this over the old version." }
gh release create "v$version" $setup $sumFile --repo "$owner/$repo" --title "tekken8mvm v$version beta (early access)" --notes $Notes
if ($LASTEXITCODE -ne 0) { throw "GitHub didn't take the release (see above)." }
Write-Host "Published v$version to https://github.com/$owner/$repo/releases/tag/v$version"
