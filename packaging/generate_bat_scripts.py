#!/usr/bin/env python3
"""Generate clean, reliable Windows batch scripts with guaranteed CRLF and proper escaping."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORTABLE = ROOT / "dist" / "Lijian-LidarTool-Portable"

SCRIPTS = {
    "一键启动.bat": """@echo off
chcp 65001 >nul
title 砺见智行激光雷达测试工具 - 启动器
cd /d "%~dp0"

echo 正在启动 砺见智行激光雷达测试工具...
start "" "%~dp0砺见智行激光雷达测试工具.exe"
exit
""",

    "创建桌面快捷方式.bat": """@echo off
chcp 65001 >nul
title 砺见智行激光雷达测试工具 - 创建快捷方式
cd /d "%~dp0"

echo ========================================================
echo       砺见智行激光雷达测试工具 - 快捷方式设置
echo ========================================================
echo.
echo 正在创建桌面和开始菜单快捷方式...

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$dir = '%~dp0'.TrimEnd('\\'); $target = Join-Path $dir '砺见智行激光雷达测试工具.exe'; $wsh = New-Object -ComObject WScript.Shell; $desktop = [Environment]::GetFolderPath('Desktop'); if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path $desktop)) { $desktop = $wsh.SpecialFolders.Item('Desktop') }; if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path $desktop)) { $desktop = Join-Path $env:USERPROFILE 'Desktop' }; if (-not (Test-Path $desktop)) { $desktop = 'C:\\Users\\Public\\Desktop' }; $lnk = Join-Path $desktop '砺见智行激光雷达测试工具.lnk'; $sc = $wsh.CreateShortcut($lnk); $sc.TargetPath = $target; $sc.WorkingDirectory = $dir; $sc.Description = '砺见智行激光雷达测试工具与报告工作台'; $sc.Save(); Write-Host (' [√] 桌面快捷方式已创建：' + $lnk) -ForegroundColor Green; $prog = [Environment]::GetFolderPath('Programs'); if ([string]::IsNullOrWhiteSpace($prog) -or -not (Test-Path $prog)) { $prog = $wsh.SpecialFolders.Item('Programs') }; if ([string]::IsNullOrWhiteSpace($prog) -or -not (Test-Path $prog)) { $prog = Join-Path $env:APPDATA 'Microsoft\\Windows\\Start Menu\\Programs' }; if (Test-Path $prog) { $menuLnk = Join-Path $prog '砺见智行激光雷达测试工具.lnk'; $scMenu = $wsh.CreateShortcut($menuLnk); $scMenu.TargetPath = $target; $scMenu.WorkingDirectory = $dir; $scMenu.Description = '砺见智行激光雷达测试工具与报告工作台'; $scMenu.Save(); Write-Host (' [√] 开始菜单快捷方式已创建：' + $menuLnk) -ForegroundColor Green; }"

echo.
echo ========================================================
echo 快捷方式已成功创建，您现在可以直接从桌面双击图标启动！
echo ========================================================
timeout /t 3 >nul 2>&1
exit /b 0
""",

    "移除桌面快捷方式.bat": """@echo off
chcp 65001 >nul
title 砺见智行激光雷达测试工具 - 移除快捷方式
cd /d "%~dp0"

echo 正在清理桌面和开始菜单快捷方式...

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$wsh = New-Object -ComObject WScript.Shell; $desktop = [Environment]::GetFolderPath('Desktop'); if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path $desktop)) { $desktop = $wsh.SpecialFolders.Item('Desktop') }; if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path $desktop)) { $desktop = Join-Path $env:USERPROFILE 'Desktop' }; if (-not (Test-Path $desktop)) { $desktop = 'C:\\Users\\Public\\Desktop' }; $lnk = Join-Path $desktop '砺见智行激光雷达测试工具.lnk'; if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Host (' [√] 已移除桌面快捷方式：' + $lnk) -ForegroundColor Green; } else { Write-Host ' [i] 桌面快捷方式未发现或已移除。' -ForegroundColor Gray; }; $prog = [Environment]::GetFolderPath('Programs'); if ([string]::IsNullOrWhiteSpace($prog) -or -not (Test-Path $prog)) { $prog = $wsh.SpecialFolders.Item('Programs') }; if ([string]::IsNullOrWhiteSpace($prog) -or -not (Test-Path $prog)) { $prog = Join-Path $env:APPDATA 'Microsoft\\Windows\\Start Menu\\Programs' }; if (Test-Path $prog) { $menuLnk = Join-Path $prog '砺见智行激光雷达测试工具.lnk'; if (Test-Path $menuLnk) { Remove-Item $menuLnk -Force; Write-Host (' [√] 已移除开始菜单快捷方式：' + $menuLnk) -ForegroundColor Green; } else { Write-Host ' [i] 开始菜单快捷方式未发现或已移除。' -ForegroundColor Gray; }; }"

