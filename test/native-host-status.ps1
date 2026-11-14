[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
  [string]$Configuration = 'Release',
  [string]$BuildDirectory,
  [string]$EvidencePath,
  [string]$BuildEvidencePath,
  [switch]$KeepFixtures,
  [switch]$SkipCliSmoke
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sourceDir = Join-Path $repoRoot 'native-host'
$testDir = Join-Path $repoRoot 'test'
$testResultsDir = Join-Path $repoRoot 'test-results'

if (-not $BuildDirectory) {
  $BuildDirectory = Join-Path $sourceDir 'build'
} elseif (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
  $BuildDirectory = Join-Path $repoRoot $BuildDirectory
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)

if (-not $EvidencePath) {
  $EvidencePath = Join-Path $testResultsDir 'native-host-status.json'
} elseif (-not [IO.Path]::IsPathRooted($EvidencePath)) {
  $EvidencePath = Join-Path $repoRoot $EvidencePath
}
$EvidencePath = [IO.Path]::GetFullPath($EvidencePath)

if (-not $BuildEvidencePath) {
  $BuildEvidencePath = Join-Path $testResultsDir 'native-host-status-build.json'
} elseif (-not [IO.Path]::IsPathRooted($BuildEvidencePath)) {
  $BuildEvidencePath = Join-Path $repoRoot $BuildEvidencePath
}
$BuildEvidencePath = [IO.Path]::GetFullPath($BuildEvidencePath)

New-Item -ItemType Directory -Force -Path $testResultsDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $EvidencePath) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $BuildEvidencePath) | Out-Null

$commands = New-Object System.Collections.Generic.List[string]
$cliChecks = New-Object System.Collections.Generic.List[object]
$results = [ordered]@{
  configure = $null
  build = $null
  tests = $null
  cliSmoke = $null
}

function Add-Command {
  param([string]$Text)
  $commands.Add($Text)
}

function Invoke-CliJson {
  param(
    [string]$CliPath,
    [string[]]$Arguments
  )

  $previousEncoding = [Console]::OutputEncoding
  try {
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $text = (& $CliPath @Arguments | Out-String)
    $exitCode = $LASTEXITCODE
  } finally {
    [Console]::OutputEncoding = $previousEncoding
  }

  if ([string]::IsNullOrWhiteSpace($text)) {
    throw "kex-status produced no output (exit $exitCode)."
  }
  $parsed = $text | ConvertFrom-Json
  return [pscustomobject]@{
    ExitCode = $exitCode
    Json = $parsed
    Text = $text
  }
}

