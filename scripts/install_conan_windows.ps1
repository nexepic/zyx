$vsPath = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvarsall.bat`" x64 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}
$env:CC = "cl"
$env:CXX = "cl"
conan profile detect --force
New-Item -ItemType Directory -Force -Path buildDir
conan install . --output-folder=buildDir --build=missing -s build_type=Release -s compiler.cppstd=20
