$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$version = (Get-Content -LiteralPath (Join-Path $project 'extension\manifest.json') -Raw | ConvertFrom-Json).version
$zipPath = Join-Path $project "dist\Notion-KEX-$version.zip"
$expected = @('Install.cmd','Uninstall.cmd','Status.cmd','checksums.json','README.md')
foreach ($folder in @('extension','native','tools','docs')) {
  foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $project $folder) -File)) { $expected += "$folder/$($file.Name)" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
  $entries = @($archive.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
  if ($entries.Count -ne $expected.Count -or @($entries.FullName | Sort-Object -Unique).Count -ne $expected.Count) { throw 'Unexpected portable package inventory.' }
  foreach ($entry in $entries) {
    if ($entry.FullName -cnotin $expected) { throw "Unexpected ZIP entry: $($entry.FullName)" }
    $stream = $entry.Open()
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') } finally { $stream.Dispose(); $sha.Dispose() }
    if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $project $entry.FullName)).Hash) { throw "ZIP content mismatch: $($entry.FullName)" }
  }
} finally { $archive.Dispose() }
$extract = Join-Path $project ('.scratch\portable-test-' + [guid]::NewGuid().ToString('N'))
[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $extract)
& (Join-Path $PSScriptRoot 'installer.ps1') -PackageRoot $extract
Write-Host "PASS: exact $($expected.Count)-file source-only ZIP inventory, byte comparisons, and extracted-package installation."
