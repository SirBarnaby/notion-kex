$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path $PSScriptRoot -Parent
$manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'extension\manifest.json') -Raw | ConvertFrom-Json
$hashes = [ordered]@{}
Get-ChildItem -LiteralPath (Join-Path $packageRoot 'extension') -File | Sort-Object Name | ForEach-Object { $hashes['extension/' + $_.Name] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
foreach ($name in @('native/pinch.js','native/pinch-main.js','tools/Asar.ps1','tools/NativeZoom.ps1','tools/Setup.ps1')) {
  $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $packageRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$checksums = Join-Path $packageRoot 'checksums.json'
[IO.File]::WriteAllText($checksums, ($hashes | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
$distRoot = Join-Path $packageRoot 'dist'
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
$zip = Join-Path $distRoot "Notion-KEX-$($manifest.version).zip"
$items = @('extension','native','tools','docs','Install.cmd','Uninstall.cmd','Status.cmd','checksums.json','README.md') | ForEach-Object { Join-Path $packageRoot $_ }
Compress-Archive -LiteralPath $items -DestinationPath $zip -Force
Write-Host "Built $zip"
Write-Host ('SHA256: ' + (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
