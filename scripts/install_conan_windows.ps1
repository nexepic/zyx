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

# Remove MinGW from PATH to prevent CMake from detecting mingw32-make and
# selecting "MinGW Makefiles" generator. GitHub Actions windows-latest has
# C:\mingw64\bin on PATH by default, which interferes with MSVC builds.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*mingw*' }) -join ';'
Write-Host "Removed MinGW from PATH."

# Generate base default profile so Conan doesn't fail
conan profile detect --force

# Show detected profile for debugging
Write-Host "=== Detected Conan profile ==="
conan profile show
Write-Host "=== End profile ==="

$buildType = "Release"
if ($args.Count -gt 0) {
    $buildType = $args[0]
}

Write-Host "Running Conan install with build_type=$buildType..."
New-Item -ItemType Directory -Force -Path buildDir | Out-Null

# Use cppstd=17 to match ConanCenter prebuilt binaries for antlr4-cppruntime.
# Our project uses C++20 via its own build system (Meson native files), not via
# Conan's cppstd setting. antlr4 only requires C++17 and ConanCenter only
# provides cppstd=17 prebuilts. Using cppstd=20 forces a build from source
# which fails due to toolchain issues on GitHub Actions Windows runners.
$conanArgs = @(
    "install",
    ".",
    "--output-folder=buildDir",
    "--build=missing",
    "-s", "os=Windows",
    "-s", "arch=x86_64",
    "-s", "build_type=$buildType",
    "-s", "compiler=msvc",
    "-s", "compiler.version=194",
    "-s", "compiler.cppstd=17",
    "-s", "compiler.runtime=dynamic",
    "-c", "tools.cmake.cmaketoolchain:generator=Visual Studio 17 2022",
    "-vvv"
)

Write-Host "Conan args: $conanArgs"
& conan $conanArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Conan install failed."
    exit $LASTEXITCODE
}

Write-Host "Conan install completed."
