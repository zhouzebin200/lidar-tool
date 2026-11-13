cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Allow third-party SDKs with older cmake_minimum_required")
project(ulv_native_plugins LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hesai's logger uses the MSVC-only spelling even on other Windows C++
# compilers. Supply the equivalent Clang/GCC function signature macro.
if(WIN32 AND NOT MSVC)
  add_compile_definitions(__FUNCSIG__=__PRETTY_FUNCTION__)
endif()

set(RS_DRIVER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/rs_driver" CACHE PATH "Path to rs_driver")
set(HESAI_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/HesaiLidar_SDK_2.0" CACHE PATH "Path to Hesai SDK")
set(INNO_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/inno-lidar-sdk" CACHE PATH "Path to Seyond Inno SDK")
if(NOT EXISTS "${RS_DRIVER_ROOT}/src/rs_driver/api/lidar_driver.hpp")
  message(FATAL_ERROR "rs_driver not found. Run scripts/fetch_sdks.ps1 or set RS_DRIVER_ROOT.")
endif()

add_library(ulv_robosense SHARED plugins/robosense_adapter.cpp)
target_include_directories(ulv_robosense PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${RS_DRIVER_ROOT}/src"
)
target_compile_definitions(ulv_robosense PRIVATE DISABLE_PCAP_PARSE)
# rs_driver declares recvmmsg in its Linux socket backend even in RAW_PACKET mode.
if(APPLE)
  target_compile_options(ulv_robosense PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/include/macos_socket_compat.h")
endif()

if(WIN32)
  target_link_libraries(ulv_robosense PRIVATE ws2_32)
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(ulv_robosense PRIVATE Threads::Threads)
endif()

set_target_properties(ulv_robosense PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
)

# ros-drivers/velodyne's packet layout and calibration schema are implemented
# behind the same small ABI without linking ROS, PCL or yaml-cpp.
add_library(ulv_velodyne SHARED plugins/velodyne_adapter.cpp)
target_include_directories(ulv_velodyne PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
)
set_target_properties(ulv_velodyne PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
)

# Huawei L306/D3 uses a compact proprietary 192-channel packet captured from
# production hardware. This adapter has no external SDK dependency.
add_library(ulv_huawei_l306 SHARED plugins/huawei_l306_adapter.cpp)
target_include_directories(ulv_huawei_l306 PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
)
set_target_properties(ulv_huawei_l306 PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
)

if(EXISTS "${HESAI_SDK_ROOT}/libhesai/UdpParser/udp_parser.h")
  # The adapter only consumes the header-only UDP parser. Avoid pulling in the
  # SDK's network, serial, PTC and lidar-board clients; the host already owns
  # the UDP sockets and forwards raw datagrams through the plugin ABI.
  set(WITH_LIDARBOARD OFF CACHE BOOL "ULV supplies UDP packets" FORCE)
  set(WITH_UDPPARSER ON CACHE BOOL "Build the Hesai UDP parser" FORCE)
  set(WITH_SERIALCLIENT OFF CACHE BOOL "ULV does not use serial input" FORCE)
  set(WITH_PTCCLIENT ON CACHE BOOL "Fetch Hesai calibration over PTC" FORCE)
  set(WITH_LIDARLIB OFF CACHE BOOL "ULV uses UdpParser directly" FORCE)
  set(WITH_PTCS_USE OFF CACHE BOOL "The raw adapter does not open a PTC socket" FORCE)
  add_subdirectory("${HESAI_SDK_ROOT}/libhesai" "${CMAKE_BINARY_DIR}/hesai-lib" EXCLUDE_FROM_ALL)
  add_library(ulv_hesai SHARED plugins/hesai_adapter.cpp)
  target_include_directories(ulv_hesai PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${HESAI_SDK_ROOT}"
    "${HESAI_SDK_ROOT}/libhesai"
  )
  target_link_libraries(ulv_hesai PRIVATE udpParser_lib ptcClient_lib)
  if(APPLE)
    target_compile_options(ulv_hesai PRIVATE
      -include "${CMAKE_CURRENT_LIST_DIR}/include/macos_socket_compat.h")
    if(TARGET ptcClient_lib)
      target_compile_options(ptcClient_lib PRIVATE
        -include "${CMAKE_CURRENT_LIST_DIR}/include/macos_socket_compat.h")
    endif()
  endif()
  if(WIN32)
    target_link_libraries(ulv_hesai PRIVATE ws2_32)
  endif()
  set_target_properties(ulv_hesai PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
  )
else()
  message(WARNING "Hesai SDK not found; ulv_hesai will not be built.")
endif()

if(EXISTS "${INNO_SDK_ROOT}/src/sdk_common/inno_lidar_packet_utils.h")
  # Build the official packet conversion and control libraries. ULV keeps
  # ownership of UDP capture; the client control API is used only to fetch a
  # Robin/HB AngleHV table when compact packets require it.
  set(MAKE_SHARED OFF CACHE BOOL "Link Seyond SDK statically into the adapter" FORCE)
  add_subdirectory("${INNO_SDK_ROOT}/src" "${CMAKE_BINARY_DIR}/inno-sdk" EXCLUDE_FROM_ALL)
  set_target_properties(innolidarsdkclient innolidarsdkcommon innolidarutils PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/inno-sdk-lib"
  )
  add_library(ulv_seyond SHARED plugins/seyond_adapter.cpp)
  target_include_directories(ulv_seyond PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${INNO_SDK_ROOT}"
    "${INNO_SDK_ROOT}/src"
  )
  target_link_libraries(ulv_seyond PRIVATE innolidarsdkclient innolidarsdkcommon innolidarutils)
  if(WIN32)
    target_link_libraries(ulv_seyond PRIVATE ws2_32 wsock32)
    if(NOT MSVC)
      # Inno's UDP ABI uses GCC packed bitfields and has a 70-byte data
      # header. Clang targeting Windows otherwise enables MS bitfield layout,
      # expanding the header to 72 bytes and reading the packet type at the
      # wrong offset (Robin compact type 13 becomes type 0).
      foreach(inno_target ulv_seyond innolidarsdkclient innolidarsdkcommon innolidarutils)
        target_compile_options(${inno_target} PRIVATE
          -mno-ms-bitfields
          -include "${CMAKE_CURRENT_LIST_DIR}/include/win_zig_inno_compat.h")
      endforeach()
    endif()
  elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ulv_seyond PRIVATE Threads::Threads)
  endif()
  set_target_properties(ulv_seyond PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../plugins"
  )
else()
  message(WARNING "Seyond Inno SDK not found; ulv_seyond will not be built.")
endif()
