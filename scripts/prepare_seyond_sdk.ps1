$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$sdkRoot = Join-Path $projectRoot "third_party\inno-lidar-sdk"
$baseFile = Join-Path $sdkRoot "build\inno_lidar_base.cmake"

if (-not (Test-Path -LiteralPath $baseFile)) {
    throw "Seyond Inno SDK is missing. Run scripts/fetch_sdks.ps1 first."
}

$content = [System.IO.File]::ReadAllText($baseFile)
$updated = $content.Replace(
    'if((CMAKE_SYSTEM_NAME NOT STREQUAL "Windows") OR (DEFINED MINGW))',
    'if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR DEFINED MINGW)'
)
if ($updated -ne $content) {
    [System.IO.File]::WriteAllText($baseFile, $updated, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Prepared Seyond SDK CMake syntax for current CMake."
} else {
    Write-Host "Seyond SDK CMake syntax is already prepared."
}
