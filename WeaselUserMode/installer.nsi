Unicode true

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "dev"
!endif

Name "Weasel User Mode"
OutFile "..\output\archives\weasel-user-mode-${PRODUCT_VERSION}-installer.exe"
InstallDir "$LOCALAPPDATA\Rime\WeaselUserMode"
InstallDirRegKey HKCU "Software\Rime\Weasel\UserMode" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WeaselUserMode"
!define USERMODE_KEY "Software\Rime\Weasel\UserMode"

!macro StopPortableProcesses
  ; Extract the process stopper into NSIS's private temp directory. It runs
  ; without admin rights and only touches binaries in this install directory.
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=stop-user-mode.ps1 "stop-user-mode.ps1"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\stop-user-mode.ps1" "$INSTDIR"'
  Pop $0
  StrCmp $0 "0" stop_finished
    MessageBox MB_OK|MB_ICONSTOP "Weasel User Mode could not stop its existing processes (code $0). Check $TEMP\weasel-upgrade-process-stop.txt and try again."
    Abort
  stop_finished:
!macroend

Function StopUserModeProcesses
  !insertmacro StopPortableProcesses
FunctionEnd

Function un.StopUserModeProcesses
  !insertmacro StopPortableProcesses
FunctionEnd

Section "Weasel User Mode" SecMain
  SetShellVarContext current
  Call StopUserModeProcesses

  SetOutPath "$INSTDIR"
  File "..\output\WeaselUserMode.exe"
  File "..\output\WeaselServer.exe"
  File "..\output\WeaselDeployer.exe"
  File "..\output\rime.dll"
  File "..\output\WinSparkle.dll"
  File "..\output\rime-install.bat"
  File "..\output\rime-install-config.bat"
  File "..\output\7z.exe"
  File "..\output\7z.dll"
  File "..\output\curl.exe"
  File "..\output\curl-ca-bundle.crt"
  File "..\output\COPYING-curl.txt"
  File "..\output\7-zip-license.txt"

  SetOutPath "$INSTDIR\data"
  File "..\output\data\*.yaml"
  File /nonfatal "..\output\data\*.txt"
  File /nonfatal "..\output\data\*.gram"

  SetOutPath "$INSTDIR\data\opencc"
  File "..\output\data\opencc\*.json"
  File "..\output\data\opencc\*.ocd*"

  SetOutPath "$INSTDIR\data\preview"
  File /nonfatal "..\output\data\preview\*.png"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Native Weasel also deploys schemas before starting its service.
  ExecWait '"$INSTDIR\WeaselDeployer.exe" /deploy' $0
  IntCmp $0 0 deploy_ok deploy_error deploy_error
  deploy_error:
    MessageBox MB_OK|MB_ICONSTOP "Rime data deployment failed (exit code $0)."
    Abort
  deploy_ok:

  WriteRegStr HKCU "${USERMODE_KEY}" "InstallDir" "$INSTDIR"
  ; Preserve existing current-user settings across an upgrade.
  ClearErrors
  ReadRegDWORD $0 HKCU "${USERMODE_KEY}" "StartEnabled"
  IfErrors 0 +2
    WriteRegDWORD HKCU "${USERMODE_KEY}" "StartEnabled" 1
  ; MOD_ALT | MOD_CONTROL, VK_F11. These can be changed later without admin.
  ClearErrors
  ReadRegDWORD $0 HKCU "${USERMODE_KEY}" "ToggleModifiers"
  IfErrors 0 +2
    WriteRegDWORD HKCU "${USERMODE_KEY}" "ToggleModifiers" 3
  ClearErrors
  ReadRegDWORD $0 HKCU "${USERMODE_KEY}" "ToggleVirtualKey"
  IfErrors 0 +2
    WriteRegDWORD HKCU "${USERMODE_KEY}" "ToggleVirtualKey" 0x7A
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WeaselUserMode" '"$INSTDIR\WeaselUserMode.exe"'

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Weasel User Mode"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Rime"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\WeaselServer.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\Weasel User Mode"
  CreateShortCut "$SMPROGRAMS\Weasel User Mode\Weasel User Mode.lnk" "$INSTDIR\WeaselUserMode.exe"
  CreateShortCut "$SMPROGRAMS\Weasel User Mode\Weasel Settings.lnk" "$INSTDIR\WeaselUserMode.exe" "--settings"
  CreateShortCut "$SMPROGRAMS\Weasel User Mode\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

  Exec '"$INSTDIR\WeaselUserMode.exe"'
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  Call un.StopUserModeProcesses

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WeaselUserMode"
  DeleteRegKey HKCU "${USERMODE_KEY}"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"

  RMDir /r "$SMPROGRAMS\Weasel User Mode"
  SetOutPath "$TEMP"
  RMDir /r "$INSTDIR"
SectionEnd
