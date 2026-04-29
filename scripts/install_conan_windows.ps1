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

# Explicitly unset CC and CXX so Conan uses the default MSVC compiler (cl.exe)
# instead of picking up cibuildwheel's clang-cl environment variables.
$env:CC = ""
$env:CXX = ""

Write-Host "Generating explicit MSVC Conan profile..."
$profile = @"
[settings]
os=Windows
arch=x86_64
compiler=msvc
compiler.version=194
compiler.cppstd=20
compiler.runtime=dynamic
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
