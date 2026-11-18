# Builds Tekken Studio for TTT2: BlueSpark's own game, linked once more with the studio's code, as a separate program.
#
#   powershell -ExecutionPolicy Bypass -File build-studio.ps1 -Playtest C:\ttt2
#
# It needs a BlueSpark play test that has been installed and built once with Start Here (so the compiler and the game's object
# files are there). Nothing of BlueSpark's is changed: its ttt2.exe, its scripts and its build folder are only read. The studio's
# program goes in rex\out\build\tekken-studio\ttt2-studio.exe, beside BlueSpark's build folder.
#
# The studio's code names things inside BlueSpark's prebuilt library, which move from release to release, so each studio build is
# for one BlueSpark release (-Release, checked against rex\package-info.txt).
param(
    [Parameter(Mandatory)][string]$Playtest,
    [string]$Sources = '',
    [int]$Release = 23
)
$ErrorActionPreference = 'Stop'
if (-not $Sources) { $Sources = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'src' }

$rex = Join-Path $Playtest 'rex'
$build = Join-Path $rex 'out\build\win-amd64-release'
$ninja = Join-Path $build 'build.ninja'
foreach ($need in (Join-Path $rex 'package-info.txt'), $ninja, (Join-Path $build 'ttt2.exe'), (Join-Path $build 'CMakeCache.txt')) {
    if (-not (Test-Path -LiteralPath $need)) { throw "This doesn't look like a built BlueSpark play test: $need is missing. Open its Start Here and run Install first." }
}
$info = Get-Content -LiteralPath (Join-Path $rex 'package-info.txt')
$patch = [int](($info | Where-Object { $_ -match '^patch=(\d+)' }) -replace '^patch=', '')
if ($patch -ne $Release) { throw "This studio is made for BlueSpark release $Release, and this play test is release $patch. Update Tekken Studio (or BlueSpark) so the two match." }

# The compiler BlueSpark's Install downloaded, found through the build's own cache.
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
$clang = (($cache | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:' }) -replace '^[^=]*=', '').Trim()
if (-not (Test-Path -LiteralPath $clang)) { throw "The compiler isn't where the build left it ($clang). Run Start Here's Install again." }
$toolchain = Split-Path (Split-Path (Split-Path $clang))
$msvc = Get-ChildItem -LiteralPath $toolchain -Directory -Filter 'msvc-*' | Select-Object -First 1
if (-not $msvc) { throw "Microsoft's C++ runtime and Windows SDK aren't in $toolchain." }
$env:INCLUDE = (@('crt\include', 'sdk\include\ucrt', 'sdk\include\um', 'sdk\include\shared', 'sdk\include\winrt', 'sdk\include\cppwinrt') | ForEach-Object { Join-Path $msvc.FullName $_ }) -join ';'
$env:LIB = (@('crt\lib\x64', 'sdk\lib\ucrt\x64', 'sdk\lib\um\x64') | ForEach-Object { Join-Path $msvc.FullName $_ }) -join ';'
$env:VCToolsInstallDir = (Join-Path $msvc.FullName 'crt') + '\'
$msvcVersion = (Get-Content -LiteralPath (Join-Path $build 'ttt2-msvc-version.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
if ($msvcVersion) { $env:CCC_OVERRIDE_OPTIONS = "# ^-fms-compatibility-version=$($msvcVersion.Trim())" }
$env:PATH = (Split-Path $clang) + ';' + $env:PATH

# What BlueSpark's own link uses: the game's object files, then the libraries.
$lines = Get-Content -LiteralPath $ninja
$at = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l.StartsWith('build ttt2.exe:') })
if ($at -lt 0) { throw "build.ninja has no link step for ttt2.exe (another BlueSpark layout?)." }
$objects = @(($lines[$at] -split '\s+') | Where-Object { $_ -like '*.obj' } | ForEach-Object { Join-Path $build ($_ -replace '/', '\') })
$libraries = ''
for ($i = $at + 1; $i -lt $at + 12; $i++) { if ($lines[$i] -match '^\s+LINK_LIBRARIES = (.*)$') { $libraries = $Matches[1]; break } }
if ($objects.Count -lt 10 -or -not $libraries) { throw "Couldn't read the game's link step from build.ninja ($($objects.Count) object files)." }
foreach ($o in $objects) { if (-not (Test-Path -LiteralPath $o)) { throw "A game object file is missing ($o). Run Start Here's Install again." } }

$out = Join-Path $rex 'out\build\tekken-studio'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$sdk = Join-Path $rex 'prebuilt\sdk'
$compile = @('-DNOMINMAX', '-DREX_HAS_D3D12=1', '-DSPDLOG_COMPILED_LIB', '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN',
    "-I$Sources", "-I$rex", "-I$(Join-Path $rex 'generated\default')", '-isystem', (Join-Path $sdk 'include'), '-isystem', (Join-Path $sdk 'include\dxc'),
    '-O2', '-DNDEBUG', '-std=gnu++23', '-D_DLL', '-D_MT', '-Xclang', '--dependent-lib=msvcrt', '-msse4.1', '-fasync-exceptions', '-Wno-everything')
$mine = @()
foreach ($source in Get-ChildItem -LiteralPath $Sources -Filter '*.cpp') {
    $target = Join-Path $obj ($source.BaseName + '.obj')
    $mine += $target
    $headers = Get-ChildItem -LiteralPath $Sources -Filter '*.h' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $newest = if ($headers -and $headers.LastWriteTime -gt $source.LastWriteTime) { $headers.LastWriteTime } else { $source.LastWriteTime }
    if ((Test-Path -LiteralPath $target) -and (Get-Item -LiteralPath $target).LastWriteTime -gt $newest) { continue }
    Write-Host "Compiling $($source.Name)"
    & $clang @compile '-c' $source.FullName '-o' $target
    if ($LASTEXITCODE -ne 0) { throw "Compiling $($source.Name) failed." }
}

Write-Host "Linking ttt2-studio.exe ($($objects.Count) game object files, $($mine.Count) of the studio's)"
$rsp = Join-Path $obj 'link.rsp'
$quoted = @($objects + $mine | ForEach-Object { '"' + ($_ -replace '\\', '/') + '"' })
Set-Content -LiteralPath $rsp -Value (($quoted -join ' ') + ' ' + $libraries) -Encoding ascii
$exe = Join-Path $out 'ttt2-studio.exe'
Push-Location $out
try {
    & $clang '-nostartfiles' '-nostdlib' '-O3' '-DNDEBUG' '-D_DLL' '-D_MT' '-Xclang' '--dependent-lib=msvcrt' '-Xlinker' '/subsystem:windows' `
        '-Xlinker' '/MAP:ttt2-studio.map' '-fuse-ld=lld-link' "@$rsp" '-o' $exe '-Xlinker' '/MANIFEST:EMBED' '-Xlinker' '/version:0.0'
    if ($LASTEXITCODE -ne 0) { throw 'Linking failed.' }
} finally { Pop-Location }
foreach ($dll in 'rexruntime.dll', 'rexgpu-xenos.dll') { Copy-Item -LiteralPath (Join-Path $sdk "bin\$dll") -Destination $out -Force }
Set-Content -LiteralPath (Join-Path $out 'studio-release.txt') -Value "$Release" -Encoding ascii
Write-Host "Built $exe"
