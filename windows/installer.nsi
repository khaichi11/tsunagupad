; Tsunagu-Pad, pemasang Windows (NSIS).
; Dibangun oleh windows/build.sh:
;   makensis -DVERSION=0.2.0 -DSTAGE=...\stage -DROOT=... -DOUTFILE=...\setup.exe windows/installer.nsi
;
; Pemasang dibuka dengan pilihan bahasa: English atau Bahasa Indonesia.
; Semua teksnya ada di bagian "teks" di bawah, dalam kedua bahasa.
;
; SPDX-License-Identifier: MIT

Unicode true
ManifestDPIAware true
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef STAGE
  !error "STAGE belum diberikan (folder hasil windows/build.sh)"
!endif
!ifndef OUTFILE
  !define OUTFILE "Tsunagu-Pad-${VERSION}-setup.exe"
!endif

!define APP "Tsunagu-Pad"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Tsunagu-Pad"
!define OUR_KEY "Software\TsunaguPad"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"

Name "${APP}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP}"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "${APP}"
VIAddVersionKey /LANG=0 "FileDescription" "${APP} Setup"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "LegalCopyright" "MIT License"

!define MUI_ICON "${ROOT}\build\tsunagupad.ico"
!define MUI_UNICON "${ROOT}\build\tsunagupad.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Tsunagu-Pad ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "$(RunText)"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchTray
!define MUI_FINISHPAGE_SHOWREADME "https://github.com/khaichi11/tsunagupad#windows"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "$(ReadmeText)"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_COMPONENTSPAGE_SMALLDESC

; Pilihan bahasa di awal; pilihannya diingat untuk penghapusan.
!define MUI_LANGDLL_WINDOWTITLE "${APP}"
!define MUI_LANGDLL_INFO "Choose a language / Pilih bahasa"
!define MUI_LANGDLL_REGISTRY_ROOT "HKLM"
!define MUI_LANGDLL_REGISTRY_KEY "${OUR_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "$(LicenseFile)"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Indonesian"
!insertmacro MUI_RESERVEFILE_LANGDLL

; ---------------------------------------------------------------- teks

LicenseLangString LicenseFile ${LANG_ENGLISH} "${ROOT}\windows\installer-license-en.txt"
LicenseLangString LicenseFile ${LANG_INDONESIAN} "${ROOT}\windows\installer-license-id.txt"

LangString WelcomeText ${LANG_ENGLISH} "Tsunagu-Pad turns an iPad into a touch screen for Windows, a second screen with no cable, and the other way round shows the iPad screen on Windows over AirPlay.$\r$\n$\r$\nEverything runs on your own local network. Nothing is uploaded to any server, and the iPad needs no app.$\r$\n$\r$\nThis installer also opens Windows Firewall for the local network and Tailscale only, and when selected installs the second screen driver and a service for the sign-in screen."
LangString WelcomeText ${LANG_INDONESIAN} "Tsunagu-Pad menjadikan iPad layar sentuh untuk Windows, layar kedua tanpa kabel, dan sebaliknya menampilkan layar iPad di Windows lewat AirPlay.$\r$\n$\r$\nSemuanya berjalan di jaringan lokal Anda sendiri. Tidak ada yang diunggah ke server mana pun, dan iPad tidak perlu memasang aplikasi apa pun.$\r$\n$\r$\nPemasang ini juga membuka firewall Windows untuk jaringan lokal dan Tailscale saja, serta (bila dipilih) memasang driver layar kedua dan layanan untuk layar login."
LangString RunText ${LANG_ENGLISH} "Start Tsunagu-Pad now"
LangString RunText ${LANG_INDONESIAN} "Jalankan Tsunagu-Pad sekarang"
LangString ReadmeText ${LANG_ENGLISH} "Open the user guide"
LangString ReadmeText ${LANG_INDONESIAN} "Buka panduan pemakaian"

LangString SecCoreName ${LANG_ENGLISH} "Tsunagu-Pad (required)"
LangString SecCoreName ${LANG_INDONESIAN} "Tsunagu-Pad (wajib)"
LangString SecDisplayName ${LANG_ENGLISH} "Second screen (Tsunagu-Pad driver)"
LangString SecDisplayName ${LANG_INDONESIAN} "Layar kedua (driver Tsunagu-Pad)"
LangString SecServiceName ${LANG_ENGLISH} "Sign-in screen and next session service"
LangString SecServiceName ${LANG_INDONESIAN} "Layanan layar login dan sesi berikutnya"
LangString SecPathName ${LANG_ENGLISH} "tsunagupad command in the terminal (PATH)"
LangString SecPathName ${LANG_INDONESIAN} "Perintah tsunagupad di terminal (PATH)"

