<#
.SYNOPSIS
  Installs the CopyToPoints plugin package into the current user's ~/.nuke.

  Copies the CopyToPoints\ folder (DLLs, menu.py, init.py, docs, example) to
  %USERPROFILE%\.nuke\CopyToPoints and appends an idempotent
  nuke.pluginAddPath('./CopyToPoints') block to %USERPROFILE%\.nuke\init.py.

.EXAMPLE
  .\install.ps1                      # install for the current user
  .\install.ps1 -NukeHome D:\nukeprefs   # install into another .nuke folder
  .\install.ps1 -Uninstall           # remove the folder and the init.py block

  Double-click install.bat if PowerShell refuses to run scripts.
#>
param(
    [string]$NukeHome = (Join-Path $env:USERPROFILE ".nuke"),
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "CopyToPoints"
$dst  = Join-Path $NukeHome "CopyToPoints"
$init = Join-Path $NukeHome "init.py"
$begin = "# --- CopyToPoints (auto-added by install.ps1) ---"
$end   = "# --- end CopyToPoints ---"

if ($Uninstall) {
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst; Write-Host "Removed $dst" }
    if (Test-Path $init) {
        $txt = Get-Content $init -Raw
        $pattern = [regex]::Escape($begin) + "[\s\S]*?" + [regex]::Escape($end) + "\r?\n?"
        $new = [regex]::Replace($txt, $pattern, "")
        if ($new -ne $txt) { Set-Content -Path $init -Value $new -NoNewline; Write-Host "Removed registration from $init" }
    }
    Write-Host "CopyToPoints uninstalled."
    exit 0
}

if (-not (Test-Path $src)) { throw "Package folder not found: $src (run this script from the unzipped package)" }
if (-not (Test-Path $NukeHome)) { New-Item -ItemType Directory -Force $NukeHome | Out-Null }

# Nuke locks loaded DLLs: warn if a Nuke is running.
if (Get-Process -Name "Nuke*" -ErrorAction SilentlyContinue) {
    Write-Warning "A Nuke process is running. Close Nuke before installing/overwriting the DLLs."
}

if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
Copy-Item -Recurse -Force $src $dst
Write-Host "Copied plugin to $dst"

$block = "$begin`r`nimport nuke`r`nnuke.pluginAddPath('./CopyToPoints')`r`n$end`r`n"
$existing = ""
if (Test-Path $init) { $existing = Get-Content $init -Raw }
if ($existing -and $existing.Contains($begin)) {
    Write-Host "$init already registers the plugin (skipped)"
} else {
    if ($existing -and -not $existing.EndsWith("`n")) { $existing += "`r`n" }
    Set-Content -Path $init -Value ($existing + $block) -NoNewline
    Write-Host "Registered plugin path in $init"
}

Write-Host ""
Write-Host "Done. Restart Nuke: 14.1 -> 3D > Modify > CopyToPoints (+ 3D > Shader > MultiplyCf); 15/16/17 -> 3D > Modify > CopyToPointsUSD."
Write-Host "Example script: $dst\examples\CopyToPoints_example.nk"
