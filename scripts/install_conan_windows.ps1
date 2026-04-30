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
# Conan 2 ignores the CMAKE_GENERATOR env var (it always passes -G explicitly),
# so the only reliable way to prevent MinGW interference is to remove it from PATH.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*mingw*' }) -join ';'
Write-Host "Removed MinGW from PATH."

# Generate base default profile so Conan doesn't fail
conan profile detect --force

$buildType = "Release"
if ($args.Count -gt 0) {
    $buildType = $args[0]
}

Write-Host "Running Conan install with build_type=$buildType and explicitly forced MSVC arguments..."
New-Item -ItemType Directory -Force -Path buildDir | Out-Null

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
    "-s", "compiler.cppstd=20",
    "-s", "compiler.runtime=dynamic",
    "-c", "tools.cmake.cmaketoolchain:generator=Visual Studio 17 2022"
)

& conan $conanArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Conan install failed."
    exit $LASTEXITCODE
}

Write-Host "Conan install completed."
