param(
    [string]$Python = "python",
    [string]$BuildDirectory = "build/native-zig-release"
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
$toolRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot "build/native-zig-tools"))

& $Python -c "import cmake, ninja, ziglang" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "Missing build tools. Run: python -m pip install cmake ninja ziglang"
}

$cmakeDirectory = & $Python -c "import cmake; print(cmake.CMAKE_BIN_DIR)"
$ninjaDirectory = & $Python -c "import ninja; print(ninja.BIN_DIR)"
$zigPath = & $Python -c "import pathlib, ziglang; print(pathlib.Path(ziglang.__file__).parent / 'zig.exe')"
$cmakePath = Join-Path $cmakeDirectory "cmake.exe"
$ninjaPath = Join-Path $ninjaDirectory "ninja.exe"

New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
$arWrapper = Join-Path $toolRoot "zig-ar.cmd"
$ranlibWrapper = Join-Path $toolRoot "zig-ranlib.cmd"
$ccWrapper = Join-Path $toolRoot "zig-cc.cmd"
$cxxWrapper = Join-Path $toolRoot "zig-cxx.cmd"
[System.IO.File]::WriteAllText($arWrapper, "@echo off`r`n`"$zigPath`" ar %*`r`n", [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($ranlibWrapper, "@echo off`r`n`"$zigPath`" ranlib %*`r`n", [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($ccWrapper, "@echo off`r`n`"$zigPath`" cc -target x86_64-windows-gnu -mcpu=x86_64 %*`r`n", [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($cxxWrapper, "@echo off`r`n`"$zigPath`" c++ -target x86_64-windows-gnu -mcpu=x86_64 %*`r`n", [System.Text.Encoding]::ASCII)

& (Join-Path $PSScriptRoot "prepare_hesai_sdk.ps1")
& (Join-Path $PSScriptRoot "prepare_seyond_sdk.ps1")

$configure = @(
    "-S", (Join-Path $projectRoot "native"),
    "-B", $buildRoot,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath",
    "-DCMAKE_C_COMPILER=$ccWrapper",
    "-DCMAKE_CXX_COMPILER=$cxxWrapper",
    "-DCMAKE_AR=$arWrapper",
    "-DCMAKE_RANLIB=$ranlibWrapper",
    "-DCMAKE_CXX_COMPILER_AR=$arWrapper",
    "-DCMAKE_CXX_COMPILER_RANLIB=$ranlibWrapper",
    "-DCMAKE_SYSTEM_NAME=Windows",
    "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    "-DCMAKE_BUILD_TYPE=Release"
)
& $cmakePath @configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmakePath --build $buildRoot --config Release -j 4
if ($LASTEXITCODE -ne 0) { throw "Native plugin build failed." }

Write-Host "Native plugins are ready in $(Join-Path $projectRoot 'plugins')."
