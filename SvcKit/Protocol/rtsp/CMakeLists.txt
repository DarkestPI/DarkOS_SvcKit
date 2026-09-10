# RTSP 服务器（M1：OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN，UDP + TCP interleaved）
add_library(darkos_rtsp STATIC
    src/RtspServer.cpp
    src/RtspConnection.cpp
    src/RtspSession.cpp
    src/RtspRtpSender.cpp
)
target_include_directories(darkos_rtsp PUBLIC include)
# svckit_stream 会传递带上 svckit_base（见 stream/CMakeLists.txt）
target_link_libraries(darkos_rtsp PUBLIC svckit_stream)

# 自测：本机回环验证 OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN 响应格式
if(DARKOS_BUILD_TESTS)
    add_executable(rtsp_probe tests/rtsp_probe.cpp)
    target_link_libraries(rtsp_probe PRIVATE darkos_rtsp)

    # 端到端出流验证：合成相机 → x264 软编 → RtspServer（真机拉流联调用）
    add_executable(live_probe tests/live_probe.cpp)
    target_link_libraries(live_probe PRIVATE
        darkos_rtsp hardware camera_interfaces media_interfaces)
endif()
