# Register QtMeshEditor as a per-user "Open with" handler for common 3D formats.
# Usage (from extracted zip or installed bin folder):
#   pwsh -File scripts/register-windows-file-associations.ps1
#   pwsh -File scripts/register-windows-file-associations.ps1 -BinDir "C:\path\to\bin"

param(
    [string]$BinDir = (Join-Path $PSScriptRoot "..\bin")
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $BinDir "QtMeshEditor.exe"
if (-not (Test-Path $exe)) {
    Write-Error "QtMeshEditor.exe not found at $exe"
}

$extensions = @{
    ".fbx"  = "FBX 3D Model"
    ".glb"  = "glTF Binary Model"
    ".gltf" = "glTF Model"
    ".obj"  = "Wavefront OBJ Model"
    ".dae"  = "Collada Model"
    ".stl"  = "STL Model"
    ".ply"  = "PLY Model"
    ".mesh" = "Ogre Mesh"
    ".rsd"  = "PlayStation RSD Mesh"
    ".tmd"  = "PlayStation TMD Mesh"
}

foreach ($entry in $extensions.GetEnumerator()) {
    $ext = $entry.Key
    $label = $entry.Value
    $progId = "QtMeshEditor.Model$($ext.Replace('.', ''))"
    New-Item -Path "HKCU:\Software\Classes\$ext" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$ext" -Name "(default)" -Value $progId
    New-Item -Path "HKCU:\Software\Classes\$progId" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progId" -Name "(default)" -Value $label
    New-Item -Path "HKCU:\Software\Classes\$progId\DefaultIcon" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progId\DefaultIcon" -Name "(default)" -Value "$exe,0"
    New-Item -Path "HKCU:\Software\Classes\$progId\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progId\shell\open\command" -Name "(default)" -Value "`"$exe`" `"%1`""
    Write-Host "Registered $ext -> $progId"
}

Write-Host "Done. QtMeshEditor should appear in Explorer 'Open with' for registered extensions."
