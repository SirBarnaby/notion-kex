[CmdletBinding()]
param(
  [ValidateSet('Install','Uninstall','Status','Validate')][string]$Action = 'Status',
  [string]$NotionData = (Join-Path $env:APPDATA 'Notion'),
  [string]$NotionExe = (Join-Path $env:LOCALAPPDATA 'Programs\Notion\Notion.exe')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Asar.ps1')
$packageRoot = Split-Path $PSScriptRoot -Parent
$asarPath = [IO.Path]::GetFullPath((Join-Path (Split-Path $NotionExe) 'resources\app.asar'))
$recordRoot = [IO.Path]::GetFullPath((Join-Path $NotionData 'Notion KEX'))
$recordPath = Join-Path $recordRoot 'native-zoom.json'
$pendingPath = Join-Path $recordRoot 'native-zoom-pending.json'
$members = [ordered]@{
  '.webpack/renderer/tab_browser_view/preload.js' = @{ payload = 'native/pinch.js'; marker = '/* Notion KEX native pinch v1 */'; contracts = @('contextBridge', '__electronApi', '__isElectron') }
  '.webpack/main/index.js' = @{ payload = 'native/pinch-main.js'; marker = '/* Notion KEX native pinch reset v1 */'; contracts = @('require("electron")') }
}

function Assert-KexRegularPath([string]$Path) {
  $current = [IO.Path]::GetFullPath($Path)
  while ($current) {
    if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Native zoom paths must not contain symbolic links or junctions.' }
    $parent = Split-Path $current -Parent
    if ($parent -eq $current) { break }
    $current = $parent
  }
}
function Assert-KexNativeClosed {
  $target = [IO.Path]::GetFullPath($NotionExe)
  $running = Get-Process -Name Notion -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or [IO.Path]::GetFullPath($_.Path) -eq $target }
  if ($running) { throw 'Quit Notion completely from its tray menu before changing its native pinch support.' }
}
function Replace-KexFile([string]$Source, [string]$Destination) {
  [IO.File]::Replace($Source, $Destination, [NullString]::Value)
}
function Write-KexNativeJson([string]$Path, $Object) {
  $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
  try {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($Object | ConvertTo-Json -Depth 20))
    $file = [IO.FileStream]::new($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
    if (Test-Path -LiteralPath $Path) { Replace-KexFile $temporary $Path }
    else { Move-Item -LiteralPath $temporary -Destination $Path }
  } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}
