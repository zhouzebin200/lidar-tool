$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$thirdPartyRoot = Join-Path $projectRoot "third_party"
New-Item -ItemType Directory -Path $thirdPartyRoot -Force | Out-Null

$rsPath = Join-Path $thirdPartyRoot "rs_driver"
$hesaiPath = Join-Path $thirdPartyRoot "HesaiLidar_SDK_2.0"
$seyondPath = Join-Path $thirdPartyRoot "inno-lidar-sdk"

if (-not (Test-Path -LiteralPath $rsPath)) {
    git clone --depth 1 https://github.com/RoboSense-LiDAR/rs_driver.git $rsPath
}
if (-not (Test-Path -LiteralPath $hesaiPath)) {
    git clone --depth 1 https://github.com/HesaiTechnology/HesaiLidar_SDK_2.0.git $hesaiPath
}
if (-not (Test-Path -LiteralPath $seyondPath)) {
    git clone --depth 1 https://github.com/Seyond-Inc/inno-lidar-sdk.git $seyondPath
}

Write-Output "SDK sources are ready in $thirdPartyRoot"
