# EM4 ROS SDK 修复版

本包从用户提供的 EM4_SDK.7z 源码制作。包含 ROS1/ROS2 的 rslidar_sdk、所需 packet 消息包以及 EM4 回归测试；不包含旧 build/devel 目录、Mac 可执行程序或抓包文件。

## 已修改

- EM4 直接解码当前抓包的新版压缩块，不再依赖网页适配器。
- 接收 1162 字节、A5 FF 00 AE 标记的 DIFOP2。默认等待标定，收到后开始输出。
- 按 v1.1 手册表 10 读取 26 个有符号单字节 yaw 修正，每 20 通道一组；读取 520 个俯仰角和 4 个镜面偏移。
- 保留 1084 字节固定格式、旧压缩格式和旧 1310 字节标定输入；校验压缩边界及匹配分片。
- 去掉示例中的第二台 EMX；点云统一发布到 `/EM4_rslidar_points`，frame_id 为 `rslidar`，RViz 配置同步修改。
- 默认使用 XYZIRT 点结构，保留 ring、timestamp；ROS2 安装时一并安装 config。

## 1. Ubuntu 编译

先加载已安装的 ROS 环境。首次使用时可通过 rosdep 安装本工作区声明的依赖：

```bash
cd EM4_ROS_fixed
rosdep install --from-paths src --ignore-src -r -y
```

ROS1，例如已加载 Noetic 环境：

```bash
catkin_make -DCMAKE_BUILD_TYPE=Release
source devel/setup.bash
```

ROS2，例如已加载 Humble 环境：

```bash
colcon build --symlink-install --cmake-args -DCMAKE_BUILD_TYPE=Release
source install/setup.bash
```

源码内保留原厂许可证。新增 rslidar_msg 直接使用原 SDK 随附的 RslidarPacket.msg，避免另行寻找消息依赖。

## 2. 回放这次的 EM4.pcapng

在 Ubuntu 上把抓包放在任意位置，运行脚本生成具有绝对路径的配置：

```bash
cd EM4_ROS_fixed
python3 scripts/configure_pcap.py /你的实际路径/EM4.pcapng
```

生成工作区根目录的 `em4_replay.yaml`。脚本同时支持经典 PCAP；libpcap 负责读取原始 PCAPNG，无需转换副本。

ROS1：

```bash
roslaunch rslidar_sdk em4.launch config_path:="$(pwd)/em4_replay.yaml"
```

ROS2：

```bash
ros2 launch rslidar_sdk em4.launch.py config_path:="$(pwd)/em4_replay.yaml"
```

本抓包配置已设置：MSOP 34870、DIFOP2 34872、`use_vlan: true`。抓包内含 VLAN 129；DIFOP1/状态端口 34869 不承担点云角度标定。输入先等待首个有效 DIFOP2，可能在开始的一小段时间没有点云；循环回放继续保留已接收标定。回放速率沿用原 SDK 的逐包延时机制，不保证按抓包原始时间戳精确恢复 10 Hz。

不打开 RViz 时，ROS1 加 `rviz:=false`，ROS2 加 `rviz:=false`。

## 3. 在线连接这台雷达

```bash
roslaunch rslidar_sdk em4.launch
# 或
ros2 launch rslidar_sdk em4.launch.py
```

默认加载 `config/em4_online.yaml`，按这份抓包设置为组播 239.31.129.1、MSOP 34870、DIFOP2 34872，主机绑定 0.0.0.0。若雷达实际配置不同，修改这些参数。主机需要接入对应的 VLAN/子网，并能接收雷达组播；在线 UDP socket 收到的是操作系统剥离以太网/VLAN 头后的负载，因此在线配置 `use_vlan: false`。抓包回放配置的 `use_vlan: true` 不能用来替代操作系统的 VLAN 网卡配置。

RViz 中 Fixed Frame 使用 `rslidar`，PointCloud2 Topic 使用 `/EM4_rslidar_points`。两个随包 RViz 配置已设好。

查看话题：

```bash
rostopic hz /EM4_rslidar_points
# 或
ros2 topic hz /EM4_rslidar_points
```

## 4. 已完成的验证与边界

当前主机是 macOS，没有安装 ROS。以下均为真实执行结果：

- 12 项 EM4 回归测试通过，启用 AddressSanitizer/UndefinedBehaviorSanitizer。
- 原 SDK 的 58 项测试全部通过。
- 原始 EM4.pcapng 经原厂 LidarDriver、libpcap、VLAN/端口过滤、DIFOP2、EM4 解码器和点云帧回调，读取 157,556 个 MSOP 和 5 个 DIFOP2。
- 输出 95 帧、36,392,507 个有效点，覆盖 ring 0–519，队列溢出 0，非有限点 0。首个 DIFOP2 前的 MSOP 因等待标定而跳过，文件末尾尚未封闭的帧不输出。

ROS1/ROS2 节点和 DDS/ROS 话题发布尚未在本机执行；不能把上述驱动核心验证等同于 ROS 实机发布验证。分片支持已通过合成数据测试，本抓包全部为未分片的压缩列。

无需 ROS 的复测方法（编译器、libpcap 开发包需已安装）：

```bash
sh scripts/test_core.sh /你的实际路径/EM4.pcapng
```

完整记录见 `tests/after-fix.log`、`tests/upstream-results.log`、`tests/real-pcap-report.json`。

## 手册核对

用户提供的 RS-EM4_UserGuide_v1.1_EN.pdf：PDF 第 22–23 页说明 0x70 型号、520 通道和 5 mm 距离单位；第 28 页表 10 定义 DIFOP2 字段。该版本对压缩包的具体数据块说明不完整，新版压缩结构依据当前厂商公开手册和真实抓包核验。手册前段所称单一 1310 字节 DIFOP 与后面的 DIFOP1/DIFOP2 表格并不完全一致，因此代码分别按完整标记与长度识别，不能只认旧 DIFOP。

本次修改是上述 ROS 工作区副本；原始 EM4_SDK.7z、PDF 和抓包均未修改。
