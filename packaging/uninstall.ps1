<#
.SYNOPSIS
  Remove CopyToPoints from a Nuke plugin folder.
.EXAMPLE
  .\uninstall.ps1
#>
param([string]$Prefix = (Join-Path $env:USERPROFILE ".nuke"))
$ErrorActionPreference = "Stop"
$dest = Join-Path $Prefix "CopyToPoints"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force; Write-Host "removed $dest" }
else { Write-Host "nothing at $dest" }
# Leave <prefix>\init.py alone apart from our own block: it is the user's file
# and may register several plugins.
$initPy = Join-Path $Prefix "init.py"
if (Test-Path $initPy) {
    $cur = Get-Content $initPy -Raw
    $new = [regex]::Replace($cur, "(?ms)^# --- CopyToPoints \(auto-added by [^)]*\) ---.*?^# --- end CopyToPoints ---\r?\n", "")
    if ($new -ne $cur) { [System.IO.File]::WriteAllText($initPy, $new, (New-Object System.Text.UTF8Encoding($false))); Write-Host "removed the registration block from $initPy" }
    else { Write-Host "no registration block in $initPy (left untouched)" }
}
