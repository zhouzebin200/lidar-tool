set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)

set(CMAKE_C_COMPILER "${TOOLCHAIN_DIR}/zig-cc-win.sh")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_DIR}/zig-cxx-win.sh")
set(CMAKE_AR "${TOOLCHAIN_DIR}/zig-ar-win.sh")
set(CMAKE_RANLIB "${TOOLCHAIN_DIR}/zig-ranlib-win.sh")

set(CMAKE_C_STANDARD_LIBRARIES "-lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 -lws2_32 -lopengl32")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 -lws2_32 -lopengl32")
