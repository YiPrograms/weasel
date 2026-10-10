param([Parameter(Mandatory = $true)][string]$InstallDir)
$ErrorActionPreference = 'Continue'
$root = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
$log = Join-Path $env:TEMP 'weasel-upgrade-process-stop.txt'
"Target: $root" | Set-Content $log -Encoding UTF8
$failed = $false
foreach ($name in @('WeaselUserMode', 'WeaselServer', 'WeaselDeployer')) {
  foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
    $path = $null
    try { $path = $process.Path } catch { }
    "Examining: name=$name id=$($process.Id) path=$path" | Add-Content $log
    if (-not $path) { continue }
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
Start-Sleep -Milliseconds 400
foreach ($name in @('WeaselUserMode', 'WeaselServer', 'WeaselDeployer')) {
  foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
    try {
      if ($process.Path -and [IO.Path]::GetDirectoryName($process.Path).TrimEnd('\') -ieq $root) {
        "STILL RUNNING: $name $($process.Id)" | Add-Content $log
        $failed = $true
      }
    } catch { }
  }
}
if ($failed) { 'Result: FAILED' | Add-Content $log; exit 1 }
'Result: SUCCESS' | Add-Content $log
exit 0
