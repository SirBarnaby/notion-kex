param([string]$PackageRoot = (Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath($PackageRoot)
$fixture = Join-Path (Split-Path $PSScriptRoot -Parent) ('.scratch\installer-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $fixture 'app\resources') -Force | Out-Null
[IO.File]::SetAttributes($fixture, ([IO.File]::GetAttributes($fixture) -bor [IO.FileAttributes]::Hidden))
$profile = Join-Path $fixture 'profile'
New-Item -ItemType Directory -Path $profile -Force | Out-Null
$exe = Join-Path $fixture 'app\Notion.exe'
$asar = Join-Path $fixture 'app\resources\app.asar'
& node (Join-Path $PSScriptRoot 'installer-fixture.mjs') (Join-Path $fixture 'app') 1
if ($LASTEXITCODE) { throw 'Fixture creation failed.' }
$state = Join-Path $profile 'state.json'
[IO.File]::WriteAllText($state, '{"appState":{"preferences":{"disabledExtensions":["React Developer Tools"],"isDebugMenuEnabled":false}},"history":{"fixture":"retain"}}')
$original = (Get-FileHash -LiteralPath $asar).Hash
& (Join-Path $project 'tools\Setup.ps1') -Action Install -NotionData $profile -NotionExe $exe -NoLaunch
$installed = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
if (-not $installed.appState.preferences.isChromeExtensionsEnabled -or $installed.history.fixture -ne 'retain' -or $installed.appState.preferences.disabledExtensions[0] -ne 'React Developer Tools') { throw 'Installation lost unrelated state.' }
if ((Get-FileHash -LiteralPath $asar).Hash -eq $original) { throw 'Native pinch was not installed.' }
$patched = (Get-FileHash -LiteralPath $asar).Hash
& (Join-Path $project 'tools\Setup.ps1') -Action Install -NotionData $profile -NotionExe $exe -NoLaunch
if ((Get-FileHash -LiteralPath $asar).Hash -ne $patched) { throw 'Repeat installation changed the archive.' }
& node (Join-Path $PSScriptRoot 'installer-fixture.mjs') (Join-Path $fixture 'app') 2
$updatedOriginal = (Get-FileHash -LiteralPath $asar).Hash
if (-not (Test-Path -LiteralPath (Join-Path $profile 'extensions\Notion KEX\manifest.json'))) { throw 'Application replacement removed KEX.' }
& (Join-Path $project 'tools\Setup.ps1') -Action Install -NotionData $profile -NotionExe $exe -NoLaunch
& (Join-Path $project 'tools\Setup.ps1') -Action Uninstall -NotionData $profile -NotionExe $exe -NoLaunch
if ((Get-FileHash -LiteralPath $asar).Hash -ne $updatedOriginal) { throw 'Uninstall did not restore the current app version.' }
$restored = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
if ($restored.appState.preferences.isDebugMenuEnabled -ne $false -or $restored.appState.preferences.PSObject.Properties['isChromeExtensionsEnabled'] -or $restored.history.fixture -ne 'retain') { throw 'Uninstall did not restore original preferences.' }
& node (Join-Path $PSScriptRoot 'installer-fixture.mjs') (Join-Path $fixture 'app') 3 1
$before = (Get-FileHash -LiteralPath $state).Hash
try { & (Join-Path $project 'tools\Setup.ps1') -Action Install -NotionData $profile -NotionExe $exe -NoLaunch; throw 'Expected fuse rejection.' } catch { if ($_.Exception.Message -notlike '*enforces embedded ASAR integrity*') { throw } }
if ((Get-FileHash -LiteralPath $state).Hash -ne $before -or (Test-Path -LiteralPath (Join-Path $profile 'extensions\Notion KEX'))) { throw 'Unsupported-app preflight changed profile files.' }
Write-Host 'PASS: native install, idempotent reinstall, application update reapply, exact current-version restoration, preserved preferences, and integrity-fuse refusal before changes.'
