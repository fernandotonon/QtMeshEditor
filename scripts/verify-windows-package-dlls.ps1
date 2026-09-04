# Verify that every non-system DLL imported by QtMeshEditor / qtmesh (and bundled
# DLLs in bin/) is present in the package directory. Mirrors the Linux Docker
# "ldd | grep not found" check in deploy.yml.
#
# Usage (CI):
#   pwsh scripts/verify-windows-package-dlls.ps1 `
#     -BinDir "$env:GITHUB_WORKSPACE/bin" `
#     -Objdump "D:/a/QtMeshEditor/Qt/Tools/mingw1310_64/bin/objdump.exe" `
#     -OgreSdkBin "$env:GITHUB_WORKSPACE/ogre-build/SDK/bin"

param(
    [Parameter(Mandatory = $true)]
    [string]$BinDir,
    [string]$Objdump = "objdump",
    [string]$OgreSdkBin = "",
    [switch]$CopyMissingOgreDlls
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $BinDir)) {
    Write-Error "BinDir does not exist: $BinDir"
    exit 1
}

$BinDir = (Resolve-Path $BinDir).Path

# Windows loader / MinGW runtimes that are expected from System32 or need not be bundled.
# NB vcruntime*/msvcp* are NOT in this list: since ONNX Runtime (MSVC-built)
# ships in the package, the VC++ runtime must be bundled app-local — a clean
# Windows has no VC redistributable and the process would fail at startup.
$SystemDllPattern = '^(?i)(api-ms-.*|ext-ms-.*|kernel32|kernelbase|user32|gdi32|advapi32|shell32|ole32|comdlg32|ws2_32|winmm|imm32|oleaut32|setupapi|version|msvcrt|ucrtbase|dbghelp|crypt32|secur32|bcrypt|ntdll|d3d.*|dxgi|opengl32|glu32|wtsapi32|userenv|netapi32|iphlpapi|shlwapi|comctl32|uxtheme|dwmapi|propsys|windowscodecs|hid|setupapi|winspool|mpr|powrprof|cfgmgr32|devobj|wintrust|imagehlp|mswsock|dnsapi|rsaenh|bcryptprimitives|profapi)\.dll$'

function Test-IsSystemDll([string]$Name) {
    return $Name -match $SystemDllPattern
}

function Get-PeImports([string]$Path) {
    if (-not (Test-Path $Path)) { return @() }
    $imports = [System.Collections.Generic.List[string]]::new()
    $output = & $Objdump -p $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "objdump failed for $Path (exit $LASTEXITCODE)"
        return @()
    }
    foreach ($line in $output) {
        if ($line -match '^\s*DLL Name:\s*(.+)\s*$') {
            $imports.Add($Matches[1].Trim())
        }
    }
    return $imports
}

function Resolve-DllPath([string]$Name, [string]$SearchDir) {
    $local = Join-Path $SearchDir $Name
    if (Test-Path $local) { return $local }
    $sys32 = Join-Path $env:SystemRoot "System32\$Name"
    if (Test-Path $sys32) { return $sys32 }
    $syswow = Join-Path $env:SystemRoot "SysWOW64\$Name"
    if (Test-Path $syswow) { return $syswow }
    return $null
}

function Copy-OgreSdkDlls([string]$SdkBin, [string]$DestBin) {
    if (-not (Test-Path $SdkBin)) {
        Write-Host "Ogre SDK bin not found ($SdkBin); skipping Ogre DLL copy pass"
        return
    }
    $patterns = @("Ogre*.dll", "Plugin_*.dll", "Codec_*.dll", "RenderSystem_*.dll", "cg.dll")
    foreach ($pattern in $patterns) {
        Get-ChildItem -Path $SdkBin -Filter $pattern -ErrorAction SilentlyContinue | ForEach-Object {
            $dest = Join-Path $DestBin $_.Name
            if (-not (Test-Path $dest)) {
                Copy-Item $_.FullName $dest -Force
                Write-Host "Copied missing Ogre runtime: $($_.Name)"
            }
        }
    }
}

if ($CopyMissingOgreDlls -and $OgreSdkBin) {
    Copy-OgreSdkDlls -SdkBin $OgreSdkBin -DestBin $BinDir
}

$roots = @()
Get-ChildItem -Path $BinDir -Filter "*.exe" -File | ForEach-Object { $roots += $_.FullName }
if ($roots.Count -eq 0) {
    Write-Error "No .exe files found in $BinDir"
    exit 1
}

$queue = [System.Collections.Generic.Queue[string]]::new()
$processed = @{}
foreach ($r in $roots) { $queue.Enqueue($r) }

$missing = [System.Collections.Generic.List[object]]::new()

while ($queue.Count -gt 0) {
    $path = $queue.Dequeue()
    if ($processed.ContainsKey($path)) { continue }
    $processed[$path] = $true

    $imports = Get-PeImports -Path $path
    foreach ($dll in $imports) {
        if (Test-IsSystemDll $dll) { continue }

        $resolved = Resolve-DllPath -Name $dll -SearchDir $BinDir
        if (-not $resolved) {
            $missing.Add([PSCustomObject]@{
                    RequiredBy = Split-Path $path -Leaf
                    Dll        = $dll
                })
            continue
        }

        # Walk transitive deps for DLLs shipped in the package.
        if ($resolved.StartsWith($BinDir, [System.StringComparison]::OrdinalIgnoreCase) `
            -and $resolved -match '\.dll$' `
            -and -not $processed.ContainsKey($resolved)) {
            $queue.Enqueue($resolved)
        }
    }
}

Write-Host "=== Windows DLL dependency check: $BinDir ==="
Write-Host "Scanned $($processed.Count) PE file(s) (exe + bundled dll chain)"

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing DLL(s) (not in bin/ or System32):"
    $missing | Sort-Object Dll, RequiredBy -Unique | Format-Table -AutoSize
    Write-Error "$($missing.Count) missing DLL dependency(ies) in Windows package"
    exit 1
}

Write-Host "All non-system DLL dependencies resolved."
exit 0
