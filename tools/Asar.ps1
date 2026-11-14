function Get-KexByteHash([byte[]]$Bytes) {
  $sha = [Security.Cryptography.SHA256]::Create()
  try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
  finally { $sha.Dispose() }
}

function Get-KexAsar([string]$Path) {
  if ((Get-Item -LiteralPath $Path).Length -gt 512MB) { throw 'ASAR exceeds the supported package size.' }
  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 16 -or [BitConverter]::ToUInt32($bytes, 0) -ne 4) { throw 'Unsupported ASAR header.' }
  $headerSize = [BitConverter]::ToUInt32($bytes, 4)
  $jsonSize = [BitConverter]::ToUInt32($bytes, 12)
  if ($headerSize -lt 8 -or $headerSize -gt 16MB -or $jsonSize -gt $headerSize - 8 -or $headerSize - $jsonSize - 8 -gt 3 -or 8L + $headerSize -gt $bytes.Length) { throw 'Invalid ASAR bounds.' }
  $header = [Text.UTF8Encoding]::new($false, $true).GetString($bytes, 16, $jsonSize) | ConvertFrom-Json
  $entries = [Collections.Generic.List[object]]::new()
  function Add-KexEntries($Directory, [string]$Prefix, [int]$Depth) {
    if ($Depth -gt 64 -or -not $Directory.files) { throw 'Unsupported ASAR directory tree.' }
    foreach ($property in $Directory.files.PSObject.Properties) {
      if ($property.Name -match '[/\\]' -or $property.Name -in @('.', '..')) { throw 'Invalid ASAR member name.' }
      $memberPath = if ($Prefix) { "$Prefix/$($property.Name)" } else { $property.Name }
      $member = $property.Value
      if ($member.files) { Add-KexEntries $member $memberPath ($Depth + 1) }
      elseif (-not $member.unpacked -and -not $member.link) {
        $offset = 0L
        if (-not [long]::TryParse([string]$member.offset, [ref]$offset) -or $offset -lt 0 -or $member.size -lt 0 -or 8L + $headerSize + $offset + [long]$member.size -gt $bytes.Length) { throw "Invalid ASAR member bounds: $memberPath" }
        $entries.Add([pscustomobject]@{ Path = $memberPath; Offset = $offset; Size = [int]$member.size; Header = $member })
      }
    }
  }
  Add-KexEntries $header '' 0
  $end = 0L
  foreach ($entry in ($entries | Sort-Object Offset)) {
    if ($entry.Size -eq 0) { continue }
    if ($entry.Offset -lt $end) { throw 'Overlapping ASAR members are unsupported.' }
    $end = $entry.Offset + $entry.Size
  }
  return [pscustomobject]@{ Bytes = $bytes; Header = $header; BodyOffset = 8L + $headerSize; Entries = $entries }
}

function Get-KexAsarMember($Archive, [string]$MemberPath) {
  $entry = @($Archive.Entries | Where-Object Path -eq $MemberPath)
  if ($entry.Count -ne 1) { throw "Required packed ASAR member is unavailable: $MemberPath" }
  $data = [byte[]]::new($entry[0].Size)
  [Array]::Copy($Archive.Bytes, $Archive.BodyOffset + $entry[0].Offset, $data, 0L, $data.Length)
  if ($entry[0].Header.integrity) {
    if ($entry[0].Header.integrity.algorithm -ne 'SHA256' -or (Get-KexByteHash $data) -ne $entry[0].Header.integrity.hash) { throw "ASAR member integrity mismatch: $MemberPath" }
  }
  return ,$data
}

function Set-KexAsarMember($Archive, [string]$MemberPath, [byte[]]$Replacement) {
  $entry = @($Archive.Entries | Where-Object Path -eq $MemberPath)
  if ($entry.Count -ne 1) { throw 'Unexpected ASAR replacement target.' }
  $entry = $entry[0]
  $delta = $Replacement.Length - $entry.Size
  foreach ($other in $Archive.Entries) {
    if ($other.Offset -gt $entry.Offset) { $other.Header.offset = [string]($other.Offset + $delta) }
  }
  $entry.Header.size = $Replacement.Length
  if ($entry.Header.integrity) {
    $blockSize = [int]$entry.Header.integrity.blockSize
    if ($blockSize -lt 1 -or $blockSize -gt 16MB) { throw 'Unsupported ASAR integrity block size.' }
    $blocks = @()
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
      for ($offset = 0; $offset -lt $Replacement.Length; $offset += $blockSize) {
        $hash = $sha.ComputeHash($Replacement, $offset, [Math]::Min($blockSize, $Replacement.Length - $offset))
        $blocks += ([BitConverter]::ToString($hash)).Replace('-', '').ToLowerInvariant()
      }
    } finally { $sha.Dispose() }
    $entry.Header.integrity.hash = Get-KexByteHash $Replacement
    $entry.Header.integrity.blocks = @($blocks)
  }
  $json = [Text.Encoding]::UTF8.GetBytes(($Archive.Header | ConvertTo-Json -Depth 100 -Compress))
  $paddedSize = [int]([Math]::Ceiling($json.Length / 4.0) * 4)
  $stream = [IO.MemoryStream]::new()
  try {
    foreach ($value in @([uint32]4, [uint32](8 + $paddedSize), [uint32](4 + $paddedSize), [uint32]$json.Length)) {
      $word = [BitConverter]::GetBytes($value); $stream.Write($word, 0, 4)
    }
    $stream.Write($json, 0, $json.Length)
    for ($padding = $json.Length; $padding -lt $paddedSize; $padding++) { $stream.WriteByte(0) }
    $stream.Write($Archive.Bytes, $Archive.BodyOffset, $entry.Offset)
    $stream.Write($Replacement, 0, $Replacement.Length)
    $tail = $Archive.BodyOffset + $entry.Offset + $entry.Size
    $stream.Write($Archive.Bytes, $tail, $Archive.Bytes.Length - $tail)
    return ,$stream.ToArray()
  } finally { $stream.Dispose() }
}

function Assert-KexAsarEdits($Before, $After, [string[]]$MemberPaths) {
  if ($Before.Entries.Count -ne $After.Entries.Count) { throw 'ASAR inventory changed.' }
  foreach ($entry in $Before.Entries) {
    if ($entry.Path -in $MemberPaths) { continue }
    $first = Get-KexAsarMember $Before $entry.Path
    $second = Get-KexAsarMember $After $entry.Path
    if ((Get-KexByteHash $first) -ne (Get-KexByteHash $second)) { throw "Unrelated ASAR member changed: $($entry.Path)" }
  }
}
