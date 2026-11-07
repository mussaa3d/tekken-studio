; tekken8mvm installer (Inno Setup 6). Build with installer\build_installer.cmd, which builds the tool first.
; Installs for the current user only, so no administrator rights are needed and the launcher can write its settings, skins and
; replay vault next to itself. The one thing the tool needs that Windows doesn't have is ffmpeg, for recording: the installer
; downloads a pinned release from GitHub and checks its SHA-256 before using it.

#define AppName "tekken8mvm"
#define AppVersion "0.1"
#define FfmpegVersion "9.0.2"
#define FfmpegUrl "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip"
#define FfmpegSha256 "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"
#define FfmpegFolder "ffmpeg-9.0.2-essentials_build"

[Setup]
AppId={{0F81B8E8-9138-4408-BD65-3C51D081EB80}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
InfoBeforeFile=before_install.txt
OutputDir=..\release
OutputBaseFilename={#AppName}-{#AppVersion}-setup
UninstallDisplayIcon={app}\tekken8mvm.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
CloseApplications=yes

[Tasks]
Name: "ffmpeg"; Description: "Download ffmpeg {#FfmpegVersion} for recording (about 110 MB)"
Name: "desktopicon"; Description: "Desktop shortcut"; Flags: unchecked

[Files]
Source: "..\dist\tekken8mvm.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\mod\dinput8.dll"; DestDir: "{app}\mod"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\tekken8mvm.exe"
Name: "{autoprograms}\{#AppName} README"; Filename: "{app}\README.md"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\tekken8mvm.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\tekken8mvm.exe"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Takes the mod and any skins back out of the game folder before the launcher that would do it is gone.
Filename: "{app}\tekken8mvm.exe"; Parameters: "--cleanup"; Flags: waituntilterminated; RunOnceId: "cleanup"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\ffmpeg"

[Code]
var
  DownloadPage: TDownloadWizardPage;
  FfmpegDownloaded: Boolean;

function HaveFfmpeg(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\ffmpeg\bin\ffmpeg.exe'));
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading ffmpeg, which records the videos...', nil);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsTaskSelected('ffmpeg') and not HaveFfmpeg() then begin
    DownloadPage.Clear;
    // Checked against this SHA-256, so a changed or broken file is never used.
    DownloadPage.Add('{#FfmpegUrl}', 'ffmpeg.zip', '{#FfmpegSha256}');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        FfmpegDownloaded := True;
      except
        if DownloadPage.AbortedByUser then
          Log('ffmpeg download cancelled')
        else
          SuppressibleMsgBox('ffmpeg couldn''t be downloaded: ' + AddPeriod(GetExceptionMessage) + #13#10#13#10 +
            'Everything else installs. To record, run this installer again later, or put ffmpeg on your PATH.', mbInformation, MB_OK, IDOK);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Unpacked: String;
begin
  if (CurStep = ssPostInstall) and FfmpegDownloaded then begin
    Unpacked := ExpandConstant('{app}\{#FfmpegFolder}');
    DelTree(Unpacked, True, True, True);
    // tar comes with Windows 10 and 11 and unpacks zip files.
    if Exec(ExpandConstant('{sys}\tar.exe'), '-xf "' + ExpandConstant('{tmp}\ffmpeg.zip') + '" -C "' + ExpandConstant('{app}') + '"',
            '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) and DirExists(Unpacked) then begin
      DelTree(ExpandConstant('{app}\ffmpeg'), True, True, True);
      if not RenameFile(Unpacked, ExpandConstant('{app}\ffmpeg')) then
        SuppressibleMsgBox('ffmpeg was downloaded but couldn''t be put in place. Recording needs ffmpeg on your PATH.', mbInformation, MB_OK, IDOK);
    end else
      SuppressibleMsgBox('ffmpeg was downloaded but couldn''t be unpacked. Recording needs ffmpeg on your PATH.', mbInformation, MB_OK, IDOK);
  end;
end;
