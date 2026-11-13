cmake_minimum_required(VERSION 3.16)
project(LijianLidarViewer VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw)

# Glad OpenGL loader
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad")
set(GLAD_SOURCES "${GLAD_DIR}/src/gl.c")

# ImGui sources
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Application sources
set(APP_SOURCES
    src/main.cpp
    src/renderer/pointcloud_renderer.cpp
    src/renderer/grid_renderer.cpp
    src/renderer/image_encoder.cpp
    src/renderer/viewport_capture.cpp
    src/ui/ui_workspace.cpp
    src/ui/ui_theme.cpp
    src/ui/file_dialog.cpp
    src/net/pcap_reader.cpp
    src/net/udp_receiver.cpp
    src/net/detector.cpp
    src/net/ptc_client.cpp
    src/core/decoder_manager.cpp
    src/core/ptp_monitor.cpp
    src/core/report_generator.cpp
)

set(ALL_SOURCES ${APP_SOURCES} ${IMGUI_SOURCES})
if(NOT APPLE)
    list(APPEND ALL_SOURCES ${GLAD_SOURCES})
endif()

add_executable(ulv_viewer ${ALL_SOURCES})

target_include_directories(ulv_viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../native/include
    ${GLAD_DIR}/include
)

target_link_libraries(ulv_viewer PRIVATE glfw)

if(APPLE)
    target_link_libraries(ulv_viewer PRIVATE
        "-framework OpenGL"
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework UniformTypeIdentifiers"
    )
    set_source_files_properties(src/ui/file_dialog.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++")
elseif(WIN32)
    target_link_libraries(ulv_viewer PRIVATE opengl32 ws2_32 gdi32 user32 shell32 comdlg32 ole32)
    target_link_options(ulv_viewer PRIVATE "-Wl,--subsystem,windows")
else()
    target_link_libraries(ulv_viewer PRIVATE GL dl pthread)
endif()

# Output binary to build root
set_target_properties(ulv_viewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET ulv_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:ulv_viewer>/resources"
    COMMENT "Copying shared UI fonts and license...")

if(APPLE)
    set(MACOS_BUNDLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../砺见智行激光雷达测试工具.app")
    file(GLOB MACOS_DYLIBS "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/*.dylib")
    set(BUNDLE_PLUGIN_COPY_CMD "")
    if(MACOS_DYLIBS)
        set(BUNDLE_PLUGIN_COPY_CMD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MACOS_DYLIBS} "${MACOS_BUNDLE_DIR}/Contents/Resources/plugins/")
    endif()
    add_custom_command(TARGET ulv_viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MACOS_BUNDLE_DIR}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MACOS_BUNDLE_DIR}/Contents/Resources/plugins"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MACOS_BUNDLE_DIR}/Contents/Resources/corrections"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ulv_viewer> "${MACOS_BUNDLE_DIR}/Contents/MacOS/ulv_viewer"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${MACOS_BUNDLE_DIR}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../corrections" "${MACOS_BUNDLE_DIR}/Contents/Resources/corrections"
        ${BUNDLE_PLUGIN_COPY_CMD}
        COMMENT "Updating 砺见智行激光雷达测试工具.app bundle..."
    )
endif()
