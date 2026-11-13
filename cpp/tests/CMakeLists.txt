cmake_minimum_required(VERSION 3.16)
project(UlvTestSuite VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TEST_SOURCES
    test_main.cpp
    test_cli_headless.cpp
    test_native_file_dialog.cpp
    test_udp_ingestion.cpp
    test_ptc_protocol.cpp
    test_ptc_adversarial_challenge.cpp
    test_report_html.cpp
    test_image_encoder.cpp
    test_image_encoder_adversarial.cpp
    test_viewport_capture.cpp
    test_audit_fixes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/net/pcap_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/net/detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/net/udp_receiver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/net/ptc_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/ptp_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/decoder_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/report_generator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/image_encoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/pointcloud_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/grid_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer/viewport_capture.cpp
)

if(NOT APPLE)
    list(APPEND TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/glad/src/gl.c)
endif()

add_executable(ulv_test_suite ${TEST_SOURCES})

target_include_directories(ulv_test_suite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/glad/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../native/include
)

if(APPLE)
    target_link_libraries(ulv_test_suite PRIVATE pthread "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(ulv_test_suite PRIVATE ws2_32 opengl32)
else()
    target_link_libraries(ulv_test_suite PRIVATE pthread GL dl)
endif()

set_target_properties(ulv_test_suite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Headless ImGui regression checks: real workspace, no desktop or GL context.
set(UI_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui)
add_executable(ulv_ui_tests
    test_ui_workspace.cpp
    ../src/ui/ui_workspace.cpp ../src/ui/ui_theme.cpp ../src/ui/file_dialog.cpp
    ../src/net/pcap_reader.cpp ../src/net/ptc_client.cpp
    ../src/core/report_generator.cpp ../src/renderer/image_encoder.cpp
    ${UI_IMGUI_DIR}/imgui.cpp ${UI_IMGUI_DIR}/imgui_draw.cpp
    ${UI_IMGUI_DIR}/imgui_widgets.cpp ${UI_IMGUI_DIR}/imgui_tables.cpp)
target_compile_definitions(ulv_ui_tests PRIVATE ULV_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(ulv_ui_tests PRIVATE . ../include ../src ../src/ui ../src/net ../src/core ../src/renderer ../third_party/stb ../third_party/glad/include ${UI_IMGUI_DIR})
if(APPLE)
    set_source_files_properties(../src/ui/file_dialog.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++")
    target_link_libraries(ulv_ui_tests PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
elseif(WIN32)
    target_link_libraries(ulv_ui_tests PRIVATE ws2_32 comdlg32 shell32 ole32)
else()
    target_link_libraries(ulv_ui_tests PRIVATE pthread)
endif()
set_target_properties(ulv_ui_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
