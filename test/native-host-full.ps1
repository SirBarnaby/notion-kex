[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
  [string]$Configuration = 'Release',
  [string]$BuildDirectory,
  [string]$EvidencePath,
  [string]$BuildEvidencePath,
  [switch]$KeepFixtures,
  [switch]$SkipBuild
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
  $EvidencePath = Join-Path $testResultsDir 'native-host-full.json'
} elseif (-not [IO.Path]::IsPathRooted($EvidencePath)) {
  $EvidencePath = Join-Path $repoRoot $EvidencePath
}
$EvidencePath = [IO.Path]::GetFullPath($EvidencePath)

if (-not $BuildEvidencePath) {
  $BuildEvidencePath = Join-Path $testResultsDir 'native-host-full-build.json'
} elseif (-not [IO.Path]::IsPathRooted($BuildEvidencePath)) {
  $BuildEvidencePath = Join-Path $repoRoot $BuildEvidencePath
}
$BuildEvidencePath = [IO.Path]::GetFullPath($BuildEvidencePath)

New-Item -ItemType Directory -Force -Path $testResultsDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $EvidencePath) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $BuildEvidencePath) | Out-Null

$commands = New-Object System.Collections.Generic.List[string]
$results = [ordered]@{
  configure = $null
  build = $null
  tests = $null
}

function Add-Command {
  param([string]$Text)
  $commands.Add($Text)
}

function Get-RelativePath {
  param([string]$Path)
  return $Path.Substring($repoRoot.Length).TrimStart('\', '/').Replace('\', '/')
}

function Get-FileHashMap {
  param([string[]]$RelativePaths, [string[]]$Roots)

  $hashes = [ordered]@{}
  foreach ($relative in $RelativePaths) {
    $path = Join-Path $repoRoot $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) {
      $hashes[$relative.Replace('\', '/')] =
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  foreach ($root in $Roots) {
    $path = Join-Path $repoRoot $root
    if (-not (Test-Path -LiteralPath $path)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File -Force) {
      $hashes[(Get-RelativePath $file.FullName)] =
        (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  return $hashes
}

function Get-ProtectedHashes {
  return Get-FileHashMap -RelativePaths @(
    'checksums.json',
    'native/pinch-main.js',
    'native/pinch.js',
    'tools/NativeZoom.ps1',
    'tools/Setup.ps1'
  ) -Roots @('dist')
}

function Test-HashMapsEqual {
  param($Left, $Right)
  $leftJson = $Left | ConvertTo-Json -Compress
  $rightJson = $Right | ConvertTo-Json -Compress
  return $leftJson -eq $rightJson
}

function Get-SourceHashes {
  $sourceFiles = @(Get-ChildItem -LiteralPath $sourceDir -Recurse -File -Force |
      Where-Object { $_.FullName -notmatch '\\build\\' })
  $hashes = [ordered]@{}
  foreach ($file in $sourceFiles) {
    $hashes[(Get-RelativePath $file.FullName)] =
      (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  foreach ($relative in @(
      'extension/kex.css',
      'extension/kex.js',
      'extension/manifest.json',
      'extension/policy.js',
      'extension/settings.css',
      'extension/settings.html',
      'extension/settings.js',
      'native/pinch.js',
      'native/pinch-main.js',
      'test/native-host-full.cpp',
      'test/native-host-full.ps1'
    )) {
    $path = Join-Path $repoRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $hashes[$relative] =
      (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  return $hashes
}

$failure = $null
$cmakeVersion = $null
$vsInstall = $null
$protectedBefore = Get-ProtectedHashes

try {
  if (-not $SkipBuild) {
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

    $buildArgs = @(
      '--build', $BuildDirectory,
      '--config', $Configuration,
      '--target', 'notion-kex', 'kex-native-tests', 'kex-status-tests',
      '--parallel'
    )
    Add-Command ('"{0}" {1}' -f $cmake, ($buildArgs -join ' '))
    & $cmake @buildArgs
    $results.build = $LASTEXITCODE
    if ($results.build -ne 0) {
      throw "CMake build failed with exit code $($results.build)."
    }
  } else {
    $results.configure = 'skipped'
    $results.build = 'skipped'
  }

  $testExe = Join-Path $BuildDirectory (Join-Path $Configuration 'kex-native-tests.exe')
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
    throw "Native host fixture tests failed with exit code $($results.tests)."
  }
} catch {
  $failure = $_
  if ($null -eq $results.configure) { $results.configure = -1 }
  if ($null -eq $results.build) { $results.build = -1 }
  if ($null -eq $results.tests) { $results.tests = -1 }
} finally {
  $protectedAfter = Get-ProtectedHashes
  $protectedUnchanged = Test-HashMapsEqual -Left $protectedBefore -Right $protectedAfter

  $fixtureEvidence = $null
  if (Test-Path -LiteralPath $EvidencePath -PathType Leaf) {
    try {
      $fixtureEvidence = Get-Content -LiteralPath $EvidencePath -Raw | ConvertFrom-Json
    } catch {
      $fixtureEvidence = $null
    }
  }

  $productExe = Join-Path $BuildDirectory (Join-Path $Configuration 'Notion KEX.exe')
  $testExe = Join-Path $BuildDirectory (Join-Path $Configuration 'kex-native-tests.exe')
  $artifacts = [ordered]@{}
  foreach ($path in @($productExe, $testExe)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $item = Get-Item -LiteralPath $path
    $artifacts[(Get-RelativePath $item.FullName)] = [ordered]@{
      size = $item.Length
      sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }

  $testSummary = $null
  $limitations = @()
  if ($null -ne $fixtureEvidence) {
    $testSummary = $fixtureEvidence.summary
    $limitations = @($fixtureEvidence.limitations)
  }

  $buildEvidence = [ordered]@{
    generatedAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    component = 'kex-native'
    version = '1.0.0'
    cmake = $cmakeVersion
    generator = 'Visual Studio 17 2022'
    visualStudio = $vsInstall
    configuration = $Configuration
    buildDirectory = $BuildDirectory
    evidence = $EvidencePath
    commands = $commands
    results = $results
    testSummary = $testSummary
    artifacts = $artifacts
    protectedFiles = [ordered]@{
      unchanged = $protectedUnchanged
      before = $protectedBefore
      after = $protectedAfter
    }
    sourceHashes = Get-SourceHashes
    limitations = $limitations
  }
  $buildJson = $buildEvidence | ConvertTo-Json -Depth 12
  [IO.File]::WriteAllText($BuildEvidencePath, $buildJson + "`n", [Text.UTF8Encoding]::new($false))

  if (-not $protectedUnchanged -and $null -eq $failure) {
    $failure = 'Protected files changed during verification.'
  }
}

if ($null -ne $failure) {
  $message = if ($failure -is [string]) { $failure } else { $failure.Exception.Message }
  Write-Host ''
  Write-Host "native-host full verification failed: $message" -ForegroundColor Red
  Write-Host "Fixture evidence: $EvidencePath"
  Write-Host "Build evidence:   $BuildEvidencePath"
  exit 1
}

Write-Host ''
Write-Host 'native-host full verification passed.'
if ($null -ne $testSummary) {
  Write-Host ("Fixture tests: {0} passed, {1} failed, {2} skipped." -f `
      $testSummary.passed, $testSummary.failed, $testSummary.skipped)
  if ($testSummary.skipped -gt 0) {
    Write-Host 'Skipped cases are recorded with reasons in the fixture evidence.'
  }
}
Write-Host "Fixture evidence: $EvidencePath"
Write-Host "Build evidence:   $BuildEvidencePath"
exit 0
