# EM4 ROS SDK 修复工作区

[完整编译、在线接入和抓包回放说明](README_EM4_%E4%B8%AD%E6%96%87.md)

- `src/rslidar_sdk`：用户提供 SDK 的修复源码，兼容旧格式并支持当前 EM4 压缩列及 DIFOP2。
- `src/rslidar_msg`：随 SDK 提供的 packet 消息定义。
- `scripts/configure_pcap.py`：生成抓包绝对路径配置。
- `scripts/test_core.sh`：无需 ROS 的驱动核心回归。
- `tests`、`docs/validation-summary.json`：测试源码与结果。

点云话题：`/EM4_rslidar_points`；frame_id：`rslidar`。ROS1 入口 `em4.launch`，ROS2 入口 `em4.launch.py`。抓包文件、设备标定二进制、虚拟环境和编译产物不纳入源码。

验证：12 项 EM4 测试和原 SDK 58 项测试通过；真实抓包输出 95 帧、36,392,507 个有效点，覆盖 520 通道。没有 ROS 环境下的节点运行结果，驱动核心回归不等同于 ROS 话题发布实测。
