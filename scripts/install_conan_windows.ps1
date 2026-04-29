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

# Remove CC and CXX variables from the current session so CMake doesn't try to use clang-cl
Remove-Item Env:\CC -ErrorAction Ignore
Remove-Item Env:\CXX -ErrorAction Ignore

Write-Host "Generating explicit MSVC Conan profile..."
# We explicitly set the generator to Visual Studio 17 2022 so CMake completely
# ignores any remaining CC/CXX variables and uses MSBuild and cl.exe correctly.
$profile = @"
[settings]
os=Windows
arch=x86_64
compiler=msvc
compiler.version=194
compiler.cppstd=20
compiler.runtime=dynamic
[conf]
tools.cmake.cmaketoolchain:generator=Visual Studio 17 2022
"@
$profile | Out-File -FilePath "conan_msvc_profile" -Encoding utf8

# Check if build_type argument was passed to script, default to Release
$buildType = "Release"
if ($args.Count -gt 0) {
    $buildType = $args[0]
}

Write-Host "Running Conan install with build_type=$buildType..."
New-Item -ItemType Directory -Force -Path buildDir | Out-Null
conan install . --output-folder=buildDir --build=missing -s build_type=$buildType --profile:build conan_msvc_profile --profile:host conan_msvc_profile

Write-Host "Conan install completed."
