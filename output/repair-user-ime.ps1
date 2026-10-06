$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$profile = $null

$weasel = Get-ItemProperty -LiteralPath 'HKCU:\Software\Rime\Weasel' -ErrorAction SilentlyContinue
if ($weasel) {
    $profile = [string]$weasel.Profile
    if (-not (Test-Path -LiteralPath (Join-Path $root 'WeaselSetup.exe'))) {
        $root = [string]$weasel.WeaselRoot
    }
}

if ([string]::IsNullOrWhiteSpace($root) -or
    -not (Test-Path -LiteralPath (Join-Path $root 'WeaselSetup.exe'))) {
    throw 'WeaselSetup.exe was not found in the current-user installation.'
}

if ([string]::IsNullOrWhiteSpace($profile)) {
    $languages = Get-WinUserLanguageList
    if ($languages.LanguageTag -contains 'zh-Hant-TW') {
        $profile = 'hant'
    } elseif ($languages.LanguageTag -contains 'zh-Hant-HK') {
        $profile = 'hongkong'
    } elseif ($languages.LanguageTag -contains 'zh-Hant-MO') {
        $profile = 'macau'
    } elseif ($languages.LanguageTag -contains 'zh-Hans-SG') {
        $profile = 'singapore'
    } else {
        $profile = 'hans'
    }
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
