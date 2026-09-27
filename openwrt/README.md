# OpenWrt 雷达网关版

该版本在路由器上执行二层自动发现、VLAN/QinQ 解析、ARP 引导、UDP 转发、USB PCAP 录制和 PTP 管理。它不携带体积较大的厂商点云解码动态库；PCAP 可直接交给电脑端 Universal LiDAR Viewer/Wireshark。

## 安装

先安装运行依赖和 USB 挂载支持：

```sh
opkg update
opkg install python3 libpcap linuxptp block-mount kmod-usb-storage
opkg install kmod-fs-ext4 e2fsprogs
```

FAT/exFAT/NTFS U 盘应改装对应文件系统包。建议使用 ext4，持续高速录包的稳定性和写入性能更好。

上传 `ulv-gateway_*.ipk` 后安装：

```sh
opkg install /tmp/ulv-gateway_*.ipk
/etc/init.d/ulv-gateway enable
/etc/init.d/ulv-gateway restart
```

浏览器访问 `http://路由器LAN地址:17777`，打开“网关 / PTP”选项卡。服务由 procd 托管，关闭网页不会停止转发、录包或 PTP。

安装后可运行 `ulv-check` 输出主 SoC 架构、内存、USB 挂载以及各网口 PHC/PTP 能力。`MT7961` 常见于 MediaTek 无线固件名称，不能单独代表运行 OpenWrt 的主 CPU，因此仍应以该命令和 `ubus call system board` 的结果为准。

HTTP 地址可在 `/etc/config/ulv-gateway` 修改。转发、录包和 PTP 参数由网页保存到 `/etc/ulv-gateway/universal-lidar-viewer/`。

## USB

程序只允许把 PCAP 写入 `/mnt`、`/media`、mountd 目录或非根文件系统的块设备挂载点，避免误写路由器内部闪存。先通过 OpenWrt `block-mount` 挂载 U 盘，再在网页选择挂载点。拔 U 盘前必须点击“停止”。

## PTP

- 主时钟：路由器向雷达发布 PTP。路由器系统时间应先由 NTP、GNSS 或其他可靠来源校准。
- 从时钟：路由器跟随网络中的上游 PTP 主时钟。
- 自动时间戳：网卡有 PHC 时使用硬件时间戳并启动 `phc2sys`；否则使用软件时间戳。
- L2 模式直接发送 IEEE 802.3 PTP，UDPv4 使用 PTP 事件/通用端口。PTP Domain、E2E/P2P 必须与雷达配置一致。

PTP 硬件时间戳是否可用由网卡驱动决定。交换芯片 CPU 口、软件 bridge 和部分 USB 网卡通常没有 PHC；此时应选择物理网口或软件时间戳。

## 构建 IPK

在项目根目录运行：

```powershell
python packaging/build_openwrt_gateway.py
```

输出位于 `dist/openwrt/`。网关包是纯 Python，架构标记为 `all`；它仍要求目标固件的软件源提供兼容的 Python、libpcap 和 linuxptp。
