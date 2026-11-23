Unicode True
!include "MUI2.nsh"

Name "Interact Studio"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Interact Studio"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "Interact Studio installer"
VIAddVersionKey "ProductName" "Interact Studio"
VIAddVersionKey "CompanyName" "Interact Studio"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}\LICENSE"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!define APP_REGISTRY "Software\Microsoft\Windows\CurrentVersion\Uninstall\InteractStudio"

Section "Interact Studio"
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    File /r "${PAYLOAD}\*.*"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateShortcut "$SMPROGRAMS\Interact Studio.lnk" "$INSTDIR\InteractStudio.exe"
    WriteRegStr HKCU "${APP_REGISTRY}" "DisplayName" "Interact Studio"
    WriteRegStr HKCU "${APP_REGISTRY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${APP_REGISTRY}" "Publisher" "Interact Studio"
    WriteRegStr HKCU "${APP_REGISTRY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${APP_REGISTRY}" "DisplayIcon" "$INSTDIR\InteractStudio.exe"
    WriteRegStr HKCU "${APP_REGISTRY}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
    WriteRegDWORD HKCU "${APP_REGISTRY}" "NoModify" 1
    WriteRegDWORD HKCU "${APP_REGISTRY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$SMPROGRAMS\Interact Studio.lnk"
    DeleteRegKey HKCU "${APP_REGISTRY}"
    !include "${UNINSTALL_FILES}"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
