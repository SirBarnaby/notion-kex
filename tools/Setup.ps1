[CmdletBinding()]
param(
  [ValidateSet('Install','Uninstall','Status')][string]$Action = 'Status',
  [string]$NotionData = (Join-Path $env:APPDATA 'Notion'),
  [string]$NotionExe = (Join-Path $env:LOCALAPPDATA 'Programs\Notion\Notion.exe'),
  [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path $PSScriptRoot -Parent
$installRoot = Join-Path $NotionData 'extensions\Notion KEX'
$recordRoot = Join-Path $NotionData 'Notion KEX'
$recordPath = Join-Path $recordRoot 'install.json'
$statePath = Join-Path $NotionData 'state.json'
$markerName = '.notion-kex-owned'

function Write-JsonAtomic($Path, $Object) {
  $tempPath = "$Path.notion-kex.tmp"
  [IO.File]::WriteAllText($tempPath, ($Object | ConvertTo-Json -Depth 100), [Text.UTF8Encoding]::new($false))
  Move-Item -LiteralPath $tempPath -Destination $Path -Force
}
function Set-Property($Object, $Name, $Value) { $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $Value -Force }
function Assert-Closed {
  $targetExe = [IO.Path]::GetFullPath($NotionExe)
  $running = Get-Process -Name Notion -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or [IO.Path]::GetFullPath($_.Path) -eq $targetExe }
  if ($running) { throw 'Quit Notion completely from its tray menu, then run this command again. Running settings will not be overwritten.' }
}
function Assert-OwnedDirectory {
  $resolvedData = [IO.Path]::GetFullPath($NotionData).TrimEnd('\') + '\'
  $resolvedInstall = [IO.Path]::GetFullPath($installRoot)
  if (-not $resolvedInstall.StartsWith($resolvedData, [StringComparison]::OrdinalIgnoreCase) -or -not $resolvedInstall.EndsWith('\extensions\Notion KEX', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected installation path.' }
  if ((Test-Path -LiteralPath $installRoot) -and -not (Test-Path -LiteralPath (Join-Path $installRoot $markerName))) { throw 'The target contains an unrecognized extension. It will not be overwritten.' }
  if ((Test-Path -LiteralPath $installRoot) -and ((Get-Item -LiteralPath $installRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Extension directory must not be a symbolic link or junction.' }
  foreach ($ancestor in @($NotionData, (Join-Path $NotionData 'extensions'), $recordRoot)) {
    if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Installation ancestors must not be symbolic links or junctions.' }
  }
}
function Get-NotionState {
  if (-not (Test-Path -LiteralPath $statePath)) { throw 'Start Notion once, quit it completely, and run the installer again.' }
  $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
  if (-not $state.appState -or -not $state.appState.preferences) { throw 'This Notion settings format is unsupported. No changes made.' }
  return $state
}

if ($Action -eq 'Status') {
  $present = Test-Path -LiteralPath (Join-Path $installRoot 'manifest.json')
  Write-Host 'Notion KEX'
  Write-Host "Extension installed: $present"
  Write-Host "Location: $installRoot"
  $problems = 0
  if (-not $present) { $problems++ }
  if (Test-Path -LiteralPath $NotionExe) { Write-Host ('Notion version: ' + (Get-Item -LiteralPath $NotionExe).VersionInfo.ProductVersion) }
  if (Test-Path -LiteralPath $recordPath) {
    $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    foreach ($file in $record.files.PSObject.Properties) {
      $target = Join-Path $installRoot $file.Name
      if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.Value) { Write-Warning "File missing or changed: $($file.Name)"; $problems++ }
    }
    Write-Host ('Installed package: ' + $record.version)
  }
  try {
    $native = & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Status -NotionData $NotionData -NotionExe $NotionExe
    Write-Host $native.message
    if (-not $native.installed) { $problems++ }
  } catch { Write-Warning $_.Exception.Message; $problems++ }
  Write-Host 'Runtime check: View -> Extensions -> Notion KEX. A stored extension alone does not prove the native permission is enabled.'
  if ($problems) { exit 1 } else { exit 0 }
}

Assert-Closed
Assert-OwnedDirectory
if ($Action -eq 'Install') {
  if (-not (Test-Path -LiteralPath $NotionExe)) { throw 'Notion was not found. Install the official Windows desktop app first, or pass -NotionExe.' }
  $asarPath = Join-Path (Split-Path $NotionExe) 'resources\app.asar'
  if (-not (Test-Path -LiteralPath $asarPath)) { throw 'The installed Notion package is unsupported.' }
  # Match the extension-loader contract instead of a brittle bundle offset or exact version.
  $bundle = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($asarPath))
  foreach ($term in @('setupExtensions','isChromeExtensionsEnabled','isChromeExtensionsAllowed','loadExtension')) {
    if (-not $bundle.Contains($term)) { throw "Notion's native extension loader changed ($term). No changes made; update Notion KEX first." }
  }
  $state = Get-NotionState
  $manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'extension\manifest.json') -Raw | ConvertFrom-Json
  if ($manifest.name -ne 'Notion KEX') { throw 'Unexpected package identity.' }
  $hashes = Get-Content -LiteralPath (Join-Path $packageRoot 'checksums.json') -Raw | ConvertFrom-Json
  $expectedFiles = @('extension/kex.css','extension/kex.js','extension/manifest.json','extension/policy.js','extension/settings.css','extension/settings.html','extension/settings.js','native/pinch.js','native/pinch-main.js','tools/Asar.ps1','tools/NativeZoom.ps1','tools/Setup.ps1')
  if (@($hashes.PSObject.Properties).Count -ne $expectedFiles.Count) { throw 'Unexpected package inventory.' }
  foreach ($file in $hashes.PSObject.Properties) {
    if ($file.Name -cnotin $expectedFiles -or $file.Value -notmatch '^[a-f0-9]{64}$') { throw 'Unexpected package file path or hash.' }
    if ((Get-FileHash -LiteralPath (Join-Path $packageRoot $file.Name) -Algorithm SHA256).Hash -ne $file.Value) { throw "Package verification failed: $($file.Name)" }
  }
  $native = & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Validate -NotionData $NotionData -NotionExe $NotionExe
  Write-Host $native.message
  New-Item -ItemType Directory -Force -Path $recordRoot | Out-Null
  if (-not (Test-Path -LiteralPath $recordPath)) {
    $backup = Join-Path $recordRoot ('state-before-install-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
    Copy-Item -LiteralPath $statePath -Destination $backup
    $before = @{}
    foreach ($name in @('isChromeExtensionsEnabled','isDebugMenuEnabled')) {
      $prop = $state.appState.preferences.PSObject.Properties[$name]
      $before[$name] = @{ existed = $null -ne $prop; value = if ($prop) { $prop.Value } else { $null } }
    }
    $record = [pscustomobject]@{ version = $manifest.version; installedAt = (Get-Date).ToUniversalTime().ToString('o'); preferencesBefore = [pscustomobject]$before; stateBackup = $backup; files = @{} }
  } else { $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json }
  New-Item -ItemType Directory -Force -Path $installRoot | Out-Null
  $files = @{}
  foreach ($file in ($hashes.PSObject.Properties | Where-Object Name -like 'extension/*')) {
    $name = Split-Path $file.Name -Leaf
    Copy-Item -LiteralPath (Join-Path $packageRoot $file.Name) -Destination (Join-Path $installRoot $name) -Force
    $files[$name] = $file.Value
  }
  [IO.File]::WriteAllText((Join-Path $installRoot $markerName), 'Notion KEX installer owns this folder.')
  Set-Property $record 'files' $files
  Set-Property $record 'version' $manifest.version
  Write-JsonAtomic $recordPath $record
  Set-Property $state.appState.preferences 'isChromeExtensionsEnabled' $true
  Set-Property $state.appState.preferences 'isDebugMenuEnabled' $true
  # Existing disabled extensions are retained; only KEX is enabled.
  if ($state.appState.preferences.PSObject.Properties['disabledExtensions']) {
    $state.appState.preferences.disabledExtensions = @($state.appState.preferences.disabledExtensions | Where-Object { $_ -ne 'Notion KEX' })
  }
  Write-JsonAtomic $statePath $state
  $native = & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Install -NotionData $NotionData -NotionExe $NotionExe
  Write-Host $native.message
  Write-Host "Notion KEX $($manifest.version) installed in $installRoot"
  Write-Host 'One-time native permission: focus the Notion desktop window, press and release Alt, then choose Debug -> Extensions Enabled.'
  Write-Host 'This is the application menu, separate from a page or workspace menu. Then Quit and reopen Notion.'
  Write-Host 'Settings: View -> Extensions -> Notion KEX. Pinch with two fingers to zoom around the pointer.'
  Write-Host 'After a Notion app update, rerun Install.cmd with Notion closed to restore native pinch support.'
  if (-not $NoLaunch) { Start-Process -FilePath $NotionExe -WindowStyle Normal }
} else {
  if (-not (Test-Path -LiteralPath $recordPath)) { throw 'No Notion KEX installation record was found.' }
  $state = Get-NotionState
  $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
  $native = & (Join-Path $PSScriptRoot 'NativeZoom.ps1') -Action Uninstall -NotionData $NotionData -NotionExe $NotionExe
  Write-Host $native.message
  foreach ($property in $record.preferencesBefore.PSObject.Properties) {
    $name = $property.Name
    if ($name -eq 'isChromeExtensionsEnabled' -and (Get-ChildItem -LiteralPath (Join-Path $NotionData 'extensions') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'Notion KEX' })) { continue }
    if ($state.appState.preferences.PSObject.Properties[$name] -and $state.appState.preferences.$name -eq $true) {
      if ($property.Value.existed) { Set-Property $state.appState.preferences $name $property.Value.value }
      else { $state.appState.preferences.PSObject.Properties.Remove($name) }
    }
  }
  Write-JsonAtomic $statePath $state
  if (Test-Path -LiteralPath $installRoot) { Remove-Item -LiteralPath ([IO.Path]::GetFullPath($installRoot)) -Recurse -Force }
  Remove-Item -LiteralPath $recordPath
  Write-Host 'Notion KEX removed. Your notes, other extensions, and secure Notion settings were retained.'
}
