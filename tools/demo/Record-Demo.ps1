# Record-Demo.ps1: records the README clips from the real Notion window, in your own Notion look.
#
#   powershell -ExecutionPolicy Bypass -File tools\demo\Record-Demo.ps1 -Out <folder> [-Launcher <Notion KEX.exe>]
#
# Open a page titled "Notion Demo" in Notion first. The script quits Notion, appends demo.js to the installed
# kex.js for one run, starts Notion through the KEX launcher and records the Notion window while the demo mode
# plays its scenes (margins on and off, then typing). It restores the original kex.js afterwards. The demo mode
# deletes the sample text it typed and restores the saved settings. Keep the Notion window uncovered while it
# records (about 45 seconds): the recording is taken from the screen.
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Out,
  [string]$Launcher = (Join-Path $env:LOCALAPPDATA 'NotionKEX\Notion KEX.exe'),
  [string]$NotionData = (Join-Path $env:APPDATA 'Notion'),
  [int]$Seconds = 45
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class KexDemoWin {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
  public struct RECT { public int L, T, R, B; }
}
'@
[KexDemoWin]::SetProcessDPIAware() | Out-Null

$extension = Join-Path $NotionData 'extensions\Notion KEX\kex.js'
$demo = Join-Path $PSScriptRoot 'demo.js'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Get-NotionWindow {
  Get-Process Notion -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle } | Select-Object -First 1
}

function Stop-Notion {
  $processes = Get-Process Notion -ErrorAction SilentlyContinue
  if (-not $processes) { return }
  $processes | ForEach-Object { [void]$_.CloseMainWindow() }
  Start-Sleep -Seconds 3
  Get-Process Notion -ErrorAction SilentlyContinue | Stop-Process -Force
  for ($i = 0; $i -lt 50 -and (Get-Process Notion -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 200 }
}

$original = [IO.File]::ReadAllBytes($extension)
try {
  Stop-Notion
  $expires = [DateTimeOffset]::UtcNow.AddMinutes(5).ToUnixTimeMilliseconds()
  $demoText = (Get-Content -LiteralPath $demo -Raw).Replace('__KEX_DEMO_EXPIRES__', "$expires")
  $combined = [Text.Encoding]::UTF8.GetString($original).TrimEnd() + "`n" + $demoText
  [IO.File]::WriteAllText($extension, $combined, [Text.UTF8Encoding]::new($false))

  # Started through WMI so Notion does not belong to the calling process tree.
  $null = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "`"$Launcher`"" }

  $window = $null
  for ($i = 0; $i -lt 300 -and -not $window; $i++) { Start-Sleep -Milliseconds 100; $window = Get-NotionWindow }
  if (-not $window) { throw 'The Notion window did not appear.' }
  $rect = New-Object KexDemoWin+RECT
  [void][KexDemoWin]::DwmGetWindowAttribute($window.MainWindowHandle, 9, [ref]$rect, 16)
  $width = ($rect.R - $rect.L); $height = ($rect.B - $rect.T)
  $width -= $width % 2; $height -= $height % 2

  $video = Join-Path $Out 'demo.mkv'
  $ffmpeg = Start-Process ffmpeg -PassThru -NoNewWindow -ArgumentList @(
    '-hide_banner', '-loglevel', 'error', '-y', '-f', 'gdigrab', '-framerate', '30', '-draw_mouse', '0',
    '-offset_x', $rect.L, '-offset_y', $rect.T, '-video_size', "${width}x${height}", '-i', 'desktop',
    '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '12', '-pix_fmt', 'yuv420p', "`"$video`"")
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $log = New-Object Collections.Generic.List[object]
  $last = ''
  while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    $current = (Get-NotionWindow).MainWindowTitle
    if ($current -and $current -ne $last) {
      $log.Add([ordered]@{ t = [math]::Round($clock.Elapsed.TotalSeconds, 2); title = $current }); $last = $current
    }
    Start-Sleep -Milliseconds 50
  }
  Start-Sleep -Seconds 1
  Stop-Process -Id $ffmpeg.Id -Force -ErrorAction SilentlyContinue
  [ordered]@{ rect = @{ x = $rect.L; y = $rect.T; width = $width; height = $height }; titles = $log } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'timeline.json') -Encoding utf8
  $log | ForEach-Object { '{0,7}  {1}' -f $_.t, $_.title }
} finally {
  [IO.File]::WriteAllBytes($extension, $original)
}
