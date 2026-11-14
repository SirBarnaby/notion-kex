[CmdletBinding()]
param(
  [string]$NotionExe = (Join-Path $env:LOCALAPPDATA 'Programs\Notion\Notion.exe'),
  [string]$NotionData = (Join-Path $env:APPDATA 'Notion'),
  [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
try {
  if (-not (Test-Path -LiteralPath (Join-Path $NotionData 'Notion KEX\install.json'))) {
    if (-not $NoLaunch) { Start-Process -FilePath $NotionExe -WindowStyle Normal }
    return
  }
  $running = @(Get-Process Notion -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path -eq $NotionExe })
  $native = & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Status -NotionExe $NotionExe -NotionData $NotionData
  if (-not $native.installed) {
    if ($running.Count) { throw 'Quit Notion from its tray menu, then open Notion again. Its update replaced the zoom patch.' }
    & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Install -NotionExe $NotionExe -NotionData $NotionData | Out-Null
    $native = & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Status -NotionExe $NotionExe -NotionData $NotionData
    if (-not $native.installed) { throw 'Zoom repair verification failed.' }
  }
  if (-not $NoLaunch) { Start-Process -FilePath $NotionExe -WindowStyle Normal }
} catch {
  if ($NoLaunch) { throw }
  Add-Type -AssemblyName System.Windows.Forms
  [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Notion KEX') | Out-Null
  exit 1
}
