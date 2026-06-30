#ifndef MyAppVersion
  #define MyAppVersion "0.2.0"
#endif

[Setup]
AppId={{14E5381D-B01A-4DC0-A7DF-8964BF21A0E5}
AppName=VodLink
AppVersion={#MyAppVersion}
AppPublisher=VodLink
AppPublisherURL=https://github.com/Dycool/VodLink
DefaultDirName={localappdata}\VodLink\app
DefaultGroupName=VodLink
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\..\resources\vodlink.ico
UninstallDisplayIcon={app}\VodLink.exe
OutputDir=..\..\installer-output
OutputBaseFilename=VodLink-Windows-x64-Setup
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\..\dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\VodLink"; Filename: "{app}\VodLink.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\VodLink.exe"; Description: "Open VodLink"; Flags: nowait postinstall skipifsilent

[Code]
function HasCommandLineParam(const Value: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
  begin
    if CompareText(ParamStr(I), Value) = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function InitializeSetup(): Boolean;
var
  ExistingApp: String;
  ErrorCode: Integer;
begin
  ExistingApp := ExpandConstant('{localappdata}\VodLink\app\VodLink.exe');
  if FileExists(ExistingApp) and not HasCommandLineParam('/VODLINKUPDATE') then
  begin
    ShellExec('', ExistingApp, '', ExtractFileDir(ExistingApp), SW_SHOWNORMAL,
      ewNoWait, ErrorCode);
    Result := False;
  end
  else
    Result := True;
end;