function Invoke-CliSmoke {
  param([string]$CliPath)

  $smokeRoot = Join-Path ([IO.Path]::GetTempPath()) ('notion-kex-cli-smoke-' + [guid]::NewGuid().ToString('N'))
  try {
    $notionName = 'N' + [char]0x00F6 + 'tion ' + [char]0x5E94 + [char]0x7528 + ' with spaces'
    $dataName = 'N' + [char]0x00F6 + 'tion ' + [char]0x6570 + [char]0x636E + ' with spaces'
    $notionDir = Join-Path $smokeRoot (Join-Path $notionName 'Notion')
    $asar = Join-Path $notionDir 'resources\app.asar'
    $recordRoot = Join-Path $smokeRoot (Join-Path $dataName 'Notion KEX')
    $notionExe = Join-Path $notionDir 'Notion.exe'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $asar) | Out-Null
    New-Item -ItemType Directory -Force -Path $recordRoot | Out-Null

    $original = [Text.Encoding]::UTF8.GetBytes("original cli archive`n")
    $patched = [Text.Encoding]::UTF8.GetBytes("patched cli archive`n")
    [IO.File]::WriteAllBytes($asar, $patched)
    [IO.File]::WriteAllBytes($notionExe, [byte[]]@())

    $backup = Join-Path $recordRoot ('app-asar-before-native-' + ('a' * 32) + '.bak')
    [IO.File]::WriteAllBytes($backup, $original)

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
      $originalHash = ([BitConverter]::ToString($sha.ComputeHash($original)) -replace '-', '').ToLowerInvariant()
      $patchedHash = ([BitConverter]::ToString($sha.ComputeHash($patched)) -replace '-', '').ToLowerInvariant()
    } finally {
      $sha.Dispose()
    }

    $recordPath = Join-Path $recordRoot 'native-zoom.json'
    $record = [ordered]@{
      asarPath = $asar
      backupPath = $backup
      originalHash = $originalHash
      patchedHash = $patchedHash
      payloadHash = ('b' * 64)
      installedAt = '2026-11-09T00:00:00Z'
    }
    [IO.File]::WriteAllText($recordPath, ($record | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    $recordBytes = [IO.File]::ReadAllBytes($recordPath)

    Add-Command ('"{0}" --asar "{1}" --record-root "{2}" --json' -f $CliPath, $asar, $recordRoot)
    $installed = Invoke-CliJson -CliPath $CliPath -Arguments @('--asar', $asar, '--record-root', $recordRoot, '--json')
    if ($installed.ExitCode -ne 0) {
      throw "CLI installed smoke returned exit $($installed.ExitCode)."
    }
    if ($installed.Json.status -ne 'installed' -or -not $installed.Json.installed) {
      throw "CLI installed smoke returned status '$($installed.Json.status)'."
    }
    $cliChecks.Add([ordered]@{
        name = 'installed via --asar'
        exitCode = $installed.ExitCode
        status = $installed.Json.status
        passed = $true
      })

    Add-Command ('"{0}" --notion-exe "{1}" --record-root "{2}" --json' -f $CliPath, $notionExe, $recordRoot)
    $derived = Invoke-CliJson -CliPath $CliPath -Arguments @('--notion-exe', $notionExe, '--record-root', $recordRoot, '--json')
    if ($derived.ExitCode -ne 0 -or $derived.Json.status -ne 'installed') {
      throw "CLI --notion-exe derivation smoke failed (exit $($derived.ExitCode), status '$($derived.Json.status)')."
    }
    $cliChecks.Add([ordered]@{
        name = 'installed via --notion-exe'
        exitCode = $derived.ExitCode
        status = $derived.Json.status
        passed = $true
      })

    if ([Convert]::ToBase64String($recordBytes) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes($recordPath))) {
      throw 'CLI changed the committed record during read-only inspection.'
    }

    Remove-Item -LiteralPath $recordPath -Force
    Add-Command ('"{0}" --asar "{1}" --record-root "{2}" --json' -f $CliPath, $asar, $recordRoot)
    $notInstalled = Invoke-CliJson -CliPath $CliPath -Arguments @('--asar', $asar, '--record-root', $recordRoot, '--json')
    if ($notInstalled.ExitCode -ne 1 -or $notInstalled.Json.status -ne 'not-installed') {
      throw "CLI not-installed smoke failed (exit $($notInstalled.ExitCode), status '$($notInstalled.Json.status)')."
    }
    $cliChecks.Add([ordered]@{
        name = 'not-installed exit code'
        exitCode = $notInstalled.ExitCode
        status = $notInstalled.Json.status
        passed = $true
      })

    Add-Command ('"{0}" --json' -f $CliPath)
    $usageStdout = Join-Path $smokeRoot 'usage-stdout.txt'
    $usageStderr = Join-Path $smokeRoot 'usage-stderr.txt'
    $usageProcess = Start-Process -FilePath $CliPath -ArgumentList @('--json') -Wait -PassThru -NoNewWindow -RedirectStandardOutput $usageStdout -RedirectStandardError $usageStderr
    $usageExit = $usageProcess.ExitCode
    if ($usageExit -ne 2) {
      throw "CLI usage smoke returned exit $usageExit instead of 2."
    }
    $cliChecks.Add([ordered]@{
        name = 'usage error exit code'
        exitCode = $usageExit
        status = 'usage-error'
        passed = $true
      })

    Add-Command ('"{0}" --help' -f $CliPath)
    & $CliPath --help | Out-Null
    $helpExit = $LASTEXITCODE
    if ($helpExit -ne 0) {
      throw "CLI help returned exit $helpExit."
    }
    $cliChecks.Add([ordered]@{
        name = 'help exit code'
        exitCode = $helpExit
        status = 'help'
        passed = $true
      })
  } finally {
    Remove-Item -LiteralPath $smokeRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}

$failure = $null
$cmakeVersion = $null
$vsInstall = $null

