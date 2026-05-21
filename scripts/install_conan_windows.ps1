$ErrorActionPreference = "Stop"

$vsPath = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if (-not $vsPath) {
    Write-Error "Visual Studio not found."
}

Write-Host "Activating MSVC environment..."
cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvarsall.bat`" x64 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

# GUARANTEE CC and CXX point to clang-cl for Conan profile detection
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
if ((Test-Path $llvmBin) -and ($env:PATH -notlike "*$llvmBin*")) {
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
compiler.version=194
compiler.cppstd=20
compiler.runtime=dynamic

[conf]
tools.cmake.cmaketoolchain:generator=Ninja
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
