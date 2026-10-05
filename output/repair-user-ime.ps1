$ErrorActionPreference = 'Stop'

$weasel = Get-ItemProperty -LiteralPath 'HKCU:\Software\Rime\Weasel'
$root = [string]$weasel.WeaselRoot
$profile = [string]$weasel.Profile

if ([string]::IsNullOrWhiteSpace($root) -or
    -not (Test-Path -LiteralPath (Join-Path $root 'WeaselSetup.exe'))) {
    throw 'WeaselSetup.exe was not found in the current-user installation.'
}

$profileArg = switch ($profile) {
    'hant'      { '/t' }
    'hongkong'  { '/hk' }
    'macau'     { '/mc' }
    'singapore' { '/sg' }
    default     { '/s' }
}

$process = Start-Process -FilePath (Join-Path $root 'WeaselSetup.exe') -ArgumentList @($profileArg, '/user') -PassThru -Wait
exit $process.ExitCode