LangString DESC_Core ${LANG_ENGLISH} "Program, tray panel, iPad page, AirPlay (UxPlay), and firewall rules for the local network."
LangString DESC_Core ${LANG_INDONESIAN} "Program, panel di area notifikasi, halaman iPad, AirPlay (UxPlay), dan aturan firewall untuk jaringan lokal."
LangString DESC_Display ${LANG_ENGLISH} "Tsunagu-Pad's own virtual monitor driver, so the iPad can be a second screen. Signed on this computer; no third party involved."
LangString DESC_Display ${LANG_INDONESIAN} "Driver monitor virtual milik Tsunagu-Pad sendiri, supaya iPad bisa jadi layar kedua. Ditandatangani di komputer ini, tanpa pihak ketiga."
LangString DESC_Service ${LANG_ENGLISH} "Shows the sign-in screen on the iPad and starts it at the next session (when enabled in the panel)."
LangString DESC_Service ${LANG_INDONESIAN} "Menampilkan layar login di iPad dan menyalakan iPad otomatis di sesi berikutnya (bila diaktifkan di panel)."
LangString DESC_Path ${LANG_ENGLISH} "Adds tsunagupad to PATH for use from a terminal or SSH."
LangString DESC_Path ${LANG_INDONESIAN} "Menambahkan tsunagupad ke PATH supaya perintahnya bisa dipakai dari terminal atau SSH."

LangString MsgNeed64 ${LANG_ENGLISH} "Tsunagu-Pad for Windows needs 64-bit Windows 10 or 11."
LangString MsgNeed64 ${LANG_INDONESIAN} "Tsunagu-Pad untuk Windows butuh Windows 10/11 64-bit."
LangString MsgDisplayFailed ${LANG_ENGLISH} "The second screen driver could not be installed (code $0). Everything else works; run this installer again to retry the second screen."
LangString MsgDisplayFailed ${LANG_INDONESIAN} "Driver layar kedua gagal dipasang (kode $0). Fitur lain tetap berjalan; jalankan pemasang ini sekali lagi untuk mencoba layar kedua lagi."

LangString DetailOldDriver ${LANG_ENGLISH} "Removing the virtual display driver of an earlier version (if any)"
LangString DetailOldDriver ${LANG_INDONESIAN} "Melepas driver layar virtual dari versi sebelumnya (bila ada)"
LangString DetailFirewall ${LANG_ENGLISH} "Opening Windows Firewall for the local network and Tailscale"
LangString DetailFirewall ${LANG_INDONESIAN} "Membuka Windows Firewall untuk jaringan lokal dan Tailscale"
LangString DetailDisplay ${LANG_ENGLISH} "Signing and installing the second screen driver"
LangString DetailDisplay ${LANG_INDONESIAN} "Menandatangani dan memasang driver layar kedua"
LangString DetailService ${LANG_ENGLISH} "Installing the Tsunagu-Pad service"
LangString DetailService ${LANG_INDONESIAN} "Memasang layanan Tsunagu-Pad"

LangString ShortcutToggle ${LANG_ENGLISH} "Tsunagu-Pad - Turn on or off"
LangString ShortcutToggle ${LANG_INDONESIAN} "Tsunagu-Pad - Nyalakan atau matikan"
LangString ShortcutUninstall ${LANG_ENGLISH} "Uninstall Tsunagu-Pad"
LangString ShortcutUninstall ${LANG_INDONESIAN} "Hapus Tsunagu-Pad"

; ---------------------------------------------------------------- pemasangan

!macro StopEverything
  ; Panel, sesi berbagi layar, dan UxPlay lama dihentikan dulu supaya berkasnya bisa ditimpa.
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM tsunagupad-tray.exe'
  Pop $0
  IfFileExists "$INSTDIR\bin\tsunagupad.exe" 0 +3
    nsExec::Exec '"$INSTDIR\bin\tsunagupad.exe" service stop'
    Pop $0
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM tsunagupad.exe'
  Pop $0
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM uxplay.exe'
  Pop $0
!macroend

Section "$(SecCoreName)" SecCore
  SectionIn RO
  SetShellVarContext all
  !insertmacro StopEverything

  SetOutPath "$INSTDIR"
  ; Sisa versi sebelumnya (plugin yang sudah tidak dipakai, driver lama) dibersihkan dulu.
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  RMDir /r "$INSTDIR\drivers"
  File /r "${STAGE}\*"

  ; Versi 0.2.0 bisa memasang Virtual Display Driver (ditandatangani SignPath
  ; Foundation). Driver itu dilepas bila dulu dipasang oleh Tsunagu-Pad;
  ; driver yang dipasang sendiri oleh pengguna dibiarkan.
  DetailPrint "$(DetailOldDriver)"
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup vdd-remove'
  Pop $0

  ; Versi 0.2.0 menaruh folder bin (berisi ratusan DLL, termasuk OpenSSL)
  ; di PATH sistem; program lain bisa ikut memuat DLL itu. Entri lama dihapus.
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup path-remove "$INSTDIR\bin"'
  Pop $0

  ; Firewall: hanya subnet lokal dan Tailscale yang boleh masuk.
  DetailPrint "$(DetailFirewall)"
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup firewall-add'
  Pop $0

  RMDir /r "$SMPROGRAMS\${APP}"
  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortCut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\bin\tsunagupad-tray.exe" "" "$INSTDIR\bin\tsunagupad-tray.exe" 0
  ; Bisa di-pin ke Start atau taskbar: satu klik menyalakan atau mematikan berbagi layar.
  CreateShortCut "$SMPROGRAMS\${APP}\$(ShortcutToggle).lnk" "$INSTDIR\bin\tsunagupad-tray.exe" "--toggle" "$INSTDIR\bin\tsunagupad-tray.exe" 0
  CreateShortCut "$SMPROGRAMS\${APP}\$(ShortcutUninstall).lnk" "$INSTDIR\uninstall.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "khaichi11"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/khaichi11/tsunagupad"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\tsunagupad-tray.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

