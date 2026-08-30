cmake_minimum_required(VERSION 3.15)
project(x4_live_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

# SDK 根目录（可用 -D 覆盖）
set(SDK_PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../赛事SDK包（Windows+Linux）/Windows_CameraSDK-2.1.1_MediaSDK-3.1.3/Windows_CameraSDK-2.1.1_MediaSDK-3.1.3" CACHE PATH "SDK 包根目录")
set(CAMERA_SDK_ROOT "${SDK_PACKAGE_ROOT}/CameraSDK-20250812_192505-2.1.1-win64_1754998240815/CameraSDK-20250812_192505-2.1.1-win64" CACHE PATH "CameraSDK 目录")
set(MEDIA_SDK_ROOT "${SDK_PACKAGE_ROOT}/MediaSDK-3.1.3-20260128-win64_1769600100370/MediaSDK-3.1.3-20260128-win64/MediaSDK" CACHE PATH "MediaSDK 目录")

if(NOT EXISTS "${CAMERA_SDK_ROOT}/lib/CameraSDK.lib")
    message(FATAL_ERROR "找不到 CameraSDK.lib: ${CAMERA_SDK_ROOT}")
endif()
if(NOT EXISTS "${MEDIA_SDK_ROOT}/lib/MediaSDK.lib")
    message(FATAL_ERROR "找不到 MediaSDK.lib: ${MEDIA_SDK_ROOT}")
endif()

add_executable(x4_live_demo
    src/main.cpp
    src/rdk_stream.cpp
    src/photo_output.cpp
)

add_executable(test_rdk_stream
    src/test_rdk_stream.cpp
    src/rdk_stream.cpp
)

add_executable(test_photo_output
    src/test_photo_output.cpp
    src/photo_output.cpp
)

target_include_directories(x4_live_demo PRIVATE
    "${CAMERA_SDK_ROOT}/include"
    "${MEDIA_SDK_ROOT}/include/stitcher"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(test_rdk_stream PRIVATE
    ws2_32 windowscodecs ole32
)

target_include_directories(test_photo_output PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(test_photo_output PRIVATE
    ws2_32 windowscodecs ole32
)

target_link_libraries(x4_live_demo PRIVATE
    "${CAMERA_SDK_ROOT}/lib/CameraSDK.lib"
    "${MEDIA_SDK_ROOT}/lib/MediaSDK.lib"
    ws2_32 windowscodecs ole32
)

message(STATUS "CameraSDK: ${CAMERA_SDK_ROOT}")
message(STATUS "MediaSDK:  ${MEDIA_SDK_ROOT}")
