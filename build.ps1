# Builds Iris: libwebp, Iris.exe, IrisShell.dll, the tests, the sparse package
# and the IrisSetup.exe installer.
#
#   .\build.ps1                 Build everything and run the tests.
#   .\build.ps1 -SkipTests      Build without running the tests.
#   .\build.ps1 -SkipInstaller  Build the binaries only.
#
# Requirements: Visual Studio 2022 or newer with the C++ x64 tools, and Inno
# Setup 6. The Windows SDK and libwebp are downloaded by scripts/fetch-deps.ps1.

param(
    [switch]$SkipTests,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$deps = Join-Path $root '.deps'
$out = Join-Path $root 'build'
$dist = Join-Path $root 'dist'

& (Join-Path $root 'scripts\fetch-deps.ps1')

# --- Toolchain -------------------------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 tools was not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
}

# Use the pinned Windows SDK from NuGet so builds do not depend on what is installed.
$sdk = Join-Path $deps 'microsoft.windows.sdk.cpp.10.0.28000.2526\c'
$sdkLib = Join-Path $deps 'microsoft.windows.sdk.cpp.x64.10.0.28000.2526\c'
$sdkTools = Join-Path $deps 'microsoft.windows.sdk.buildtools.10.0.28000.2526\bin\10.0.28000.0\x64'
$sdkInclude = Join-Path $sdk 'Include\10.0.28000.0'
$sdkIncludes = @('ucrt', 'um', 'shared', 'winrt') | ForEach-Object { Join-Path $sdkInclude $_ }
$env:INCLUDE = ($sdkIncludes -join ';') + ';' + $env:INCLUDE
$env:LIB = (Join-Path $sdkLib 'ucrt\x64') + ';' + (Join-Path $sdkLib 'um\x64') + ';' + $env:LIB
$env:PATH = "$sdkTools;$env:PATH"

function Invoke-Tool([string]$tool) {
    & $tool @args
    if ($LASTEXITCODE -ne 0) { throw "$tool failed with exit code $LASTEXITCODE" }
}

# --- Flags -----------------------------------------------------------------

# Static CRT: the binaries need no Visual C++ redistributable.
$commonFlags = @('/nologo', '/c', '/O2', '/Gy', '/GL', '/MT', '/guard:cf', '/utf-8', '/DNDEBUG',
                 '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00')
$cppFlags = $commonFlags + @('/std:c++20', '/EHsc', '/W4', '/WX', '/permissive-', '/sdl', '/MP',
                             '/external:anglebrackets', '/external:W0')
$linkFlags = @('/nologo', '/LTCG', '/OPT:REF', '/OPT:ICF', '/guard:cf', '/DYNAMICBASE', '/NXCOMPAT',
               '/CETCOMPAT', '/MANIFEST:NO')
$webp = Join-Path $deps 'libwebp-1.6.0'

function Build-Objects([string]$name, [string[]]$sources, [string[]]$flags) {
    $objDir = Join-Path $out "obj\$name"
    New-Item -ItemType Directory -Force $objDir | Out-Null
    Invoke-Tool cl.exe @flags "/Fo$objDir\\" @sources | Out-Host
    return ,@(Get-ChildItem $objDir -Filter *.obj | ForEach-Object FullName)
}

New-Item -ItemType Directory -Force $out | Out-Null

# --- libwebp ---------------------------------------------------------------

$webpLib = Join-Path $out 'libwebp.lib'
if (-not (Test-Path $webpLib)) {
    Write-Host 'Building libwebp'
    $webpObjects = @()
    foreach ($dir in 'sharpyuv', 'src\dec', 'src\dsp', 'src\enc', 'src\utils', 'src\demux', 'src\mux') {
        $sources = Get-ChildItem (Join-Path $webp $dir) -Filter *.c | ForEach-Object FullName
        $flags = $commonFlags + @('/W0', '/MP', "/I$webp", "/I$webp\src")
        $webpObjects += Build-Objects ('webp-' + ($dir -replace '\\', '-')) $sources $flags
    }
    Invoke-Tool lib.exe /nologo /LTCG "/OUT:$webpLib" @webpObjects
}

# --- Iris ------------------------------------------------------------------

Write-Host 'Building Iris'
$include = @("/I$webp\src", "/I$root\src")
$coreSources = Get-ChildItem (Join-Path $root 'src\core') -Filter *.cpp | ForEach-Object FullName
$coreObjects = Build-Objects 'core' $coreSources ($cppFlags + $include)
$coreLib = Join-Path $out 'iriscore.lib'
Invoke-Tool lib.exe /nologo /LTCG "/OUT:$coreLib" @coreObjects

$systemLibs = @('windowscodecs.lib', 'ole32.lib', 'shell32.lib', 'shlwapi.lib', 'user32.lib', 'propsys.lib')

$appObjects = Build-Objects 'app' (Get-ChildItem (Join-Path $root 'src\app') -Filter *.cpp | ForEach-Object FullName) ($cppFlags + $include)
Invoke-Tool rc.exe /nologo "/fo$out\Iris.res" (Join-Path $root 'src\app\Iris.rc')
Invoke-Tool link.exe @linkFlags /SUBSYSTEM:WINDOWS "/OUT:$out\Iris.exe" @appObjects "$out\Iris.res" $coreLib $webpLib @systemLibs

$shellObjects = Build-Objects 'shell' @((Join-Path $root 'src\shell\shell_extension.cpp'), (Join-Path $root 'src\core\formats.cpp')) ($cppFlags + $include)
Invoke-Tool rc.exe /nologo "/fo$out\IrisShell.res" (Join-Path $root 'src\shell\IrisShell.rc')
Invoke-Tool link.exe @linkFlags /DLL "/DEF:$root\src\shell\IrisShell.def" "/OUT:$out\IrisShell.dll" @shellObjects "$out\IrisShell.res" ole32.lib shell32.lib shlwapi.lib user32.lib runtimeobject.lib

$testObjects = Build-Objects 'tests' (Get-ChildItem (Join-Path $root 'tests') -Filter *.cpp | ForEach-Object FullName) ($cppFlags + $include)
Invoke-Tool link.exe @linkFlags /SUBSYSTEM:CONSOLE "/OUT:$out\IrisTests.exe" @testObjects $coreLib $webpLib @systemLibs advapi32.lib

if (-not $SkipTests) {
    Write-Host 'Running tests'
    Invoke-Tool (Join-Path $out 'IrisTests.exe') (Join-Path $out 'Iris.exe')
}

# --- Package and installer ---------------------------------------------------

if (-not $SkipInstaller) {
    Write-Host 'Packing the context menu package'
    Invoke-Tool makeappx.exe pack /o /nv /d (Join-Path $root 'package') /p "$out\Iris.msix"

    $iscc = @("${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 was not found.' }
    New-Item -ItemType Directory -Force $dist | Out-Null
    Invoke-Tool $iscc /Q "/DBuildDir=$out" "/O$dist" (Join-Path $root 'installer\Iris.iss')
    Write-Host "Installer: $dist\IrisSetup.exe"
}
