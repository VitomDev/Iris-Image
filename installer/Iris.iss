; Iris Image installer (Inno Setup 6).
;
; Context menu registration:
;  - Windows 11: a sparse package (Iris.msix) gives Iris the identity the
;    modern context menu requires. Iris is not code-signed, and Windows only
;    accepts unsigned packages with executable content when they are installed
;    by an administrator, which is why this installer needs elevation.
;  - Windows 10, or if the package cannot be registered: the classic context
;    menu, through an ExplorerCommandHandler verb using the same COM class.
;
; Built by build.ps1, which passes /DBuildDir=<folder with the binaries>.

#define AppVersion "1.0.1"
#define Clsid "{{4D8C2E61-7A3B-4F0E-9C15-2B6E8A1D3F47}"
#define PackageName "Iris.ImageConverter"

#ifndef BuildDir
  #define BuildDir "..\build"
#endif

[Setup]
AppId={{9B3E5C1A-2F4D-4E8B-A6C7-1D0F3E5B7A92}
AppName=Iris Image
AppVerName=Iris Image {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=VitomDev
AppPublisherURL=https://github.com/VitomDev/Iris-Image
AppSupportURL=https://github.com/VitomDev/Iris-Image/issues
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Iris Image - Instant Image Converter Setup
DefaultDirName={autopf}\Iris Image
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=IrisImageSetup
SetupIconFile=..\assets\iris.ico
UninstallDisplayIcon={app}\Iris.exe
UninstallDisplayName=Iris Image - Instant Image Converter
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
; Explorer keeps IrisShell.dll loaded; never offer to close Explorer.
CloseApplications=no
RestartIfNeededByRun=no
LicenseFile=..\LICENSE

[Files]
Source: "{#BuildDir}\Iris.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\IrisShell.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#BuildDir}\Iris.msix"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  MenuTitle = 'Iris Image ' + #$2014 + ' Convert to...';
  VerbKey = 'IrisConvert';

var
  Extensions: array of String;

procedure InitExtensions;
begin
  Extensions := ['.png', '.jpg', '.jpeg', '.jpe', '.jfif', '.webp', '.bmp', '.dib', '.gif', '.tif', '.tiff', '.ico'];
end;

function IsWindows11: Boolean;
var
  Version: TWindowsVersion;
begin
  GetWindowsVersionEx(Version);
  Result := Version.Build >= 22000;
end;

function RunPowerShell(const Command: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "' + Command + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function RegisterPackage: Boolean;
begin
  Result := RunPowerShell(
    'try { Add-AppxPackage -Path ''' + ExpandConstant('{app}\Iris.msix') + ''' ' +
    '-ExternalLocation ''' + ExpandConstant('{app}') + ''' -AllowUnsigned -ForceUpdateFromAnyVersion ' +
    '-ErrorAction Stop; exit 0 } catch { exit 1 }');
end;

procedure UnregisterPackage;
begin
  RunPowerShell(
    'Get-AppxPackage -AllUsers -Name ''{#PackageName}'' | ' +
    'Remove-AppxPackage -AllUsers -ErrorAction SilentlyContinue');
end;

procedure RegisterClassicMenu;
var
  I: Integer;
  Key: String;
begin
  Key := 'Software\Classes\CLSID\{#Clsid}';
  RegWriteStringValue(HKLM, Key, '', 'Iris Image context menu');
  RegWriteStringValue(HKLM, Key + '\InprocServer32', '', ExpandConstant('{app}\IrisShell.dll'));
  RegWriteStringValue(HKLM, Key + '\InprocServer32', 'ThreadingModel', 'Apartment');
  for I := 0 to GetArrayLength(Extensions) - 1 do
  begin
    Key := 'Software\Classes\SystemFileAssociations\' + Extensions[I] + '\shell\' + VerbKey;
    RegWriteStringValue(HKLM, Key, 'MUIVerb', MenuTitle);
    RegWriteStringValue(HKLM, Key, 'Icon', ExpandConstant('{app}\Iris.exe'));
    RegWriteStringValue(HKLM, Key, 'ExplorerCommandHandler', '{#Clsid}');
    RegWriteStringValue(HKLM, Key, 'MultiSelectModel', 'Player');
  end;
end;

procedure UnregisterClassicMenu;
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(Extensions) - 1 do
    RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Classes\SystemFileAssociations\' + Extensions[I] + '\shell\' + VerbKey);
  RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Classes\CLSID\{#Clsid}');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    InitExtensions;
    // Start clean so an upgrade never leaves both menus registered.
    UnregisterClassicMenu;
    if not (IsWindows11 and RegisterPackage) then
      RegisterClassicMenu;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    InitExtensions;
    UnregisterPackage;
    UnregisterClassicMenu;
  end;
end;