!if /FileExists "${STAGE}\drivers\display\TsunaguPadDisplay.inf"
Section "$(SecDisplayName)" SecDisplay
  ; Driver Indirect Display milik Tsunagu-Pad (windows/driver). Tanda tangan
  ; katalognya dibuat di sini dengan sertifikat sekali pakai yang kunci
  ; rahasianya langsung dihapus (windows/display-install.ps1).
  DetailPrint "$(DetailDisplay)"
  ${DisableX64FSRedirection}
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\share\tsunagupad\display-install.ps1" -DriverDir "$INSTDIR\drivers\display" -Exe "$INSTDIR\bin\tsunagupad.exe"'
  Pop $0
  ${EnableX64FSRedirection}
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 != 0
    MessageBox MB_ICONEXCLAMATION|MB_OK "$(MsgDisplayFailed)" /SD IDOK
  ${EndIf}
SectionEnd
!endif

Section "$(SecServiceName)" SecService
  ; Tidak melakukan apa pun sampai sakelar "iPad aktif di sesi berikutnya" dinyalakan di panel.
  DetailPrint "$(DetailService)"
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" service install'
  Pop $0
SectionEnd

Section "$(SecPathName)" SecPath
  ; Hanya folder kecil berisi satu peluncur yang masuk PATH, bukan folder
  ; bin beserta DLL-nya, supaya program lain tidak ikut memuat DLL kita.
  CreateDirectory "$INSTDIR\cmd"
  FileOpen $1 "$INSTDIR\cmd\tsunagupad.cmd" w
  FileWrite $1 '@"%~dp0..\bin\tsunagupad.exe" %*$\r$\n'
  FileClose $1
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup path-add "$INSTDIR\cmd"'
  Pop $0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} $(DESC_Core)
  !if /FileExists "${STAGE}\drivers\display\TsunaguPadDisplay.inf"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDisplay} $(DESC_Display)
  !endif
  !insertmacro MUI_DESCRIPTION_TEXT ${SecService} $(DESC_Service)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_Path)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  SetRegView 64
  ${IfNot} ${Silent}
    !insertmacro MUI_LANGDLL_DISPLAY
  ${EndIf}
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(MsgNeed64)"
    Abort
  ${EndIf}
  ; Pemasangan tanpa tampilan bisa melewati komponen: /S /NODISPLAY /NOSERVICE /NOPATH
  ; (/NOVDD dari versi lama masih diterima dan tidak berpengaruh).
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/NODISPLAY" $R1
  !if /FileExists "${STAGE}\drivers\display\TsunaguPadDisplay.inf"
  ${IfNot} ${Errors}
    !insertmacro UnselectSection ${SecDisplay}
  ${EndIf}
  !endif
  ClearErrors
  ${GetOptions} $R0 "/NOSERVICE" $R1
  ${IfNot} ${Errors}
    !insertmacro UnselectSection ${SecService}
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/NOPATH" $R1
  ${IfNot} ${Errors}
    !insertmacro UnselectSection ${SecPath}
  ${EndIf}
FunctionEnd

; Panel dijalankan sebagai pengguna biasa, bukan sebagai administrator:
; explorer.exe yang membukanya, sehingga hak pemasang tidak ikut menurun.
Function LaunchTray
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\bin\tsunagupad-tray.exe"'
FunctionEnd

; ------------------------------------------------------------------ penghapusan

Function un.onInit
  SetRegView 64
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  !insertmacro StopEverything
  ; Monitor virtual dilepas, layanan dan aturan firewall dihapus, lalu driver
  ; layar kedua beserta sertifikatnya dan driver lama (bila ada) dilepas.
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" second-off'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" service uninstall'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup firewall-remove'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup display-remove'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup vdd-remove'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup path-remove "$INSTDIR\bin"'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup path-remove "$INSTDIR\cmd"'
  Pop $0

  RMDir /r "$SMPROGRAMS\${APP}"
  RMDir /r "$INSTDIR\cmd"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\libexec"
  RMDir /r "$INSTDIR\share"
  RMDir /r "$INSTDIR\drivers"
  RMDir /r "$INSTDIR\licenses"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  RMDir /r "$APPDATA\TsunaguPad"   ; SetShellVarContext all: C:\ProgramData

  ; Pengaturan pengguna yang menghapus (sama seperti uninstall.sh di Linux).
  SetShellVarContext current
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Tsunagu-Pad"
  RMDir /r "$LOCALAPPDATA\tsunagupad"

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "${OUR_KEY}"
SectionEnd