try {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 Build Tools with the C++ workload.'
  }
  $vsInstall = @(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
  if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    throw 'Visual Studio 2022 with the C++ x64 toolchain was not found.'
  }
  $vsInstall = $vsInstall.Trim()

  $cmake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  if (-not (Test-Path -LiteralPath $cmake)) {
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
  }
  $cmakeVersion = (& $cmake --version | Select-Object -First 1)

  $configureArgs = @(
    '-S', $sourceDir,
    '-B', $BuildDirectory,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    '-DBUILD_TESTING=ON'
  )
  Add-Command ('"{0}" {1}' -f $cmake, ($configureArgs -join ' '))
  & $cmake @configureArgs
  $results.configure = $LASTEXITCODE
  if ($results.configure -ne 0) {
    throw "CMake configure failed with exit code $($results.configure)."
  }

  $buildArgs = @('--build', $BuildDirectory, '--config', $Configuration, '--parallel')
  Add-Command ('"{0}" {1}' -f $cmake, ($buildArgs -join ' '))
  & $cmake @buildArgs
  $results.build = $LASTEXITCODE
  if ($results.build -ne 0) {
    throw "CMake build failed with exit code $($results.build)."
  }

  $testExe = Join-Path $BuildDirectory (Join-Path $Configuration 'kex-status-tests.exe')
  if (-not (Test-Path -LiteralPath $testExe)) {
    throw "Test executable was not produced: $testExe"
  }
  $testArgs = @('--evidence', $EvidencePath)
  if ($KeepFixtures) {
    $testArgs += '--keep'
  }
  Add-Command ('"{0}" {1}' -f $testExe, ($testArgs -join ' '))
  & $testExe @testArgs
  $results.tests = $LASTEXITCODE
  if ($results.tests -ne 0) {
    throw "Fixture tests failed with exit code $($results.tests)."
  }

  if (-not $SkipCliSmoke) {
    $cliExe = Join-Path $BuildDirectory (Join-Path $Configuration 'kex-status.exe')
    if (-not (Test-Path -LiteralPath $cliExe)) {
      throw "CLI executable was not produced: $cliExe"
    }
    Invoke-CliSmoke -CliPath $cliExe
    $results.cliSmoke = 0
  } else {
    $results.cliSmoke = 'skipped'
  }
} catch {
  $failure = $_
  if ($null -eq $results.configure) { $results.configure = -1 }
  if ($null -eq $results.build) { $results.build = -1 }
  if ($null -eq $results.tests) { $results.tests = -1 }
  if ($null -eq $results.cliSmoke) { $results.cliSmoke = -1 }
} finally {
  $sourceHashes = [ordered]@{}
  $sourceFiles = @(Get-ChildItem -LiteralPath $sourceDir -Recurse -File | Where-Object {
      $_.FullName -notmatch '\\build\\'
    })
  $extraFiles = @(
    (Join-Path $testDir 'native-host-status.cpp'),
    (Join-Path $testDir 'native-host-status.ps1')
  )
  foreach ($file in @($sourceFiles) + @($extraFiles)) {
    $path = if ($file -is [IO.FileInfo]) { $file.FullName } else { [string]$file }
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $relative = $path.Substring($repoRoot.Length).TrimStart('\', '/').Replace('\', '/')
    $sourceHashes[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
  }

  $buildEvidence = [ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    component = 'kex-status'
    version = '0.1.0'
    cmake = $cmakeVersion
    generator = 'Visual Studio 17 2022'
    visualStudio = $vsInstall
    configuration = $Configuration
    buildDirectory = $BuildDirectory
    evidence = $EvidencePath
    commands = $commands
    results = $results
    cliChecks = $cliChecks
    sourceHashes = $sourceHashes
  }
  $buildJson = $buildEvidence | ConvertTo-Json -Depth 12
  [IO.File]::WriteAllText($BuildEvidencePath, $buildJson + "`n", [Text.UTF8Encoding]::new($false))
}

if ($null -ne $failure) {
  Write-Host ''
  Write-Host "native-host status verification failed: $($failure.Exception.Message)" -ForegroundColor Red
  Write-Host "Build evidence: $BuildEvidencePath"
  exit 1
}

Write-Host ''
Write-Host "native-host status verification passed."
Write-Host "Fixture evidence: $EvidencePath"
Write-Host "Build evidence:   $BuildEvidencePath"
exit 0
