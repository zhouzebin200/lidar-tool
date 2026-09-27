# Native decoder adapters

The web service and renderer deliberately do not depend on a vendor point type.
Native RoboSense, Hesai and Velodyne adapters implement `include/ulv_decoder_plugin.h` and
emit `ulv_frame_v1` frames.

Adapter mapping:

- RoboSense: `plugins/robosense_adapter.cpp` instantiates `rs_driver::lidar::LidarDriver` with
  `InputType::RAW_PACKET`, feeds MSOP/DIFOP through `decodePacket()`, and probes
  candidate decoders when the shared packet signature cannot identify one model.
- Hesai: `plugins/hesai_adapter.cpp` instantiates `hesai::lidar::UdpParser`
  from the first `UdpPacket`, uses protocol-based model detection, decodes and
  compacts complete frames. It retrieves device-specific correction data over
  PTC (port 9347 by default), accepts local correction/firetimes paths, and
  assembles AT128 mirror fields using ranges from that correction data.
- Velodyne: `plugins/velodyne_adapter.cpp` implements only the raw 1206-byte
  packet, calibration and frame-splitting portions derived from
  `ros-drivers/velodyne`. It does not link ROS 2, PCL, rclcpp or yaml-cpp.

The host sends one UDP flow to one adapter instance. This avoids competing
socket bindings when several LiDARs share a destination port.

After fetching the SDK, build the first adapter with:

```sh
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release
```

On Windows, a self-contained Zig/Clang build is also provided:

```powershell
python -m pip install cmake ninja ziglang
./scripts/build_native_zig.ps1
```

The preparation script changes Hesai's `_MSC_VER` platform tests to the
compiler-neutral `_WIN32`, allowing the same source to build with MSVC,
clang-cl, MinGW, or Zig.
