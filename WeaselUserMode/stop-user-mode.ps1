param([Parameter(Mandatory = $true)][string]$InstallDir)
$ErrorActionPreference = 'Continue'
$root = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
$log = Join-Path $env:TEMP 'weasel-upgrade-process-stop.txt'
"Target: $root" | Set-Content $log -Encoding UTF8
$failed = $false
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WeaselStopNative {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool QueryFullProcessImageName(IntPtr handle, uint flags,
                                                        StringBuilder path, ref uint size);
  [DllImport("kernel32.dll")]
  public static extern bool CloseHandle(IntPtr handle);
}
'@
function Resolve-ImagePath($process) {
  try { if ($process.Path) { return $process.Path } } catch {}
  try {
    $handle = [WeaselStopNative]::OpenProcess(0x1000, $false, [uint32]$process.Id)
    if ($handle -ne [IntPtr]::Zero) {
      try {
        $buffer = New-Object Text.StringBuilder 2048
        [uint32]$size = 2048
        if ([WeaselStopNative]::QueryFullProcessImageName($handle, 0, $buffer, [ref]$size)) {
          return $buffer.ToString()
        }
      } finally { [void][WeaselStopNative]::CloseHandle($handle) }
    }
  } catch { }
  try {
    $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)" -ErrorAction Stop
    if ($cim.ExecutablePath) { return $cim.ExecutablePath }
  } catch {}
  return $null
}
foreach ($name in @('WeaselUserMode', 'WeaselServer', 'WeaselDeployer')) {
  foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
    $path = Resolve-ImagePath $process
    "Examining: name=$name id=$($process.Id) path=$path" | Add-Content $log
    if (-not $path) {
      "UNKNOWN PATH: $name pid=$($process.Id) (will retry and refuse unsafe overwrite if still running)" | Add-Content $log
      continue
    }
    $dir = [IO.Path]::GetDirectoryName($path).TrimEnd('\')
    if ($dir -ine $root) { continue }
    try {
      "Stopping: $($process.Id)" | Add-Content $log
      Stop-Process -Id $process.Id -Force -ErrorAction Stop
      $process.WaitForExit(5000) | Out-Null
    } catch {
      "FAILED: $($_.Exception.Message)" | Add-Content $log
      $failed = $true
    }
  }
}
Start-Sleep -Milliseconds 700
# Confirm every same-named process either exited or points outside this
# install. An unresolved live image path is unsafe to overwrite blindly.
foreach ($name in @('WeaselUserMode', 'WeaselServer', 'WeaselDeployer')) {
  foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
    $path = Resolve-ImagePath $process
    "Remaining: name=$name id=$($process.Id) path=$path" | Add-Content $log
    if (-not $path) {
      "ERROR: Cannot identify running $name $($process.Id)" | Add-Content $log
      $failed = $true
      continue
    }
    if ([IO.Path]::GetDirectoryName($path).TrimEnd('\') -ieq $root) {
      "STILL RUNNING: $name $($process.Id)" | Add-Content $log
      $failed = $true
    }
  }
}
if ($failed) { 'Result: FAILED' | Add-Content $log; exit 1 }
'Result: SUCCESS' | Add-Content $log
exit 0
