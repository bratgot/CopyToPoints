<#
.SYNOPSIS
  Install CopyToPoints into a Nuke plugin folder (~/.nuke by default).

.DESCRIPTION
  Copies the plugin folder and adds one idempotent line to <prefix>\init.py so
  Nuke looks in it. There are no third-party libraries to place: these nodes use
  the USD that ships inside Nuke itself.

  Two sets of builds are installed. classic\ holds the nodes for Nuke's original
  3D system, usd\ the ones for its USD system; the plugin's own init.py adds
  whichever folders match the Nuke that is running.

.EXAMPLE
  .\install.ps1
  Installs every build in the zip to $env:USERPROFILE\.nuke.

.EXAMPLE
  .\install.ps1 -Versions 17.1
  Installs only the Nuke 17.1 builds.

.EXAMPLE
  .\install.ps1 -Prefix D:\studio\nuke_plugins
#>
param(
    [string]$Prefix = (Join-Path $env:USERPROFILE ".nuke"),
    [string[]]$Versions = @()
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "CopyToPoints"
if (-not (Test-Path $src)) { throw "CopyToPoints folder not found next to this script - unpack the whole zip, not just install.ps1" }

# ---- which builds ----------------------------------------------------------
$all = @()
foreach ($kind in @("classic", "usd")) {
    $kd = Join-Path $src $kind
    if (Test-Path $kd) {
        $all += Get-ChildItem $kd -Directory | Where-Object { $_.Name -match '^nuke\d+\.\d+$' } | ForEach-Object { $_.Name }
    }
}
$all = $all | Sort-Object -Unique
if ($Versions.Count -gt 0) {
    $want = $Versions | ForEach-Object { if ($_ -match '^nuke') { $_ } else { "nuke$_" } }
    $missing = $want | Where-Object { $all -notcontains $_ }
    if ($missing) { throw "no build in this zip for: $($missing -join ', ') (it has: $($all -join ', '))" }
    $sel = $want
} else {
    $sel = $all
}
if (-not $sel) { throw "no builds found in $src" }

$dest = Join-Path $Prefix "CopyToPoints"
Write-Host "Installing CopyToPoints to $dest"
Write-Host "  builds: $($sel -join ', ')"

# ---- the shared parts ------------------------------------------------------
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($f in @("init.py", "menu.py", "LICENSE", "THIRD_PARTY_NOTICES.md", "README.md")) {
    $p = Join-Path $src $f
    if (Test-Path $p) { Copy-Item $p $dest -Force }
}
$icons = Join-Path $src "icons"
if (Test-Path $icons) {
    New-Item -ItemType Directory -Force (Join-Path $dest "icons") | Out-Null
    Copy-Item (Join-Path $icons "*") (Join-Path $dest "icons") -Force
}

# ---- the builds ------------------------------------------------------------
foreach ($kind in @("classic", "usd")) {
    foreach ($v in $sel) {
        $from = Join-Path $src "$kind\$v"
        if (-not (Test-Path $from)) { continue }
        $to = Join-Path $dest "$kind\$v"
        New-Item -ItemType Directory -Force $to | Out-Null
        Copy-Item (Join-Path $from "*") $to -Force
        $names = (Get-ChildItem $to -Filter *.dll | ForEach-Object { $_.BaseName }) -join ', '
        Write-Host ("  {0,-8} {1,-9} {2}" -f $kind, $v, $names)
    }
}

# ---- register the folder with Nuke -----------------------------------------
# init.py rather than menu.py, so terminal (-t) sessions get it as well.
$initPy = Join-Path $Prefix "init.py"
$begin  = "# --- CopyToPoints (auto-added by install.ps1) ---"
$block  = "$begin`r`nimport nuke`r`nnuke.pluginAddPath('./CopyToPoints')`r`n# --- end CopyToPoints ---`r`n"
$cur    = if (Test-Path $initPy) { Get-Content $initPy -Raw } else { "" }
if ($cur -match [regex]::Escape("nuke.pluginAddPath('./CopyToPoints')")) {
    Write-Host "  $initPy already registers the folder"
} else {
    if ($cur -and -not $cur.EndsWith("`n")) { $cur += "`r`n" }
    # Written without a BOM: Set-Content -Encoding UTF8 adds one on Windows
    # PowerShell, and a user's init.py is nicer left as plain text.
    [System.IO.File]::WriteAllText($initPy, $cur + $block, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "  registered the folder in $initPy"
}

# ---- did we cover the Nuke that is actually here? --------------------------
$found = @()
foreach ($pf in @($env:ProgramFiles, "${env:ProgramFiles(x86)}")) {
    if ($pf -and (Test-Path $pf)) {
        $found += Get-ChildItem $pf -Directory -Filter "Nuke*" -ErrorAction SilentlyContinue |
                  ForEach-Object { if ($_.Name -match '^Nuke(\d+\.\d+)v\d+') { $Matches[1] } }
    }
}
$found = $found | Sort-Object -Unique
Write-Host ""
if ($found) {
    Write-Host "Nuke versions found on this machine: $($found -join ', ')"
    $un = $found | Where-Object { $sel -notcontains "nuke$_" }
    if ($un) { Write-Host "  NOTE: nothing was installed for $($un -join ', ') - see COMPATIBILITY.md." -ForegroundColor Yellow }
} else {
    Write-Host "No Nuke installation was found in Program Files - only a warning; it may be elsewhere."
}
Write-Host ""
Write-Host "Done. Start Nuke: CopyToPoints and MultiplyCf are on the 3D toolbar,"
Write-Host "CopyToPointsUSD, ParticlesToUSD and VolumeToUSD on the USD/3D toolbar."
