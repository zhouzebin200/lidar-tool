# 砺见智行激光雷达测试工具

Windows x64 激光雷达点云查看、设备识别、PCAP 回放与检测报告工具，支持 Hesai、RoboSense、Huawei、Velodyne 和 Seyond。

## 直接使用

当前成品：[`dist/unlicensed-x64/Lijian-LidarTool-Windows-x64.exe`](dist/unlicensed-x64/Lijian-LidarTool-Windows-x64.exe)。

复制这一个 EXE 到 Windows 10/11 x64 电脑，双击并允许管理员权限。无需 Python、无需软件许可证；已内置网页、五厂商解码器、标定文件和 Npcap 1.90 离线安装向导。Npcap 安装仍需用户确认其官方协议，检测与安装过程不需要联网下载。旧发布包已删除。

## 从哪里开始

| 需要做什么 | 入口 |
| --- | --- |
| 运行源码 | `run.py` 或 `start.ps1` / `start.sh` |
| 构建当前无授权 EXE | `python packaging/build_unlicensed.py` |
| 构建授权版 | `python packaging/build_report.py` |
| 了解代码和维护方式 | [项目结构与维护](docs/项目结构与维护.md) |
| 查阅使用说明、配置和 API | [完整使用说明](docs/guides/完整使用说明.md) |
| 运行测试 | [测试说明](tests/README.md) |
| 查阅全部文档 | [文档目录](docs/README.md) |

## 目录分工

```text
ulv/          Python 服务、抓包、协议、设备和运行时逻辑
web/          点云、报告、网关的浏览器界面
native/       C++ 解码适配器源码和 C ABI
corrections/  随软件提供的标称标定
config/       静态型号配置与本机运行配置
plugins/      编译后的解码 DLL（生成物）
third_party/  厂商 SDK 源码（本地依赖）
vendor/       离线安装器等厂商二进制依赖
licensing/    授权管理源码和独立保管的私有数据
packaging/    发布构建、内容审计与 legacy/ 历史工具
scripts/      SDK 获取、原生编译、PCAP 验证工具
tests/        Python 回归；browser/ 浏览器测试；manual/ 手动工具
docs/         指南、设计、维护、历史记录和测试证据
build/        中间产物和测试临时文件（不交付）
dist/         当前交付文件（不存旧版本）
tmp/          本地临时文件（不交付）
```

源码运行需要 Python 3.10+。构建当前 Windows EXE 需要 Windows x64 Python、Nuitka 4.3、项目要求的解码 DLL 和官方 Npcap 安装器，详见 [打包说明](packaging/README.md)。

`licensing/private/`、签发记录、许可证和本机网络状态不属于客户交付内容。Npcap 免费版不支持静默安装；随产品对外分发需取得相应 OEM 权利，见 [Npcap 官方说明](https://npcap.com/oem/redist)。
