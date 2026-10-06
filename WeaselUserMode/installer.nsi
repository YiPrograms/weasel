Unicode true

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "dev"
!endif

Name "Weasel User Mode"
OutFile "output\archives\weasel-user-mode-${PRODUCT_VERSION}-installer.exe"
InstallDir "$LOCALAPPDATA\Rime\WeaselUserMode"
InstallDirRegKey HKCU "Software\Rime\Weasel\UserMode" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WeaselUserMode"
!define USERMODE_KEY "Software\Rime\Weasel\UserMode"

Section "Weasel User Mode" SecMain
  SetShellVarContext current

  ; Stop a previous user-mode frontend before replacing its files. This does
  ; not require elevation because the process belongs to the current user.
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM WeaselUserMode.exe /F'

  SetOutPath "$INSTDIR"
  File "output\WeaselUserMode.exe"
  File "output\WeaselServer.exe"
  File "output\WeaselDeployer.exe"
  File "output\rime.dll"
  File "output\WinSparkle.dll"
  File "output\rime-install.bat"
  File "output\rime-install-config.bat"
  File "output\7z.exe"
  File "output\7z.dll"
  File "output\curl.exe"
  File "output\curl-ca-bundle.crt"
  File "output\COPYING-curl.txt"
  File "output\7-zip-license.txt"

  SetOutPath "$INSTDIR\data"
  File "output\data\*.yaml"
  File /nonfatal "output\data\*.txt"
  File /nonfatal "output\data\*.gram"

  SetOutPath "$INSTDIR\data\opencc"
  File "output\data\opencc\*.json"
  File "output\data\opencc\*.ocd*"

  SetOutPath "$INSTDIR\data\preview"
  File /nonfatal "output\data\preview\*.png"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "${USERMODE_KEY}" "InstallDir" "$INSTDIR"
  WriteRegDWORD HKCU "${USERMODE_KEY}" "StartEnabled" 1
  ; MOD_ALT | MOD_CONTROL, VK_SPACE. These can be changed later without admin.
  WriteRegDWORD HKCU "${USERMODE_KEY}" "ToggleModifiers" 3
  WriteRegDWORD HKCU "${USERMODE_KEY}" "ToggleVirtualKey" 0x20
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
    "WeaselUserMode" '"$INSTDIR\WeaselUserMode.exe"'

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Weasel User Mode"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Rime"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\WeaselServer.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\Weasel User Mode"
  CreateShortCut "$SMPROGRAMS\Weasel User Mode\Weasel User Mode.lnk" \
    "$INSTDIR\WeaselUserMode.exe"
  CreateShortCut "$SMPROGRAMS\Weasel User Mode\Weasel Settings.lnk" \
    "$INSTDIR\WeaselDeployer.exe"
  CreateShortCut "$SMPROGRAMS\Weasel User Mode\Uninstall.lnk" \
    "$INSTDIR\Uninstall.exe"

  Exec '"$INSTDIR\WeaselUserMode.exe"'
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM WeaselUserMode.exe /F'

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
    "WeaselUserMode"
  DeleteRegKey HKCU "${USERMODE_KEY}"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"

  RMDir /r "$SMPROGRAMS\Weasel User Mode"
  SetOutPath "$TEMP"
  RMDir /r "$INSTDIR"
SectionEnd
