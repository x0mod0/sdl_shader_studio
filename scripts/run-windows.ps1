#Requires -Version 5.1
<#
.SYNOPSIS
    Build (only when needed) and launch SDL Shader Studio on Windows.

.DESCRIPTION
    Checks every prerequisite before configuring, so a missing tool is reported
    as itself rather than as a CMake error three minutes in. The build step is
    skipped when the existing executable is newer than everything it is built
    from.

.PARAMETER Force
    Build even when the existing executable is up to date.

.PARAMETER Clean
    Delete the build directory and configure from scratch.

.PARAMETER DebugBuild
    Build Debug instead of Release. (Named DebugBuild because -Debug is a
    reserved PowerShell common parameter.)

.PARAMETER AppArgs
    Passed through to the app. A project path here is opened on startup.

.EXAMPLE
    .\scripts\run-windows.ps1

.EXAMPLE
    .\scripts\run-windows.ps1 examples\hello

.EXAMPLE
    .\scripts\run-windows.ps1 -Force
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Clean,
    [switch]$DebugBuild,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AppArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$BuildDir  = Join-Path $RepoRoot 'build'
$BuildType = if ($DebugBuild) { 'Debug' } else { 'Release' }
$MinCMake  = [version]'3.21'

function Write-Info { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Blue }
function Write-Warn { param([string]$Message) Write-Host "warning: $Message" -ForegroundColor Yellow }

# Every environment problem ends here: say what is missing, say how to get it,
# and stop before the build can fail in a less obvious way.
function Stop-WithError {
    param([string]$Message, [string[]]$Detail = @())
    Write-Host "error: $Message" -ForegroundColor Red
    foreach ($line in $Detail) { Write-Host "       $line" }
    exit 1
}

# --- environment -------------------------------------------------------------

# Returns the generator family so the build and the executable path agree:
# Visual Studio is multi-config (a Release\ subdirectory), Ninja and MinGW are not.
function Get-Toolchain {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        Stop-WithError 'cmake is not installed (or not on PATH).' @(
            'Install it with:  winget install Kitware.CMake',
            'or download it from https://cmake.org/download/',
            'A new terminal is needed afterwards for PATH changes to apply.')
    }

    $versionLine = (& cmake --version | Select-Object -First 1)
    if ($versionLine -match '(\d+)\.(\d+)(?:\.(\d+))?') {
        $patch = if ($Matches[3]) { $Matches[3] } else { '0' }
        $version = [version]('{0}.{1}.{2}' -f $Matches[1], $Matches[2], $patch)
        if ($version -lt $MinCMake) {
            Stop-WithError "cmake $version is too old; $MinCMake or newer is required." @(
                'Upgrade with:  winget upgrade Kitware.CMake')
        }
    } else {
        Write-Warn "could not read the cmake version from '$versionLine'; carrying on."
    }

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Stop-WithError 'git is not installed (or not on PATH).' @(
            'CMake fetches SDL3, SDL_shadercross, Dear ImGui and toml++ with it when they',
            'are not already installed.',
            'Install it with:  winget install Git.Git')
    }

    # A Developer PowerShell has cl.exe on PATH; from a plain shell, CMake can
    # still find Visual Studio itself, which is what vswhere is checked for.
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return 'msvc' }

    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $installed = & $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installed) { return 'msvc' }
        }
    }

    if (Get-Command g++ -ErrorAction SilentlyContinue) { return 'mingw' }

    Stop-WithError 'no C++ compiler found.' @(
        'This project needs C++20: MSVC 19.36+ (Visual Studio 2022 17.6) or GCC 12+.',
        '',
        'Either install the Visual Studio 2022 Build Tools with the',
        '"Desktop development with C++" workload:',
        '  winget install Microsoft.VisualStudio.2022.BuildTools',
        '',
        'then run this script from "Developer PowerShell for VS 2022",',
        'or install MSYS2/MinGW and put g++ on PATH.')
}

if ($PSVersionTable.PSVersion.Major -ge 6 -and -not $IsWindows) {
    Stop-WithError 'this script is for Windows.' @(
        'Use scripts/run-macos.sh or scripts/run-linux.sh instead.')
}

$toolchain = Get-Toolchain
$multiConfig = ($toolchain -eq 'msvc')

$AppExe = if ($multiConfig) {
    Join-Path $BuildDir "bin\$BuildType\SS Studio.exe"
} else {
    Join-Path $BuildDir 'bin\SS Studio.exe'
}

# --- build -------------------------------------------------------------------

# True when the app has never been built, or when anything it is built from has
# changed since. Keeps the common case - run, look, run again - free of a build
# step that would have nothing to do.
function Test-NeedsBuild {
    if (-not (Test-Path $AppExe)) { return $true }
    $builtAt = (Get-Item $AppExe).LastWriteTimeUtc

    $roots = @('CMakeLists.txt', 'cmake', 'src', 'include', 'templates', 'tools') |
        ForEach-Object { Join-Path $RepoRoot $_ } |
        Where-Object { Test-Path $_ }

    foreach ($root in $roots) {
        $newer = Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -gt $builtAt } |
            Select-Object -First 1
        if ($newer) { return $true }
    }
    return $false
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Info "removing $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Info "configuring ($BuildType); the first run fetches dependencies and takes a few minutes"
    & cmake -B $BuildDir -S $RepoRoot -DCMAKE_BUILD_TYPE=$BuildType
    if ($LASTEXITCODE -ne 0) {
        Stop-WithError 'cmake configure failed.' @(
            'The output above says what is missing. To start from a clean slate:',
            '  .\scripts\run-windows.ps1 -Clean')
    }
}

if ($Force -or (Test-NeedsBuild)) {
    Write-Info 'building'
    if ($multiConfig) {
        & cmake --build $BuildDir --config $BuildType
    } else {
        & cmake --build $BuildDir -j
    }
    if ($LASTEXITCODE -ne 0) {
        Stop-WithError 'the build failed; see the compiler output above.'
    }
} else {
    Write-Info 'build is up to date, skipping'
}

if (-not (Test-Path $AppExe)) {
    Stop-WithError 'the build finished but no app was produced at:' @(
        "  $AppExe",
        'The GUI is skipped when SDL3 cannot be found or built. Re-run with -Clean,',
        "and check the configure output for 'SDL3 was not found'.")
}

# Not fatal: SPIR-V and the editor work without these. Only DXIL output needs them.
$appDir = Split-Path -Parent $AppExe
foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
    if (-not (Test-Path (Join-Path $appDir $dll))) {
        Write-Warn "$dll is not beside the executable, so builds cannot emit DXIL. SDL_shadercross ships it; a clean configure copies it into $appDir."
        break
    }
}

# --- launch ------------------------------------------------------------------
Write-Info 'launching SDL Shader Studio'
if ($AppArgs.Count -gt 0) {
    & $AppExe @AppArgs
} else {
    & $AppExe
}
exit $LASTEXITCODE
