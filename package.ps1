<#
.SYNOPSIS
  Build the distributable zip: dist\CopyToPoints-<version>-Nuke14.1-17.1-win64.zip

.DESCRIPTION
  Stages what a USER needs: both sets of nodes built for each Nuke minor
  version, the loader scripts, icons, the example script, licences and the
  documentation in packaging\.

  No third-party binaries are staged, because none are used - these nodes link
  against the USD inside Nuke itself.

  .pdb files are deliberately not packaged.

.EXAMPLE
  .\build.ps1 -All -Install    # build everything first
  .\package.ps1                # -> dist\CopyToPoints-1.10.0-Nuke14.1-17.1-win64.zip

.EXAMPLE
  .\package.ps1 -Version 1.11.0 -Versions 17.0,17.1
#>
param(
    [string]$Version = "1.10.0",
    [string[]]$Versions = @("14.1", "15.2", "16.0", "16.1", "17.0", "17.1")
)
$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
# Name the zip after what is actually in it. Hard-coding the full range
# would mislabel a trimmed release as covering every version.
$span  = if ($Versions.Count -eq 1) { "Nuke$($Versions[0])" } else { "Nuke$($Versions[0])-$($Versions[-1])" }
$name  = "CopyToPoints-$Version-$span-win64"
$dist  = Join-Path $root "dist"
$stage = Join-Path $dist $name
$plug  = Join-Path $stage "CopyToPoints"

function Need($path, $what) {
    if (-not (Test-Path $path)) { throw "$what not found: $path" }
    return $path
}

# The two sets of nodes, and which build folder each comes out of. Naming them
# here rather than globbing means a missing DLL is an error instead of a zip
# that quietly ships four nodes out of five.
$sets = @(
    @{ kind = "classic"; dir = "build-classic"; dlls = @("CopyToPoints.dll", "MultiplyCf.dll") },
    @{ kind = "usd";     dir = "build-usd";     dlls = @("CopyToPointsUSD.dll", "ParticlesToUSD.dll", "VolumeToUSD.dll") }
)

# ---- check every build is there before deleting anything -------------------
foreach ($s in $sets) {
    foreach ($v in $Versions) {
        foreach ($d in $s.dlls) {
            Need (Join-Path $root "$($s.dir)$v\Release\$d") "$d for Nuke $v (run .\build.ps1 -All first)" | Out-Null
        }
    }
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $plug | Out-Null

# ---- the builds ------------------------------------------------------------
foreach ($s in $sets) {
    foreach ($v in $Versions) {
        $to = Join-Path $plug "$($s.kind)\nuke$v"
        New-Item -ItemType Directory -Force $to | Out-Null
        foreach ($d in $s.dlls) { Copy-Item (Join-Path $root "$($s.dir)$v\Release\$d") $to }
    }
}

# ---- the loader scripts, icons, licence and docs beside the builds ---------
foreach ($f in @("nuke\init.py", "nuke\menu.py", "LICENSE", "THIRD_PARTY_NOTICES.md", "README.md")) {
    Copy-Item (Need (Join-Path $root $f) $f) $plug
}
New-Item -ItemType Directory -Force (Join-Path $plug "icons") | Out-Null
# _contact_sheet.png is a proof sheet used while drawing the icons - not a node
# icon, and no business in a release.
Get-ChildItem (Join-Path $root "nuke\icons") -Filter *.png |
    Where-Object { $_.Name -notlike "_*" } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $plug "icons") }

# ---- the example script ----------------------------------------------------
$ex = Join-Path $root "examples"
if (Test-Path $ex) {
    New-Item -ItemType Directory -Force (Join-Path $stage "examples") | Out-Null
    # *.nk only: an editor backup (.nk~) sits beside it and is not a deliverable
    Get-ChildItem $ex -Filter *.nk | ForEach-Object { Copy-Item $_.FullName (Join-Path $stage "examples") }
}

# ---- the documentation and the installer -----------------------------------
foreach ($f in @("README.md", "INSTALL.md", "COMPATIBILITY.md", "install.ps1", "install.bat", "uninstall.ps1")) {
    Copy-Item (Need (Join-Path $root "packaging\$f") "packaging\$f") $stage
}
Copy-Item (Join-Path $root "LICENSE") $stage
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $stage
Set-Content (Join-Path $stage "VERSION.txt") "CopyToPoints $Version`r`nbuilt $(Get-Date -Format 'yyyy-MM-dd')`r`nNuke $($Versions -join ', ') - Windows x64`r`n" -Encoding UTF8

# ---- nothing that should not ship ------------------------------------------
$bad = Get-ChildItem $stage -Recurse -File |
       Where-Object { $_.Name -like "*.pdb" -or $_.Name -like "*.ilk" -or $_.Name -like "*.exp" -or $_.Name -like "*~" }
if ($bad) { throw "files staged that should not ship: $($bad.Name -join ', ')" }

# ---- zip -------------------------------------------------------------------
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 2)
$files = (Get-ChildItem $stage -Recurse -File).Count
Write-Host ""
Write-Host "$zip"
Write-Host "  $mb MB, $files files, Nuke $($Versions -join ', ')"
