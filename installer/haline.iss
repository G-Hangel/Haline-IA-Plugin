; Haline AI installer for Krita. Build with: ./build.sh setup
#define AppVer "1.0.0"
#define KritaSeries "5.3"

[Setup]
AppId={{5F431E9D-F967-433A-9A2B-4D5CE51F3C1E}
AppName=Haline AI for Krita
AppVersion={#AppVer}
AppVerName=Haline AI {#AppVer} for Krita
AppPublisher=Haline AI
VersionInfoVersion={#AppVer}
VersionInfoDescription=Haline AI installer for Krita
WizardStyle=modern dynamic
WizardImageFile=assets\wizard_100.bmp,assets\wizard_125.bmp,assets\wizard_150.bmp,assets\wizard_200.bmp
WizardImageFileDynamicDark=assets\wizard_100.bmp,assets\wizard_125.bmp,assets\wizard_150.bmp,assets\wizard_200.bmp
WizardSmallImageFile=assets\small_100.bmp,assets\small_125.bmp,assets\small_150.bmp,assets\small_200.bmp
WizardSmallImageFileDynamicDark=assets\small_dark_100.bmp,assets\small_dark_125.bmp,assets\small_dark_150.bmp,assets\small_dark_200.bmp
SetupIconFile=assets\haline.ico
UninstallDisplayIcon={uninstallexe}
UninstallDisplayName=Haline AI for Krita
DefaultDirName={code:GetKritaDir}
AppendDefaultDirName=no
DirExistsWarning=no
DisableDirPage=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
UninstallFilesDir={app}\lib\kritaplugins\haline
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=no
OutputDir=..\release
OutputBaseFilename=HalineAI-{#AppVer}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl,lang\en.isl"
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl,lang\ptbr.isl"
Name: "zhcn"; MessagesFile: "lang\ChineseSimplified.isl,lang\zhcn.isl"

[Files]
Source: "..\dist\lib\kritaplugins\*"; DestDir: "{app}\lib\kritaplugins"; Flags: recursesubdirs ignoreversion

[Run]
Filename: "{app}\bin\krita.exe"; Description: "{cm:LaunchKrita}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
function GetKritaDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM, 'SOFTWARE\Krita', 'InstallLocation', Result) then
    if not RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Krita_x64', 'InstallLocation', Result) then
      if not RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Krita_x64', 'InstallLocation', Result) then
        Result := ExpandConstant('{commonpf64}\Krita (x64)');
  Result := RemoveBackslashUnlessRoot(RemoveQuotes(Result));
end;

function KritaRunning: Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''krita.exe''');
    Result := Procs.Count > 0;
  except
  end;
end;

function WaitKritaClosed: Boolean;
begin
  Result := True;
  while KritaRunning do
    if SuppressibleMsgBox(CustomMessage('KritaRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Exe, Ver: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Exe := AddBackslash(WizardDirValue) + 'bin\krita.exe';
    if not FileExists(Exe) then
    begin
      SuppressibleMsgBox(CustomMessage('KritaNotFound'), mbError, MB_OK, IDOK);
      Result := False;
    end
    else if GetVersionNumbersString(Exe, Ver) and (Pos('{#KritaSeries}.', Ver) <> 1) then
      Result := SuppressibleMsgBox(FmtMessage(CustomMessage('KritaVersionMismatch'), ['{#KritaSeries}', Ver]), mbConfirmation, MB_YESNO, IDNO) = IDYES;
  end
  else if CurPageID = wpReady then
    Result := WaitKritaClosed;
end;

function InitializeUninstall: Boolean;
begin
  Result := WaitKritaClosed;
end;
