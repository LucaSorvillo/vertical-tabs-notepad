; Installer for Vertical Tabs for Notepad (VTabsNotepad) - Inno Setup 6
; Build:  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\VTabsNotepad.iss   (after build.ps1)
; Per-user install: no administrator rights needed.

#define AppName "Vertical Tabs for Notepad"
#define AppShortName "VTabsNotepad"
#define AppVersion "1.3.1"
#define AppPublisher "LucaSorvillo"
#define AppRepo "https://github.com/LucaSorvillo/vertical-tabs-notepad"
#define AppSite "https://lucasorvillo.github.io/vertical-tabs-notepad/"
#define AppExe "VTabsNotepad.exe"
#define AppMod "VTabsNotepadMod.dll"
#define BinDir "..\build\bin"

[Setup]
AppId={{1CC7AB8D-EE86-4696-94C9-68CF30CC9C15}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppSite}
AppSupportURL={#AppRepo}/issues
AppUpdatesURL={#AppRepo}/releases
AppCopyright=Copyright (c) 2026 {#AppPublisher} - MIT License
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription={#AppName} setup
DefaultDirName={localappdata}\Programs\{#AppShortName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 11 (the Notepad with tabs)
MinVersion=10.0.22000
OutputDir=..\dist
OutputBaseFilename={#AppShortName}-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=no
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile=..\src\launcher\VTabsNotepad.ico
UninstallDisplayName={#AppName}
LicenseFile=..\LICENSE
CloseApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[CustomMessages]
english.AutoStart=Start {#AppName} when Windows starts
italian.AutoStart=Avvia {#AppName} all'avvio di Windows
english.RemoveData=Do you also want to delete the settings and saved data (tab colors, closed tabs history)?
italian.RemoveData=Vuoi eliminare anche le impostazioni e i dati salvati (colori delle schede, cronologia delle schede chiuse)?
english.TrayNote=The app runs in the notification area (tray): right-click its icon for the options.
italian.TrayNote=L'app resta nell'area di notifica (tray): clic destro sulla sua icona per le opzioni.

[Tasks]
Name: "autostart"; Description: "{cm:AutoStart}"

[Files]
Source: "{#BinDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\{#AppMod}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "{#AppName}"

[Registry]
; Same value the app writes from its tray menu ("Avvia con Windows")
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppShortName}"; ValueData: """{app}\{#AppExe}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--exit"; Flags: runhidden waituntilterminated; RunOnceId: "StopTray"

[Code]
// Before installing over a running copy: ask the tray to exit, so its files can be replaced.
procedure StopTray();
var
  ResultCode: Integer;
  Exe: String;
begin
  Exe := ExpandConstant('{app}\{#AppExe}');
  if FileExists(Exe) then
  begin
    Exec(Exe, '--exit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1500);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopTray();
  Result := '';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 + CustomMessage('TrayNote');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Data := ExpandConstant('{localappdata}\{#AppShortName}');
    if DirExists(Data) and (not UninstallSilent) then
      if MsgBox(CustomMessage('RemoveData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(Data, True, True, True);
  end;
end;