function Assert-KexNativeFuse {
  $sentinel = 'dL7pKGdnNz796PbbjQWNKmHXBZaB9tsX'
  $stream = [IO.File]::OpenRead($NotionExe)
  try {
    $buffer = [byte[]]::new(262144)
    $carry = ''
    while (($count = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
      $text = $carry + [Text.Encoding]::ASCII.GetString($buffer, 0, $count)
      $index = $text.IndexOf($sentinel, [StringComparison]::Ordinal)
      if ($index -ge 0) {
        $position = $stream.Position - $count - $carry.Length + $index + $sentinel.Length
        $stream.Position = $position
        if ($stream.ReadByte() -ne 1 -or $stream.ReadByte() -lt 6) { throw 'Unsupported Electron fuse wire.' }
        $stream.Position = $position + 2 + 4
        if ($stream.ReadByte() -ne 48) { throw 'This app enforces embedded ASAR integrity. Native zoom will not weaken or bypass it.' }
        return
      }
      $carry = $text.Substring([Math]::Max(0, $text.Length - 48))
    }
    throw 'Electron fuse state could not be verified.'
  } finally { $stream.Dispose() }
}
function Assert-KexNativeRecord($Value) {
  if ($Value.asarPath -ne $asarPath) { throw 'The native zoom record belongs to a different Notion installation.' }
  foreach ($name in @('originalHash','patchedHash','payloadHash')) { if ($Value.$name -notmatch '^[a-f0-9]{64}$') { throw 'Invalid native zoom record hash.' } }
  $backup = [IO.Path]::GetFullPath($Value.backupPath)
  if (-not $backup.StartsWith($recordRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or (Split-Path $backup -Leaf) -notmatch '^app-asar-before-native-[a-f0-9]{32}\.bak$') { throw 'Unexpected native zoom backup path.' }
  Assert-KexRegularPath $backup
  if (-not (Test-Path -LiteralPath $backup) -or (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $Value.originalHash) { throw 'The original native zoom backup is missing or changed.' }
}
function Read-KexNativeJson([string]$Path) {
  Assert-KexRegularPath $Path
  if ((Get-Item -LiteralPath $Path).Length -gt 1MB) { throw 'Native zoom metadata exceeds the supported size.' }
  return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}
function Get-KexNativeRecord {
  if (-not (Test-Path -LiteralPath $recordPath)) { return $null }
  $record = Read-KexNativeJson $recordPath
  Assert-KexNativeRecord $record
  return $record
}
function Recover-KexNativeTransaction {
  if (-not (Test-Path -LiteralPath $pendingPath)) { return }
  $pending = Read-KexNativeJson $pendingPath
  if ($pending.asarPath -ne $asarPath -or $pending.previousHash -notmatch '^[a-f0-9]{64}$' -or $pending.nextHash -notmatch '^[a-f0-9]{64}$' -or $pending.newRecord.patchedHash -ne $pending.nextHash) { throw 'Invalid pending native zoom transaction.' }
  Assert-KexNativeRecord $pending.newRecord
  if ($pending.previousRecord) { Assert-KexNativeRecord $pending.previousRecord }
  $rollbackPath = [IO.Path]::GetFullPath($pending.rollbackPath)
  if (-not $rollbackPath.StartsWith($recordRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or (Split-Path $rollbackPath -Leaf) -notmatch '^native-rollback-[a-f0-9]{32}\.bak$') { throw 'Unexpected native rollback path.' }
  Assert-KexRegularPath $rollbackPath
  if (-not (Test-Path -LiteralPath $rollbackPath) -or (Get-FileHash -LiteralPath $rollbackPath).Hash -ne $pending.previousHash) { throw 'The native rollback backup is missing or changed.' }
  $actual = (Get-FileHash -LiteralPath $asarPath).Hash
  if ($actual -eq $pending.nextHash) { Write-KexNativeJson $recordPath $pending.newRecord }
  elseif ($actual -eq $pending.previousHash) {
    if ($pending.previousRecord) { Write-KexNativeJson $recordPath $pending.previousRecord }
    elseif (Test-Path -LiteralPath $recordPath) { Remove-Item -LiteralPath $recordPath }
  } else { throw 'The app changed during an interrupted native install. Its files and recovery backups were retained.' }
  Remove-Item -LiteralPath $pendingPath
  Remove-Item -LiteralPath $rollbackPath
}

Assert-KexRegularPath $asarPath
Assert-KexRegularPath $recordRoot
$mutexName = 'Local\NotionKexNativeZoom-' + (Get-KexByteHash ([Text.Encoding]::UTF8.GetBytes($asarPath.ToLowerInvariant())))
$mutex = [Threading.Mutex]::new($false, $mutexName)
$locked = $false
try {
  try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
  if (-not $locked) { throw 'Another native zoom installation is running.' }
Recover-KexNativeTransaction
$record = Get-KexNativeRecord
$currentHash = if (Test-Path -LiteralPath $asarPath) { (Get-FileHash -LiteralPath $asarPath -Algorithm SHA256).Hash } else { $null }
if ($Action -eq 'Status') {
  $installed = $record -and $currentHash -eq $record.patchedHash
  return [pscustomobject]@{ installed = [bool]$installed; message = if ($installed) { 'Native touchpad zoom: installed (100%-500%).' } elseif ($record) { 'Native touchpad zoom: app changed; rerun Install.cmd with Notion closed.' } else { 'Native touchpad zoom: not installed.' } }
}
Assert-KexNativeClosed
if ($Action -eq 'Uninstall') {
  if (-not $record) { return [pscustomobject]@{ message = 'Native touchpad zoom: no installation record.' } }
  if ($currentHash -eq $record.patchedHash) {
    $temp = "$asarPath.notion-kex-$([guid]::NewGuid().ToString('N')).tmp"
    try {
      Copy-Item -LiteralPath $record.backupPath -Destination $temp
      Assert-KexNativeClosed
      if ((Get-FileHash -LiteralPath $asarPath).Hash -ne $currentHash) { throw 'The app changed while native zoom removal was being prepared.' }
      Replace-KexFile $temp $asarPath
      if ((Get-FileHash -LiteralPath $asarPath).Hash -ne $record.originalHash) { throw 'Native zoom restoration verification failed.' }
    } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp } }
    $message = 'Native touchpad zoom removed; the original app archive is restored.'
  } else {
    if ($currentHash) {
      $archive = Get-KexAsar $asarPath
      foreach ($memberPath in $members.Keys) {
        if ([Text.Encoding]::UTF8.GetString((Get-KexAsarMember $archive $memberPath)).Contains($members[$memberPath].marker)) { throw 'The app was edited after native zoom installation. It will not be overwritten; the backup and record are retained.' }
      }
    }
    $message = 'A newer or replaced app archive was retained; native zoom backup remains available.'
  }
  Remove-Item -LiteralPath $recordPath
  return [pscustomobject]@{ message = $message }
}

Assert-KexNativeFuse
$hashes = Get-Content -LiteralPath (Join-Path $packageRoot 'checksums.json') -Raw | ConvertFrom-Json
if ($hashes.'tools/Asar.ps1' -ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Asar.ps1')).Hash) { throw 'Native zoom package verification failed.' }
$payloadHashes = @()
foreach ($memberPath in $members.Keys) {
  $payloadName = $members[$memberPath].payload
  $payload = [IO.File]::ReadAllBytes((Join-Path $packageRoot $payloadName))
  $hash = Get-KexByteHash $payload
  if ($hashes.PSObject.Properties[$payloadName].Value -ne $hash) { throw 'Native zoom package verification failed.' }
  $members[$memberPath].bytes = $payload
  $payloadHashes += $hash
}
$payloadHash = Get-KexByteHash ([Text.Encoding]::UTF8.GetBytes(($payloadHashes -join "`n")))
if ($record -and $currentHash -eq $record.patchedHash -and $payloadHash -eq $record.payloadHash) { return [pscustomobject]@{ message = 'Native touchpad zoom already matches this package.' } }
$basePath = if ($record -and $currentHash -eq $record.patchedHash) { $record.backupPath } else { $asarPath }
$archive = Get-KexAsar $basePath
foreach ($memberPath in $members.Keys) {
  $originalMember = Get-KexAsarMember $archive $memberPath
  $source = [Text.UTF8Encoding]::new($false, $true).GetString($originalMember)
  if ($source.Contains($members[$memberPath].marker)) { throw 'An unrecorded native zoom patch exists. It will not be overwritten.' }
  foreach ($contract in $members[$memberPath].contracts) {
    if (-not $source.Contains($contract)) { throw "Notion's native entry changed ($memberPath, $contract). No native changes made." }
  }
  $payload = $members[$memberPath].bytes
  $replacement = [byte[]]::new($originalMember.Length + 1 + $payload.Length)
  [Array]::Copy($originalMember, 0, $replacement, 0, $originalMember.Length)
  $replacement[$originalMember.Length] = 10
  [Array]::Copy($payload, 0, $replacement, $originalMember.Length + 1, $payload.Length)
  $members[$memberPath].replacement = $replacement
}
if ($Action -eq 'Validate') { return [pscustomobject]@{ message = 'Native touchpad zoom is supported by this app archive.' } }
$temp = "$asarPath.notion-kex-$([guid]::NewGuid().ToString('N')).tmp"
try {
  $after = $archive
  foreach ($memberPath in $members.Keys) {
    $candidate = Set-KexAsarMember $after $memberPath $members[$memberPath].replacement
    [IO.File]::WriteAllBytes($temp, $candidate)
    $after = Get-KexAsar $temp
  }
  Assert-KexAsarEdits $archive $after @($members.Keys)
  foreach ($memberPath in $members.Keys) {
    if ((Get-KexByteHash (Get-KexAsarMember $after $memberPath)) -ne (Get-KexByteHash $members[$memberPath].replacement)) { throw 'Native entry verification failed.' }
  }
  New-Item -ItemType Directory -Force -Path $recordRoot | Out-Null
  $backupPath = if ($basePath -eq $asarPath) { Join-Path $recordRoot ('app-asar-before-native-' + [guid]::NewGuid().ToString('N') + '.bak') } else { $basePath }
  if ($basePath -eq $asarPath) { Copy-Item -LiteralPath $asarPath -Destination $backupPath }
  $newRecord = [ordered]@{ asarPath = $asarPath; backupPath = $backupPath; originalHash = (Get-FileHash -LiteralPath $backupPath).Hash; patchedHash = (Get-FileHash -LiteralPath $temp).Hash; payloadHash = $payloadHash; installedAt = (Get-Date).ToUniversalTime().ToString('o') }
  if ($newRecord.originalHash -ne (Get-KexByteHash $archive.Bytes)) { throw 'The original app changed while its backup was being created.' }
  $rollback = [IO.File]::ReadAllBytes($asarPath)
  $rollbackPath = Join-Path $recordRoot ('native-rollback-' + [guid]::NewGuid().ToString('N') + '.bak')
  [IO.File]::WriteAllBytes($rollbackPath, $rollback)
  Write-KexNativeJson $pendingPath ([ordered]@{ asarPath = $asarPath; previousHash = $currentHash; nextHash = $newRecord.patchedHash; rollbackPath = $rollbackPath; previousRecord = $record; newRecord = $newRecord })
  try {
    if ((Get-FileHash -LiteralPath $asarPath).Hash -ne $currentHash) { throw 'The app changed while native zoom was being prepared.' }
    Assert-KexNativeClosed
    Replace-KexFile $temp $asarPath
    Write-KexNativeJson $recordPath $newRecord
  } catch {
    if ((Get-FileHash -LiteralPath $asarPath).Hash -eq $newRecord.patchedHash) { [IO.File]::WriteAllBytes($temp, $rollback); Replace-KexFile $temp $asarPath }
    Recover-KexNativeTransaction
    throw
  }
  Recover-KexNativeTransaction
  return [pscustomobject]@{ message = 'Native Chromium touchpad zoom installed with a verified original archive backup.' }
} finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp } }
} finally { if ($locked) { $mutex.ReleaseMutex() }; $mutex.Dispose() }
