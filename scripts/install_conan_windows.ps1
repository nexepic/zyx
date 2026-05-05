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

# GUARANTEE CC and CXX are completely unset for child processes
[System.Environment]::SetEnvironmentVariable('CC', $null)
[System.Environment]::SetEnvironmentVariable('CXX', $null)
Remove-Item Env:\CC -ErrorAction Ignore
Remove-Item Env:\CXX -ErrorAction Ignore

# Remove MinGW from PATH to prevent CMake from detecting mingw32-make.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*mingw*' }) -join ';'
Write-Host "Removed MinGW from PATH."

# Also remove VS-bundled LLVM from PATH so cmake finds cl.exe, not clang-cl.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*VC\Tools\Llvm*' }) -join ';'
Write-Host "Removed VS-bundled LLVM from PATH."

$buildType = "Release"
$outputDir = "buildDir"
$extraArgs = @()

# Parse arguments: first is build type, then optional --output-folder=PATH,
# remaining are extra Conan flags
for ($i = 0; $i -lt $args.Count; $i++) {
    if ($i -eq 0) {
        $buildType = $args[$i]
    } elseif ($args[$i] -match '^--output-folder=(.+)$') {
        $outputDir = $matches[1]
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
tools.cmake.cmaketoolchain:generator=Visual Studio 17 2022
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
conan install . --output-folder=$outputDir --build=missing --profile:host="$profilePath" --profile:build="$profilePath" @extraArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Conan install failed."
    exit $LASTEXITCODE
}

Write-Host "Conan install completed."
