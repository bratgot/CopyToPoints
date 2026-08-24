<#
.SYNOPSIS
  Configure + build (+ optionally install) the CopyToPoints Nuke 14.1 plugin.

.EXAMPLE
  .\build.ps1                 # configure if needed, build Release
  .\build.ps1 -Clean          # wipe build/ first
  .\build.ps1 -Install        # also cmake --install into ~/.nuke/CopyToPoints
  .\build.ps1 -NukeDir "C:/Program Files/Nuke14.1v8/cmake"

  If PowerShell refuses to run the script:
    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#>
param(
    [string]$NukeDir = "C:/Program Files/Nuke14.1v8/cmake",
    [string]$Generator = "Visual Studio 16 2019",
    [switch]$Clean,
    [switch]$Install,
    [switch]$Usd,                       # also build CopyToPointsUSD for the Nuke 14/15/16/17 installs found (14.1 = new-3D preview)
    [string[]]$UsdNukeDirs = @("C:/Program Files/Nuke14.1v8", "C:/Program Files/Nuke15.2v9", "C:/Program Files/Nuke16.0v8",
                               "C:/Program Files/Nuke16.1v1", "C:/Program Files/Nuke17.0v4", "C:/Program Files/Nuke17.1v1"),
    [string]$InstallPrefix = (Join-Path $env:USERPROFILE ".nuke")
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"

if ($Clean -and (Test-Path $build)) {
    Write-Host "Removing $build"
    Remove-Item -Recurse -Force $build
}

if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    Write-Host "Configuring ($Generator, Nuke_DIR=$NukeDir)"
    & cmake -G $Generator -A x64 -DNuke_DIR="$NukeDir" -S $root -B $build
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

Write-Host "Building Release"
# Filter MSVC output down to errors so they are not lost in the noise, but
# keep the raw log for inspection.
$log = Join-Path $build "build_log.txt"
& cmake --build $build --config Release 2>&1 | Tee-Object -FilePath $log | ForEach-Object {
    if ($_ -match "error C\d+|error LNK\d+|error :|fatal error|warning C4[0-9]{3}: .*(unre|deprecat)|Build succeeded|FAILED") { $_ }
}
if ($LASTEXITCODE -ne 0) { throw "build failed (see $log)" }

$dll = Join-Path $build "Release\CopyToPoints.dll"
if (Test-Path $dll) { Write-Host "OK: $dll" } else { throw "DLL not produced: $dll" }

if ($Install) {
    Write-Host "Installing to $InstallPrefix"
    & cmake --install $build --config Release --prefix "$InstallPrefix"
    if ($LASTEXITCODE -ne 0) { throw "install failed" }
}

# ---------------------------------------------------------------------------
# USD / new-3D builds (Nuke 15 = VS 2019, Nuke 16/17 = VS 2022), one build dir each
# ---------------------------------------------------------------------------
if ($Usd) {
    foreach ($nk in $UsdNukeDirs) {
        if (-not (Test-Path (Join-Path $nk "cmake/NukeConfig.cmake"))) { Write-Host "skip (not installed): $nk"; continue }
        # One build directory per MINOR version: the NDK is not compatible across
        # them, and sharing a directory silently builds one minor against
        # another's headers - which is how a 17.1 plugin ended up refusing to
        # load in 17.1v1 ("the specified procedure could not be found").
        $mm = [regex]::Match($nk, "Nuke(\d+)\.(\d+)")
        if (-not $mm.Success) { Write-Host "skip (no version in the path): $nk"; continue }
        $major = $mm.Groups[1].Value
        $ver = $major + "." + $mm.Groups[2].Value
        $gen = if ([int]$major -ge 16) { "Visual Studio 17 2022" } else { "Visual Studio 16 2019" }
        $bd = Join-Path $root "build-usd$ver"
        if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
        # An existing build directory is only reused if it was configured against
        # THIS Nuke: a cache left pointing at another install (a beta, say)
        # silently rebuilds nothing and ships a plugin that will not load.
        $cache = Join-Path $bd "CMakeCache.txt"
        if (Test-Path $cache) {
            $cached = (Select-String -Path $cache -Pattern "^Nuke_DIR:" | Select-Object -First 1)
            $want = "$nk/cmake"
            if (-not $cached -or ($cached.Line -split "=", 2)[1] -ne $want) {
                Write-Host "Reconfiguring $bd (was $(if ($cached) { ($cached.Line -split '=', 2)[1] } else { 'unset' }), want $want)"
                Remove-Item -Recurse -Force $bd
            }
        }
        if (-not (Test-Path (Join-Path $bd "CMakeCache.txt"))) {
            Write-Host "Configuring USD build for Nuke $ver ($gen)"
            & cmake -G $gen -A x64 -DNuke_DIR="$nk/cmake" -DBUILD_CLASSIC=OFF -DBUILD_USD=ON -S $root -B $bd
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $nk" }
        }
        & cmake --build $bd --config Release 2>&1 | ForEach-Object { if ($_ -match "error C\d+|error LNK\d+|fatal error|\.dll") { $_ } }
        if ($LASTEXITCODE -ne 0) { throw "USD build failed for $nk" }
        if ($Install) {
            & cmake --install $bd --config Release --prefix "$InstallPrefix"
            if ($LASTEXITCODE -ne 0) { throw "USD install failed for $nk" }
        }
    }
}
