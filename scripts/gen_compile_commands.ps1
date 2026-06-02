#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Generate compile_commands.json for GingerSlicer (Windows / PowerShell).

.DESCRIPTION
    Runs a CMake configure step with CMAKE_EXPORT_COMPILE_COMMANDS=ON so
    clangd, Serena MCP, and other LSP-driven tools can resolve C++ symbols
    accurately on this template-heavy codebase.

    The generated `build/compile_commands.json` is copied to the repo root
    where clangd looks for it by default.

    This script does NOT build the project. It only configures CMake.

.PARAMETER BuildDir
    Build directory. Defaults to `build-cdb` to avoid clashing with the
    Visual Studio build under `build/`. Visual Studio's generator does not
    emit compile_commands.json, so a separate Ninja-based directory is
    required.

.PARAMETER Config
    CMake build type. Defaults to RelWithDebInfo (best for navigation +
    reasonable optimisation when actually building).

.PARAMETER Generator
    CMake generator. Defaults to "Ninja" when available, falls back to the
    default Visual Studio generator.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/gen_compile_commands.ps1
    # (or `pwsh -File ...` if PowerShell 7 is installed; many Windows boxes
    #  only have Windows PowerShell 5.1, where the command is `powershell`.)

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/gen_compile_commands.ps1 -BuildDir build-clangd -Config Debug

.NOTES
    Requires the OrcaSlicer/GingerSlicer dependencies to be already built
    (Windows convention: `deps/build/OrcaSlicer_dep/usr/local`). If you
    have not built them yet, run `build_release_vs2022.bat -d` first.

    The MSVC Developer environment is auto-loaded via Launch-VsDevShell.ps1
    when cl.exe is not in PATH, so the script works from a plain PowerShell.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build-cdb",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "RelWithDebInfo",
    [string]$Generator = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repoRoot

Write-Host "Repo root : $repoRoot"
Write-Host "Build dir : $BuildDir"
Write-Host "Config    : $Config"

# Ninja is required: Visual Studio's generator does not produce
# compile_commands.json. Default to Ninja and fail fast if missing.
if (-not $Generator) {
    $Generator = "Ninja"
}
if ($Generator -eq "Ninja" -and -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw "Ninja is required to generate compile_commands.json but was not found in PATH. Install via 'winget install Ninja-build.Ninja' or 'choco install ninja'."
}

# Load Visual Studio Developer environment (cl.exe, INCLUDE, LIB) so Ninja can
# find the MSVC toolchain. No-op when already inside a Developer Shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "cl.exe not in PATH; loading VS Developer environment..."
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio 2022 Build Tools (Desktop C++)."
    }
    $vsInstall = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsInstall) {
        throw "No Visual Studio installation with MSVC C++ toolchain found."
    }
    $devShell = Join-Path $vsInstall "Common7\Tools\Launch-VsDevShell.ps1"
    if (-not (Test-Path $devShell)) {
        throw "Launch-VsDevShell.ps1 not found under $vsInstall."
    }
    & $devShell -SkipAutomaticLocation -Arch amd64 -HostArch amd64 | Out-Null
    Set-Location $repoRoot
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Failed to load MSVC environment (cl.exe still missing)."
    }
    Write-Host "MSVC environment loaded from $vsInstall"
}

$cmakeArgs = @(
    "-S", "."
    "-B", $BuildDir
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-DCMAKE_BUILD_TYPE=$Config"
)

if ($Generator) {
    $cmakeArgs += @("-G", $Generator)
    Write-Host "Generator : $Generator"
}

# Hint CMake at the dependencies (matches build_release_vs2022.bat which sets
# CMAKE_PREFIX_PATH=%DEPS%/usr/local where DEPS=deps/build/OrcaSlicer_dep).
$candidates = @(
    (Join-Path $repoRoot "deps/build/OrcaSlicer_dep/usr/local"),     # build_release_vs2022.bat convention
    (Join-Path $repoRoot "deps/build/destdir/usr/local")             # POSIX/CI convention
)
$depsPrefix = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($depsPrefix) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$depsPrefix"
    Write-Host "Deps dir  : $depsPrefix"
}
else {
    Write-Warning "Dependencies not found. Run 'build_release_vs2022.bat -d' once before this script."
    Write-Warning "Searched: $($candidates -join '; ')"
}

# Additional flags to mirror the slicer build (Windows SDK, public release).
if ($env:WindowsSdkDir -and $env:WindowsSDKVersion) {
    $sdkInclude = Join-Path $env:WindowsSdkDir "Include\$env:WindowsSDKVersion\"
    $cmakeArgs += "-DWIN10SDK_PATH=$sdkInclude"
}
$cmakeArgs += @("-DBBL_RELEASE_TO_PUBLIC=1", "-DORCA_TOOLS=ON")

Write-Host "`nRunning: cmake $($cmakeArgs -join ' ')`n"
& cmake @cmakeArgs

$generated = Join-Path $repoRoot "$BuildDir/compile_commands.json"
if (-not (Test-Path $generated)) {
    throw "compile_commands.json was not generated at $generated. Check CMake output above."
}

# Copy to repo root so clangd / Serena pick it up without extra config
$rootCopy = Join-Path $repoRoot "compile_commands.json"
Copy-Item -Force $generated $rootCopy
Write-Host "`nGenerated : $generated"
Write-Host "Copied to : $rootCopy"
Write-Host "`nDone. Clangd / Serena MCP can now resolve C++ symbols."
