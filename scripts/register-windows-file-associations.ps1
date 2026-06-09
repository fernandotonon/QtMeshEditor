# Register QtMeshEditor as a per-user "Open with" handler for common 3D formats.
# Does not change the user's default app for each extension.
# Usage (from extracted zip or installed bin folder):
#   pwsh -File scripts/register-windows-file-associations.ps1
#   pwsh -File scripts/register-windows-file-associations.ps1 -BinDir "C:\path\to\bin"

param(
    [string]$BinDir
)

$ErrorActionPreference = "Stop"

function Resolve-QtMeshEditorBinDir {
    param([string]$Requested)
    if ($Requested) { return $Requested }

    $portableBin = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..") -ErrorAction SilentlyContinue).Path
    $repoBin     = Join-Path $PSScriptRoot "..\bin"
    if ($portableBin -and (Test-Path (Join-Path $portableBin "QtMeshEditor.exe"))) {
        return $portableBin
    }
    if (Test-Path (Join-Path $repoBin "QtMeshEditor.exe")) {
        return (Resolve-Path -LiteralPath $repoBin).Path
    }
    return $repoBin
}

$BinDir = Resolve-QtMeshEditorBinDir -Requested $BinDir
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
    $progId = "QtMeshEditor.Model.$($ext.TrimStart('.'))"
    $openWithKey = "HKCU:\Software\Classes\$ext\OpenWithProgids"
    New-Item -Path $openWithKey -Force | Out-Null
    New-ItemProperty -Path $openWithKey -Name $progId -PropertyType String -Value "" -Force | Out-Null
    New-Item -Path "HKCU:\Software\Classes\$progId" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progId" -Name "(default)" -Value $label
    New-Item -Path "HKCU:\Software\Classes\$progId\DefaultIcon" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progId\DefaultIcon" -Name "(default)" -Value "$exe,0"
    New-Item -Path "HKCU:\Software\Classes\$progId\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progId\shell\open\command" -Name "(default)" -Value "`"$exe`" `"%1`""
    Write-Host "Registered $ext in OpenWithProgids -> $progId"
}

Write-Host "Done. QtMeshEditor should appear in Explorer 'Open with' for registered extensions."
