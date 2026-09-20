# SvcKit 只定义库目标；project()、CTest、CPack 由顶层应用决定。
# 宿主机测试统一挂到该聚合目标，脚本无需逐个记忆测试程序名称。
if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_custom_target(svckit_host_tests)
    if(TARGET hardware_loader_test)
        add_dependencies(svckit_host_tests hardware_loader_test)
    endif()
    if(TARGET serial_termios_test)
        add_dependencies(svckit_host_tests serial_termios_test)
    endif()
endif()

add_subdirectory(third_party)
add_subdirectory(base)
add_subdirectory(subsystems)
add_subdirectory(protocols)

add_library(darkos_svckit INTERFACE)
target_link_libraries(darkos_svckit INTERFACE
    svckit_base
    svckit_analytics
    svckit_audio
    svckit_media
    svckit_networking
    svckit_peripheral
    svckit_storage
    svckit_alarm
    svckit_iva
    DarkOS::Platform
)
add_library(DarkOS::SvcKit ALIAS darkos_svckit)

if(DARKOS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
