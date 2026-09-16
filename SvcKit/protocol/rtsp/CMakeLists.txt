if(DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS)
    add_library(darkos_rtsp STATIC
        src/RtspServer.cpp
        src/RtspConnection.cpp
        src/RtspSession.cpp
        src/RtspRtpSender.cpp
    )
    target_include_directories(darkos_rtsp PUBLIC include)
    target_link_libraries(darkos_rtsp PUBLIC svckit_stream)
    darkos_prepare_target_output(darkos_rtsp)
else()
    add_library(darkos_rtsp INTERFACE)
endif()

add_library(DarkOS::RTSP ALIAS darkos_rtsp)

# 自测：本机回环验证 OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN 响应格式
if(DARKOS_BUILD_TESTS AND DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS)
    add_executable(rtsp_probe tests/rtsp_probe.cpp)
    target_link_libraries(rtsp_probe PRIVATE darkos_rtsp)

    # 端到端出流验证：合成相机 → x264 软编 → RtspServer（真机拉流联调用）
    add_executable(live_probe tests/live_probe.cpp)
    target_link_libraries(live_probe PRIVATE
        darkos_rtsp hardware camera_interfaces codec_interfaces)
endif()
