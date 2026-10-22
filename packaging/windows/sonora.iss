; COM-4/COM-7: Windows x64 installer (Inno Setup 6, docs/INSTALLERS.md).
;
;   iscc /DAppVersion=0.1.0 /DSourceDir=<folder with Sonora.exe> /DOutputDir=<out> [/DIconFile=<icon.ico>] packaging\windows\sonora.iss
;
; SourceDir is the app folder the build writes: Sonora.exe, sonora_plugin_scanner.exe and Resources\ side by side
; (PluginHost::scannerExecutable and the Help menu look next to the executable). The C++ runtime is linked statically
; (/MT, see CMakeLists.txt), so no Visual C++ redistributable is installed.
;
; Installs per machine (Program Files) or, when the user picks it or is not an administrator, per user. Sonora is added
; to the .als "Open with" list; making it the default app for .als is an opt-in task, because people often keep the
; reference app as their default.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<folder holding Sonora.exe>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Never change AppId: Windows uses it to find the installed copy for upgrades and uninstall.
AppId={{7FC9B339-11BE-4806-B257-CBAE60253E07}
AppName=Sonora
AppVersion={#AppVersion}
AppVerName=Sonora {#AppVersion}
AppPublisher=Sonora
AppPublisherURL=https://github.com/lordillusions/sonora
DefaultDirName={autopf}\Sonora
DefaultGroupName=Sonora
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=Sonora-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\..\LICENSE
UninstallDisplayIcon={app}\Sonora.exe
UninstallDisplayName=Sonora
ChangesAssociations=yes
CloseApplications=yes
#ifdef IconFile
SetupIconFile={#IconFile}
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Make Sonora the default app for .als sets"; GroupDescription: "File types:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\Sonora.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\sonora_plugin_scanner.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Resources\*"; DestDir: "{app}\Resources"; Flags: ignoreversion recursesubdirs createallsubdirs

[Registry]
; ProgID used by the "Open with" entry and the optional default association.
Root: HKA; Subkey: "Software\Classes\Sonora.als"; ValueType: string; ValueName: ""; ValueData: "Music set"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Sonora.als\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Sonora.exe,0"
Root: HKA; Subkey: "Software\Classes\Sonora.als\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Sonora.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.als\OpenWithProgids"; ValueType: string; ValueName: "Sonora.als"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\Sonora.exe\SupportedTypes"; ValueType: string; ValueName: ".als"; ValueData: ""; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\.als"; ValueType: string; ValueName: ""; ValueData: "Sonora.als"; Tasks: associate

[Icons]
Name: "{autoprograms}\Sonora"; Filename: "{app}\Sonora.exe"
Name: "{autodesktop}\Sonora"; Filename: "{app}\Sonora.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Sonora.exe"; Description: "{cm:LaunchProgram,Sonora}"; Flags: nowait postinstall skipifsilent
