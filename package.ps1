<#
.SYNOPSIS
  Build the distributable zip: dist\CopyToPoints-<version>-Nuke14.1-win64.zip

  Layout of the zip:
    CopyToPoints-<version>-Nuke14.1-win64/
      INSTALL.md, install.bat, install.ps1
      CopyToPoints/  (DLLs, init.py, menu.py, README.md, VERSION.txt, examples/)

.EXAMPLE
  .\build.ps1            # build the DLLs first
  .\package.ps1          # -> dist\CopyToPoints-1.0.0-Nuke14.1-win64.zip
  .\package.ps1 -Version 1.1.0
#>
param(
    [string]$Version = "1.9.1",
    [string]$NukeTag = "Nuke14-17-win64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$name = "CopyToPoints-$Version-$NukeTag"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
$plug = Join-Path $stage "CopyToPoints"

foreach ($dll in @("CopyToPoints.dll", "MultiplyCf.dll")) {
    if (-not (Test-Path (Join-Path $root "build\Release\$dll"))) { throw "build\Release\$dll missing - run .\build.ps1 first" }
}

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $plug "examples") | Out-Null

New-Item -ItemType Directory -Force (Join-Path $plug "classic/nuke14") | Out-Null
Copy-Item (Join-Path $root "build\Release\CopyToPoints.dll") (Join-Path $plug "classic/nuke14")
Copy-Item (Join-Path $root "build\Release\MultiplyCf.dll")   (Join-Path $plug "classic/nuke14")
foreach ($m in @(14, 15, 16, 17)) {
    $dll = Join-Path $root "build-usd$m\Release\CopyToPointsUSD.dll"
    if (Test-Path $dll) {
        New-Item -ItemType Directory -Force (Join-Path $plug "usd/nuke$m") | Out-Null
        Copy-Item $dll (Join-Path $plug "usd/nuke$m")
    } else { Write-Host "note: no USD build for Nuke $m (build-usd$m missing) - package will not include it" }
}
Copy-Item (Join-Path $root "nuke\init.py") $plug
Copy-Item (Join-Path $root "nuke\menu.py") $plug
Copy-Item (Join-Path $root "README.md")    $plug
Copy-Item (Join-Path $root "examples\CopyToPoints_example.nk") (Join-Path $plug "examples")
if (Test-Path (Join-Path $root "LICENSE")) { Copy-Item (Join-Path $root "LICENSE") $plug }
$stamp = Get-Date -Format "yyyy-MM-dd"
Set-Content -Path (Join-Path $plug "VERSION.txt") -Value "CopyToPoints $Version ($NukeTag), built $stamp`r`nclassic/nuke14: CopyToPoints, MultiplyCf (Nuke 14.1)`r`nusd/nuke14|15|16|17: CopyToPointsUSD (Nuke 14.1 new-3D preview / 15.2 / 16.0 / 17.0)`r`n"

Copy-Item (Join-Path $root "dist-src\INSTALL.md")  $stage
Copy-Item (Join-Path $root "dist-src\install.ps1") $stage
Set-Content -Path (Join-Path $stage "install.bat") -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0install.ps1`" %*`r`npause`r`n"

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Package: $zip"
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } | ForEach-Object { Write-Host ("  " + $_.FullName.Substring($stage.Length + 1) + "  (" + $_.Length + " bytes)") }
