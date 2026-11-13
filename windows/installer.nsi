; Tsunagu-Pad, pemasang Windows (NSIS).
; Dibangun oleh windows/build.sh:
;   makensis -DVERSION=0.2.0 -DSTAGE=...\stage -DROOT=... -DOUTFILE=...\setup.exe windows/installer.nsi
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
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "Pemasang ${APP}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "${ROOT}\build\tsunagupad.ico"
!define MUI_UNICON "${ROOT}\build\tsunagupad.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Tsunagu-Pad ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "Tsunagu-Pad menjadikan iPad layar sentuh untuk Windows, layar kedua tanpa kabel, dan sebaliknya menampilkan layar iPad di Windows lewat AirPlay.$\r$\n$\r$\nSemuanya berjalan di jaringan lokal Anda sendiri. Tidak ada yang diunggah ke server mana pun, dan iPad tidak perlu memasang aplikasi apa pun.$\r$\n$\r$\nPemasang ini juga membuka firewall Windows untuk jaringan lokal dan Tailscale saja, serta (bila dipilih) memasang driver layar virtual dan layanan untuk layar login."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Jalankan Tsunagu-Pad sekarang"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchTray
!define MUI_FINISHPAGE_SHOWREADME "https://github.com/khaichi11/tsunagupad#windows"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Buka panduan pemakaian"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_COMPONENTSPAGE_SMALLDESC

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${ROOT}\windows\installer-license.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Indonesian"
!insertmacro MUI_LANGUAGE "English"

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

Section "Tsunagu-Pad (wajib)" SecCore
  SectionIn RO
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Tsunagu-Pad untuk Windows butuh Windows 10/11 64-bit."
    Abort
  ${EndIf}
  SetShellVarContext all
  !insertmacro StopEverything

  SetOutPath "$INSTDIR"
  ; Sisa versi sebelumnya (plugin yang sudah tidak dipakai) dibersihkan dulu.
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  File /r "${STAGE}\*"

  ; Firewall: hanya subnet lokal dan Tailscale yang boleh masuk.
  DetailPrint "Membuka Windows Firewall untuk jaringan lokal dan Tailscale"
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup firewall-add'
  Pop $0

  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortCut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\bin\tsunagupad-tray.exe" "" "$INSTDIR\bin\tsunagupad-tray.exe" 0
  ; Bisa di-pin ke Start atau taskbar: satu klik menyalakan atau mematikan berbagi layar.
  CreateShortCut "$SMPROGRAMS\${APP}\${APP} - Nyalakan atau matikan.lnk" "$INSTDIR\bin\tsunagupad-tray.exe" "--toggle" "$INSTDIR\bin\tsunagupad-tray.exe" 0
  CreateShortCut "$SMPROGRAMS\${APP}\Hapus ${APP}.lnk" "$INSTDIR\uninstall.exe"

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

Section "Layar kedua (driver layar virtual)" SecVdd
  ; Virtual Display Driver (MIT) menyediakan monitor tanpa kabel untuk iPad.
  ; Windows dapat menampilkan konfirmasi "pasang perangkat lunak perangkat ini";
  ; penerbitnya SignPath Foundation atas nama proyek Virtual Display Driver.
  DetailPrint "Memasang driver layar virtual (Windows mungkin meminta konfirmasi)"
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup vdd-install "$INSTDIR\drivers\vdd"'
  Pop $0
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 != 0
    MessageBox MB_ICONEXCLAMATION|MB_OK "Driver layar virtual gagal dipasang (kode $0). Fitur lain tetap berjalan; layar kedua bisa dicoba lagi dengan menjalankan pemasang ini sekali lagi."
  ${EndIf}
SectionEnd

Section "Layanan layar login dan sesi berikutnya" SecService
  ; Tidak melakukan apa pun sampai sakelar "iPad aktif di sesi berikutnya" dinyalakan di panel.
  DetailPrint "Memasang layanan Tsunagu-Pad"
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" service install'
  Pop $0
SectionEnd

Section "Perintah tsunagupad di terminal (PATH)" SecPath
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup path-add "$INSTDIR\bin"'
  Pop $0
SectionEnd

LangString DESC_Core ${LANG_INDONESIAN} "Program, panel di area notifikasi, halaman iPad, AirPlay (UxPlay), dan aturan firewall untuk jaringan lokal."
LangString DESC_Vdd ${LANG_INDONESIAN} "Monitor virtual supaya iPad bisa jadi layar kedua, bukan hanya cermin layar yang ada."
LangString DESC_Service ${LANG_INDONESIAN} "Menampilkan layar login di iPad dan menyalakan iPad otomatis di sesi berikutnya (bila diaktifkan di panel)."
LangString DESC_Path ${LANG_INDONESIAN} "Menambahkan tsunagupad ke PATH supaya perintahnya bisa dipakai dari terminal atau SSH."
LangString DESC_Core ${LANG_ENGLISH} "Program, tray panel, iPad page, AirPlay (UxPlay), and firewall rules for the local network."
LangString DESC_Vdd ${LANG_ENGLISH} "A virtual monitor so the iPad can be a second screen, not only a mirror."
LangString DESC_Service ${LANG_ENGLISH} "Shows the sign-in screen on the iPad and starts it at the next session (when enabled in the panel)."
LangString DESC_Path ${LANG_ENGLISH} "Adds tsunagupad to PATH for use from a terminal or SSH."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} $(DESC_Core)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecVdd} $(DESC_Vdd)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecService} $(DESC_Service)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_Path)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Tsunagu-Pad untuk Windows butuh Windows 10/11 64-bit."
    Abort
  ${EndIf}
  ; Pemasangan tanpa tampilan bisa melewati komponen: /S /NOVDD /NOSERVICE /NOPATH
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/NOVDD" $R1
  ${IfNot} ${Errors}
    !insertmacro UnselectSection ${SecVdd}
  ${EndIf}
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
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  !insertmacro StopEverything
  ; Monitor virtual dilepas, layanan dan aturan firewall dihapus, lalu driver
  ; layar virtual dilepas bila dulu dipasang oleh Tsunagu-Pad sendiri.
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" second-off'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" service uninstall'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup firewall-remove'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup vdd-remove'
  Pop $0
  nsExec::ExecToLog '"$INSTDIR\bin\tsunagupad.exe" setup path-remove "$INSTDIR\bin"'
  Pop $0

  RMDir /r "$SMPROGRAMS\${APP}"
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
SectionEnd
