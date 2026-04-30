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

# Force CMake to use Visual Studio generator for ALL cmake invocations.
# This is critical because tools.cmake.cmaketoolchain:generator only applies
# to Conan recipes using the new CMakeToolchain, but antlr4-cppruntime uses
# the legacy cmake helper which ignores that conf. Without this, CMake finds
# mingw32-make on PATH and picks "MinGW Makefiles", causing clang-cl + lld-link
# to fail with missing msvcrtd.lib.
$env:CMAKE_GENERATOR = "Visual Studio 17 2022"

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
    "-s", "compiler.runtime=dynamic"
)

& conan $conanArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Conan install failed."
    exit $LASTEXITCODE
}

Write-Host "Conan install completed."
