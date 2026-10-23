# E2 网页点云查看器

E2 通过现有 RoboSense 插件直接解码，后端将点云送到浏览器；无需安装 ROS。

## 使用

重新编译 RoboSense 插件后，按项目原有方式运行 `python run.py`。旧版 DLL/dylib 不包含 E2，必须重新编译或使用本次构建的插件。

- 在线：让 E2 数据到达电脑网卡，沿用界面现有的网卡选择/发现流程。SDK 默认数据端口 6699、标定端口 7766；识别也支持自定义端口。
- 回放：在网页导入经典 Ethernet `.pcap`，自动识别为 `RSE2` 并显示点云。当前查看器不直接导入 `.pcapng`。
- 点云保留像素行号、强度及两个回波；距离单位为米。无效零距离、零方向数据不显示。
- 随 SDK 提供的默认方向表可用于初始显示。接收完整 360 个、CRC 正确且连续的标定包后使用设备标定；坏包或缺包不会替换已有标定。默认表不能替代设备专属精度校验。
- 标定更新和双回波输出是接入时的明确调整：厂家示例忽略第二回波，并在两次不同的完整标定传输后更新；本插件输出两个回波，并在一次完整且 CRC 通过的非全零标定后更新。
- 超过 1 秒的数据间隔、回放时间倒退会重置帧缓存；正常帧号回绕和迟到旧帧仍分别处理。
- 每个点云帧在收到下一帧时发出，因此离线文件最后一帧没有后继包时不会发出。低速/缺包录制可能只包含部分帧。

## 编译

沿用 `native/CMakeLists.txt` 和 `scripts/build_native_zig.ps1`。E2 必需的方向表、CRC 和许可证已随源码放在 `native/vendor/rse2`；其他速腾型号仍需要原 `rs_driver`（`scripts/fetch_sdks.ps1` 或指定 `RS_DRIVER_ROOT`）。

macOS 单独构建 RoboSense 插件：

```sh
mkdir -p plugins
c++ -std=c++17 -O2 -dynamiclib -pthread -DDISABLE_PCAP_PARSE \
  -include native/include/macos_socket_compat.h \
  -I native/include -I /path/to/rs_driver/src \
  native/plugins/robosense_adapter.cpp -o plugins/libulv_robosense.dylib
```

## 验证范围

`tests/e2_decoder_test.cpp` 覆盖默认表坐标、双回波、完整/损坏/缺包标定、重复与越界包、零距离/零方向。`tests/test_e2.py` 覆盖识别、同设备标定归组、原生插件到浏览器点格式、PCAP 完整回放链路。测试数据为合成协议数据，不是 E2 实机抓包。

```sh
c++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -I native/include -I native/plugins tests/e2_decoder_test.cpp -o /tmp/e2-test
/tmp/e2-test
python -m unittest discover -s tests -p test_e2.py -v
```

实机、真实 E2 PCAP、Windows DLL 尚未验证。本次仅接入 E2 网页查看器，不修改 `ros/em4`。