echo.
echo ========================================================
echo 快捷方式已清理完毕。绿色便携文件夹直接删除即可。
echo ========================================================
timeout /t 3 >nul 2>&1
exit /b 0
""",

    "网络配置恢复.bat": """@echo off
chcp 65001 >nul
title 砺见智行激光雷达测试工具 - 网卡配置恢复工具
cd /d "%~dp0"

echo ========================================================
echo       砺见智行激光雷达测试工具 - 网卡配置恢复
echo ========================================================
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$dir = '%~dp0'.TrimEnd('\\'); $recoveryFile = Join-Path $dir 'config\\network-recovery.json'; if (-not (Test-Path $recoveryFile)) { Write-Host ' [√] 当前未发现残留的网卡恢复日志（网络状态正常）。' -ForegroundColor Green; exit 0; }; Write-Host ' [*] 检测到网络恢复日志，正在还原网卡设置...' -ForegroundColor Yellow; try { $doc = Get-Content $recoveryFile -Raw -Encoding UTF8 | ConvertFrom-Json; $adapters = $doc.adapters; $allSuccess = $true; foreach ($rec in $adapters) { $guid = $rec.guid; $adapter = Get-NetAdapter | Where-Object { $_.InterfaceGuid.Trim('{}') -eq $guid } | Select-Object -First 1; if ($null -eq $adapter) { Write-Host (' [!] 未找到网卡 GUID: ' + $guid) -ForegroundColor Yellow; $allSuccess = $false; continue; }; Write-Host (' [*] 正在恢复网卡: ' + $adapter.Name); if ($rec.requested_ip -and ($rec.original_ipv4 -notcontains $rec.requested_ip)) { Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -eq $rec.requested_ip } | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue; }; if ($rec.vlan_changed) { if ($rec.priority_keyword) { Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword $rec.priority_keyword -RegistryValue $rec.original_priority -NoRestart -ErrorAction SilentlyContinue; }; if ($rec.vlan_keyword) { Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword $rec.vlan_keyword -RegistryValue $rec.original_vlan -NoRestart -ErrorAction SilentlyContinue; }; Restart-NetAdapter -Name $adapter.Name -Confirm:$false -ErrorAction SilentlyContinue; }; if ($rec.dhcp_enabled) { Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -Dhcp Enabled -ErrorAction SilentlyContinue; }; Write-Host (' [√] 网卡 ' + $adapter.Name + ' 已还原') -ForegroundColor Green; }; if ($allSuccess) { Remove-Item -Path $recoveryFile -Force; Write-Host ' [√] 全部网卡已恢复完毕，恢复日志已清理。' -ForegroundColor Green; } else { Write-Host ' [!] 部分网卡恢复未完成，已保留恢复日志。' -ForegroundColor Yellow; }; } catch { Write-Host (' [!] 恢复过程中出现错误: ' + $_.Exception.Message) -ForegroundColor Red; Write-Host ' [*] 为确保配置凭据安全，已保留恢复日志文件。' -ForegroundColor Yellow; }"

echo.
echo ========================================================
echo 操作完成。
echo ========================================================
timeout /t 3 >nul 2>&1
exit /b 0
"""
}

def generate_scripts() -> None:
    PORTABLE.mkdir(parents=True, exist_ok=True)
    for name, content in SCRIPTS.items():
        p = PORTABLE / name
        crlf_content = content.replace("\r\n", "\n").replace("\n", "\r\n")
        p.write_bytes(crlf_content.encode("utf-8"))
        print(f"Generated {p.name}: {len(crlf_content)} bytes (CRLF)")

if __name__ == "__main__":
    generate_scripts()
