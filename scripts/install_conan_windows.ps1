$ErrorActionPreference = "Stop"

$vswherePath = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswherePath)) {
    Write-Error "Visual Studio Installer (vswhere.exe) was not found."
}

# Select the newest stable Visual Studio installation with the C++ toolchain.
$vsPath = (& $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$vsInstallationVersion = (& $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Trim()
if (-not $vsPath -or -not $vsInstallationVersion) {
    Write-Error "No Visual Studio installation with the C++ build tools was found."
}

try {
    $vsMajorVersion = ([version]$vsInstallationVersion).Major
} catch {
    Write-Error "Could not parse the Visual Studio version '$vsInstallationVersion'."
}

if ($vsMajorVersion -lt 17) {
    Write-Error "Visual Studio $vsInstallationVersion is unsupported; Visual Studio 2022 or newer is required."
}

Write-Host "Using Visual Studio $vsInstallationVersion at $vsPath"

Write-Host "Activating MSVC environment..."
$vcvarsallPath = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsallPath)) {
    Write-Error "Visual Studio C++ environment script was not found: $vcvarsallPath"
}

$vcvarsEnvironment = cmd /c "`"$vcvarsallPath`" x64 && set"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to activate the Visual Studio C++ environment."
}

$vcvarsEnvironment | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

try {
    $clPath = (Get-Command cl.exe -CommandType Application -ErrorAction Stop).Source
    $clVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($clPath)
} catch {
    Write-Error "Could not locate cl.exe in the activated Visual Studio environment."
}

if ($clVersion.FileMajorPart -lt 19 -or $clVersion.FileMinorPart -lt 0) {
    Write-Error "Unsupported cl.exe file version: $($clVersion.FileVersion)"
}

# Conan identifies cl 19.3x, 19.4x, 19.5x, ... as MSVC ABI 193, 194, 195, ... .
$msvcAbiVersion = [int](
    ($clVersion.FileMajorPart * 10) + [math]::Floor($clVersion.FileMinorPart / 10)
)
Write-Host "Detected cl.exe $($clVersion.FileVersion) at $clPath (Conan MSVC ABI $msvcAbiVersion)"

# Ensure the generated CMake toolchain uses clang-cl with the detected MSVC ABI.
$env:CC = "clang-cl"
$env:CXX = "clang-cl"

# Remove MinGW from PATH to prevent CMake from detecting mingw32-make.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*mingw*' }) -join ';'
Write-Host "Removed MinGW from PATH."

# Also remove VS-bundled LLVM from PATH so choco-installed LLVM takes precedence.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*VC\Tools\Llvm*' }) -join ';'
Write-Host "Removed VS-bundled LLVM from PATH."

# Ensure choco-installed LLVM is in PATH (for clang-cl, lld-link)
$llvmBin = "C:\Program Files\LLVM\bin"
if (-not (Test-Path (Join-Path $llvmBin "clang-cl.exe"))) {
    Write-Error "clang-cl was not found in $llvmBin. Install LLVM before running Conan."
}
if ($env:PATH -notlike "*$llvmBin*") {
    $env:PATH = "$llvmBin;$env:PATH"
    Write-Host "Added LLVM to PATH: $llvmBin"
}

$buildType = "Release"
$outputDir = if ($env:CONAN_OUTPUT_DIR) { $env:CONAN_OUTPUT_DIR } else { "buildDir" }
$extraArgs = @()

# Parse arguments: first is build type, remaining are extra Conan flags
for ($i = 0; $i -lt $args.Count; $i++) {
    if ($i -eq 0) {
        $buildType = $args[$i]
    } else {
        $extraArgs += $args[$i]
    }
}

Write-Host "Build type: $buildType"

# Write an explicit Conan profile file to avoid any PowerShell argument parsing issues.
# This is more reliable than passing -s flags via array expansion.
$profileContent = @"
[settings]
os=Windows
arch=x86_64
build_type=$buildType
compiler=msvc
compiler.version=$msvcAbiVersion
compiler.cppstd=20
compiler.runtime=dynamic

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
tools.microsoft.msbuild:vs_version=$vsMajorVersion
"@

$profilePath = "conan_windows_profile"
$profileContent | Out-File -FilePath $profilePath -Encoding utf8NoBOM
Write-Host "=== Written Conan profile ==="
Get-Content $profilePath
Write-Host "=== End profile ==="

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

Write-Host "Output directory: $outputDir"

# Use explicit profile file instead of command-line -s flags to ensure
# settings are applied correctly regardless of PowerShell argument handling.
conan install . --output-folder=$outputDir --build=missing --build="antlr4-cppruntime/*" --profile:host="$profilePath" --profile:build="$profilePath" @extraArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Conan install failed."
    exit $LASTEXITCODE
}

# Conan's msvc profile sets CMAKE_C/CXX_COMPILER to "cl" in the generated
# toolchain file as non-cache variables, which override any -D flags passed
# on the cmake command line.  Append clang-cl overrides so they take effect
# after Conan's own set() calls.
$toolchainFile = Join-Path $outputDir "conan_toolchain.cmake"
Add-Content -Path $toolchainFile -Value "`nset(CMAKE_C_COMPILER clang-cl)`nset(CMAKE_CXX_COMPILER clang-cl)"
Write-Host "Patched $toolchainFile to use clang-cl."

Write-Host "Conan install completed."
