; ED-FSR installer (Inno Setup 6).
;
;   ISCC.exe /DAppVersion=1.2.3 /DSrcDir=<staged package> /DOutDir=<output dir> installer\edfsr.iss
;
; SrcDir is the staged package (CI: dist\ed-fsr) with this layout:
;   dinput8.dll, README.md, edfsr\edfsr.ini, edfsr\bin\amd_fidelityfx_*_dx12.dll
; Installed layout (GameDir = folder containing EliteDangerous64.exe):
;   <GameDir>\dinput8.dll
;   <GameDir>\edfsr\edfsr.ini, README.md, bin\*.dll, logs\

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef SrcDir
  #define SrcDir "..\dist\ed-fsr"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

; Numeric x.y.z for the exe version resource: strip a leading "v" and any "-suffix".
#define VerCore AppVersion
#if Copy(VerCore, 1, 1) == "v" || Copy(VerCore, 1, 1) == "V"
  #define VerCore Copy(VerCore, 2)
#endif
#if Pos("-", VerCore) > 0
  #define VerCore Copy(VerCore, 1, Pos("-", VerCore) - 1)
#endif

#define AppName "ED-FSR (FSR 4 / Frame Generation for Elite Dangerous)"

[Setup]
AppId={{B6411AE1-69B2-4B6D-ADED-EB0F4E9AC5F2}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=ED-FSR contributors
AppPublisherURL=https://github.com/anatolijphp/EliteDangerous-FSR4
AppSupportURL=https://github.com/anatolijphp/EliteDangerous-FSR4/issues
AppUpdatesURL=https://github.com/anatolijphp/EliteDangerous-FSR4/releases
VersionInfoVersion={#VerCore}
VersionInfoProductName=ED-FSR
VersionInfoDescription=ED-FSR installer (unofficial mod for Elite Dangerous)
LicenseFile=..\LICENSE
; {app} is the game folder chosen by the user (must contain EliteDangerous64.exe).
DefaultDirName={code:GetDefaultGameDir}
AppendDefaultDirName=no
DirExistsWarning=no
DisableDirPage=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
CreateUninstallRegKey=yes
; Keep the uninstaller out of the game's own files.
UninstallFilesDir={app}\edfsr\uninstall
UninstallDisplayName={#AppName}
PrivilegesRequired=admin
; /CURRENTUSER on the command line only (for testing into a writable folder); no dialog.
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=ED-FSR-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
SelectDirLabel3=Select the Elite Dangerous game folder: the folder that contains EliteDangerous64.exe (for example ...\EliteDangerous\Products\elite-dangerous-odyssey-64).
SelectDirBrowseLabel=Setup detected the folder below. Click Next to continue, or Browse to choose another game folder.
FinishedLabelNoIcons=Setup has finished installing [name] into the game folder.%n%nIn game: Upscaling = FSR, Borderless window for frame generation, press CTRL+F8 for settings.
FinishedLabel=Setup has finished installing [name] into the game folder.%n%nIn game: Upscaling = FSR, Borderless window for frame generation, press CTRL+F8 for settings.

[Dirs]
Name: "{app}\edfsr"
Name: "{app}\edfsr\bin"
; The game runs as a normal user: the mod must be able to create log files here.
Name: "{app}\edfsr\logs"; Permissions: users-modify

[Files]
Source: "{#SrcDir}\dinput8.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\edfsr\bin\amd_fidelityfx_loader_dx12.dll"; DestDir: "{app}\edfsr\bin"; Flags: ignoreversion
Source: "{#SrcDir}\edfsr\bin\amd_fidelityfx_upscaler_dx12.dll"; DestDir: "{app}\edfsr\bin"; Flags: ignoreversion
Source: "{#SrcDir}\edfsr\bin\amd_fidelityfx_framegeneration_dx12.dll"; DestDir: "{app}\edfsr\bin"; Flags: ignoreversion
; Keep the user's settings on upgrade; removed on uninstall. The in-game panel saves to it, so users need write access.
Source: "{#SrcDir}\edfsr\edfsr.ini"; DestDir: "{app}\edfsr"; Flags: onlyifdoesntexist; Permissions: users-modify
Source: "{#SrcDir}\README.md"; DestDir: "{app}\edfsr"; Flags: ignoreversion

[UninstallDelete]
; Everything the mod created at runtime (logs, saved settings) lives under edfsr\.
Type: filesandordirs; Name: "{app}\edfsr"

[Code]
const
  GameExe = 'EliteDangerous64.exe';
  OurMarker = 'ED-FSR';
  BackupSuffix = '.edfsr-backup';

var
  DefaultGameDir: String;
  FirstFoundRoot: String;

function HasGameExe(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(Dir) + GameExe);
end;

{ Root (install folder of the game) -> folder with EliteDangerous64.exe, or '' }
function ResolveGameDir(Root: String): String;
var
  FindRec: TFindRec;
  Products: String;
begin
  Result := '';
  Root := RemoveBackslashUnlessRoot(Root);
  if (Root = '') or not DirExists(Root) then Exit;
  if FirstFoundRoot = '' then FirstFoundRoot := Root;
  if HasGameExe(Root) then begin Result := Root; Exit; end;
  Products := AddBackslash(Root) + 'Products';
  if HasGameExe(Products + '\elite-dangerous-odyssey-64') then begin
    Result := Products + '\elite-dangerous-odyssey-64';
    Exit;
  end;
  if FindFirst(Products + '\*', FindRec) then begin
    try
      repeat
        if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and
           (FindRec.Name <> '.') and (FindRec.Name <> '..') and
           HasGameExe(Products + '\' + FindRec.Name) then begin
          Result := Products + '\' + FindRec.Name;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ Unescape a JSON / VDF string value: \\ -> \ and / -> \ }
function UnescapePath(S: String): String;
begin
  StringChangeEx(S, '\\', '\', True);
  StringChangeEx(S, '/', '\', True);
  Result := S;
end;

{ Value of the first quoted string after position P in S (S[P] is inside or before the key's tail). }
function QuotedValueAfter(const S: String; P: Integer): String;
var
  Rest: String;
  Q: Integer;
begin
  Result := '';
  Rest := Copy(S, P, Length(S));
  Q := Pos('"', Rest);
  if Q = 0 then Exit;
  Rest := Copy(Rest, Q + 1, Length(Rest));
  Q := Pos('"', Rest);
  if Q = 0 then Exit;
  Result := Copy(Rest, 1, Q - 1);
end;

{ 1. Epic Games Launcher manifests }
function DetectEpic: String;
var
  FindRec: TFindRec;
  Dir, Loc: String;
  Content: AnsiString;
  P: Integer;
begin
  Result := '';
  Dir := ExpandConstant('{commonappdata}') + '\Epic\EpicGamesLauncher\Data\Manifests';
  if not FindFirst(Dir + '\*.item', FindRec) then Exit;
  try
    repeat
      if LoadStringFromFile(Dir + '\' + FindRec.Name, Content) then begin
        P := Pos('"InstallLocation"', String(Content));
        if P > 0 then begin
          Loc := UnescapePath(QuotedValueAfter(String(Content), P + Length('"InstallLocation"')));
          if Pos('elitedangerous', Lowercase(Loc)) > 0 then begin
            Result := ResolveGameDir(Loc);
            if Result <> '' then Exit;
          end;
        end;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

{ 2. Steam: SteamPath + every library in libraryfolders.vdf }
function DetectSteam: String;
var
  SteamPath, Lib: String;
  Lines: TArrayOfString;
  I, P: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then Exit;
  SteamPath := RemoveBackslashUnlessRoot(UnescapePath(SteamPath));
  Result := ResolveGameDir(SteamPath + '\steamapps\common\Elite Dangerous');
  if Result <> '' then Exit;
  if not LoadStringsFromFile(SteamPath + '\steamapps\libraryfolders.vdf', Lines) then Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    P := Pos('"path"', Lines[I]);
    if P > 0 then begin
      Lib := RemoveBackslashUnlessRoot(UnescapePath(QuotedValueAfter(Lines[I], P + Length('"path"'))));
      if Lib <> '' then begin
        Result := ResolveGameDir(Lib + '\steamapps\common\Elite Dangerous');
        if Result <> '' then Exit;
      end;
    end;
  end;
end;

{ 3. Frontier standalone launcher }
function DetectFrontier: String;
begin
  Result := ResolveGameDir(ExpandConstant('{commonpf32}') + '\Frontier\EDLaunch');
  if Result = '' then
    Result := ResolveGameDir(ExpandConstant('{commonpf64}') + '\Frontier\EDLaunch');
end;

function GetDefaultGameDir(Param: String): String;
begin
  if DefaultGameDir = '' then begin
    DefaultGameDir := DetectEpic;
    if DefaultGameDir = '' then DefaultGameDir := DetectSteam;
    if DefaultGameDir = '' then DefaultGameDir := DetectFrontier;
    if DefaultGameDir = '' then begin
      { Nothing with the exe found: suggest the usual Products path under the first root seen, else Frontier's. }
      if FirstFoundRoot <> '' then
        DefaultGameDir := FirstFoundRoot + '\Products\elite-dangerous-odyssey-64'
      else
        DefaultGameDir := ExpandConstant('{commonpf32}') + '\Frontier\EDLaunch\Products\elite-dangerous-odyssey-64';
    end;
    Log('Detected game folder: ' + DefaultGameDir);
  end;
  Result := DefaultGameDir;
end;

{ True if Path is an ED-FSR build of dinput8.dll (the mod's binary contains the 'ED-FSR' string). }
function IsOurDll(const Path: String): Boolean;
var
  Content: AnsiString;
begin
  Result := LoadStringFromFile(Path, Content) and (Pos(OurMarker, Content) > 0);
end;

function ForeignDllPresent(const Dir: String): Boolean;
var
  Dll: String;
begin
  Dll := AddBackslash(Dir) + 'dinput8.dll';
  Result := FileExists(Dll) and not IsOurDll(Dll);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir: String;
begin
  Result := True;
  if CurPageID <> wpSelectDir then Exit;
  Dir := RemoveBackslashUnlessRoot(WizardDirValue);
  if not HasGameExe(Dir) then begin
    SuppressibleMsgBox(GameExe + ' was not found in:' + #13#10 + Dir + #13#10#13#10 +
      'Choose the Elite Dangerous game folder that contains ' + GameExe +
      ' (for example ...\EliteDangerous\Products\elite-dangerous-odyssey-64).', mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  if ForeignDllPresent(Dir) then
    Result := SuppressibleMsgBox('The game folder already contains a dinput8.dll that is not ED-FSR (probably another mod).' + #13#10#13#10 +
      'ED-FSR needs to replace it. The existing file will be backed up as dinput8.dll' + BackupSuffix +
      ' and restored when ED-FSR is uninstalled. The other mod will not work while ED-FSR is installed.' + #13#10#13#10 +
      'Continue?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDYES) = IDYES;
end;

{ Runs for interactive and silent installs (NextButtonClick is skipped in silent mode). }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir, Dll, Bak: String;
begin
  Result := '';
  Dir := ExpandConstant('{app}');
  if not HasGameExe(Dir) then begin
    Result := GameExe + ' was not found in ' + Dir + '. Choose the Elite Dangerous game folder.';
    Exit;
  end;
  if ForeignDllPresent(Dir) then begin
    Dll := AddBackslash(Dir) + 'dinput8.dll';
    Bak := Dll + BackupSuffix;
    if not CopyFile(Dll, Bak, False) then begin
      Result := 'Could not back up the existing ' + Dll + ' to ' + Bak + '.';
      Exit;
    end;
    Log('Backed up foreign dinput8.dll to ' + Bak);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dll, Bak: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  Dll := ExpandConstant('{app}\dinput8.dll');
  Bak := Dll + BackupSuffix;
  if FileExists(Bak) then begin
    if FileExists(Dll) and IsOurDll(Dll) then DeleteFile(Dll);
    if not FileExists(Dll) then begin
      if RenameFile(Bak, Dll) then Log('Restored ' + Dll + ' from backup')
      else Log('Could not restore ' + Dll + ' from ' + Bak);
    end;
  end;
end;
