param(
    [string]$SdkRoot = (Join-Path $PSScriptRoot "..\third_party\HesaiLidar_SDK_2.0")
)

$resolvedRoot = [System.IO.Path]::GetFullPath($SdkRoot)
if (-not (Test-Path -LiteralPath (Join-Path $resolvedRoot "libhesai"))) {
    throw "HesaiLidar_SDK_2.0 not found at $resolvedRoot. Run fetch_sdks.ps1 first."
}

# Hesai SDK 2.0 uses _MSC_VER as a Windows platform switch in its source.
# _WIN32 is the correct platform macro and also works with MSVC, clang-cl,
# MinGW and Zig. Keep this source preparation step deterministic/idempotent.
$extensions = @("*.h", "*.hpp", "*.c", "*.cc", "*.cpp")
$changed = 0
Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Include $extensions | ForEach-Object {
    $path = $_.FullName
    $content = [System.IO.File]::ReadAllText($path)
    if ($content.Contains("_MSC_VER")) {
        $content = $content.Replace("_MSC_VER", "_WIN32")
        [System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
        $changed++
    }
}

Write-Host "Prepared Hesai SDK for portable Windows compilation: $changed file(s) changed."
