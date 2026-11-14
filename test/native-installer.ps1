$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $project ('.scratch\native-transaction-test-' + [guid]::NewGuid().ToString('N'))
$app = Join-Path $fixture 'app'
$profile = Join-Path $fixture 'profile'
& node (Join-Path $PSScriptRoot 'installer-fixture.mjs') $app 1
if ($LASTEXITCODE) { throw 'Fixture creation failed.' }
$exe = Join-Path $app 'Notion.exe'
$asar = Join-Path $app 'resources\app.asar'
$recordRoot = Join-Path $profile 'Notion KEX'
$recordPath = Join-Path $recordRoot 'native-zoom.json'
$pendingPath = Join-Path $recordRoot 'native-zoom-pending.json'
function Native([string]$Action) { & (Join-Path $project 'tools\NativeZoom.ps1') -Action $Action -NotionData $profile -NotionExe $exe }
function Save-Json([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 20)) }
function Expect-Error([scriptblock]$Run, [string]$Pattern) {
  try { & $Run; throw 'The expected error did not occur.' } catch { if ($_.Exception.Message -notlike $Pattern) { throw } }
}
$original = [IO.File]::ReadAllBytes($asar)
$originalHash = (Get-FileHash -LiteralPath $asar).Hash
Native Install | Out-Null
$record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
$patchedHash = (Get-FileHash -LiteralPath $asar).Hash
function Write-Pending {
  $script:rollbackPath = Join-Path $recordRoot ('native-rollback-' + [guid]::NewGuid().ToString('N') + '.bak')
  [IO.File]::WriteAllBytes($script:rollbackPath, $original)
  Save-Json $pendingPath ([ordered]@{ asarPath = $asar; previousHash = $originalHash; nextHash = $patchedHash; rollbackPath = $script:rollbackPath; previousRecord = $null; newRecord = $record })
}

# The archive swap completed but the metadata write did not.
Remove-Item -LiteralPath $recordPath
Write-Pending
$status = Native Status
if (-not $status.installed -or (Get-FileHash -LiteralPath $asar).Hash -ne $patchedHash -or (Test-Path -LiteralPath $pendingPath) -or (Test-Path -LiteralPath $rollbackPath)) { throw 'Post-swap interruption recovery failed.' }

# The pending transaction exists but the archive swap never happened.
[IO.File]::WriteAllBytes($asar, $original)
Write-Pending
$status = Native Status
if ($status.installed -or (Get-FileHash -LiteralPath $asar).Hash -ne $originalHash -or (Test-Path -LiteralPath $recordPath) -or (Test-Path -LiteralPath $pendingPath)) { throw 'Pre-swap interruption recovery failed.' }

# An updater replaced the archive during interruption: retain all evidence.
Write-Pending
& node (Join-Path $PSScriptRoot 'installer-fixture.mjs') $app 2
$updatedHash = (Get-FileHash -LiteralPath $asar).Hash
Expect-Error { Native Status } '*app changed during an interrupted native install*'
if ((Get-FileHash -LiteralPath $asar).Hash -ne $updatedHash -or -not (Test-Path -LiteralPath $pendingPath) -or -not (Test-Path -LiteralPath $rollbackPath)) { throw 'Conflicting app change was not retained.' }
Remove-Item -LiteralPath $pendingPath
Remove-Item -LiteralPath $rollbackPath

# Missing or edited recovery data must never be used to overwrite an app.
Save-Json $recordPath $record
[IO.File]::WriteAllText($record.backupPath, 'corrupted backup fixture')
Expect-Error { Native Uninstall } '*original native zoom backup is missing or changed*'
if ((Get-FileHash -LiteralPath $asar).Hash -ne $updatedHash) { throw 'An invalid backup changed the app.' }
[IO.File]::WriteAllBytes($record.backupPath, $original)
Native Uninstall | Out-Null
if ((Get-FileHash -LiteralPath $asar).Hash -ne $updatedHash) { throw 'Removal overwrote a newer application.' }
Write-Host 'PASS: interrupted transactions before/after swap, conflicting app preservation, invalid backup refusal, and newer-app removal safety.'
