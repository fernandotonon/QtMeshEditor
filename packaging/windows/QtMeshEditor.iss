; Inno Setup script — per-user install with HKCU file associations (#664)
#define MyAppName "QtMeshEditor"
#define MyAppVersion "@PROJECT_VERSION@"
#define MyAppPublisher "Fernando Tonon"
#define MyAppURL "https://github.com/fernandotonon/QtMeshEditor"
#define MyAppExeName "QtMeshEditor.exe"

[Setup]
AppId={{A4B8D6F2-3C1E-4F9A-9B2D-664E8A3F9B2D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=QtMeshEditor-{#MyAppVersion}-setup-Windows
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "fileassoc"; Description: "Register QtMeshEditor as an ""Open with"" handler for 3D model files"; GroupDescription: "File associations:"; Flags: checkedonce

[Files]
Source: "..\..\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"

[Registry]
; Animatable / common interchange formats (Alternate handler)
Root: HKCU; Subkey: "Software\Classes\.fbx"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.fbx"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.fbx"; ValueType: string; ValueName: ""; ValueData: "FBX 3D Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.fbx\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.fbx\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.glb"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.glb"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.glb"; ValueType: string; ValueName: ""; ValueData: "glTF Binary Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.glb\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.glb\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.gltf"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.gltf"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.gltf"; ValueType: string; ValueName: ""; ValueData: "glTF Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.gltf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.gltf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.obj"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.obj"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.obj"; ValueType: string; ValueName: ""; ValueData: "Wavefront OBJ Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.obj\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.obj\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.dae"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.dae"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.dae"; ValueType: string; ValueName: ""; ValueData: "Collada Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.dae\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.dae\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.stl"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.stl"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.stl"; ValueType: string; ValueName: ""; ValueData: "STL Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.stl\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.stl\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.ply"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.ply"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.ply"; ValueType: string; ValueName: ""; ValueData: "PLY Model"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.ply\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.ply\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

; Native / PS1 formats (Owner)
Root: HKCU; Subkey: "Software\Classes\.mesh"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.mesh"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.mesh"; ValueType: string; ValueName: ""; ValueData: "Ogre Mesh"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.mesh\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.mesh\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.rsd"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.rsd"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.rsd"; ValueType: string; ValueName: ""; ValueData: "PlayStation RSD Mesh"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.rsd\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.rsd\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.tmd"; ValueType: string; ValueName: ""; ValueData: "QtMeshEditor.Model.tmd"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.tmd"; ValueType: string; ValueName: ""; ValueData: "PlayStation TMD Mesh"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.tmd\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\QtMeshEditor.Model.tmd\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
