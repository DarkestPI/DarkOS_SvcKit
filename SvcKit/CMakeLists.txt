# SvcKit 只定义库目标；project()、CTest、CPack 由顶层应用决定。
add_subdirectory(foundation)
add_subdirectory(subsystems)
add_subdirectory(Protocol)

add_library(darkos_svckit INTERFACE)
target_link_libraries(darkos_svckit INTERFACE
    svckit_base
    svckit_analytics
    svckit_audio
    svckit_media
    svckit_networking
    darkos_protocols
    DarkOS::Platform
)
add_library(DarkOS::SvcKit ALIAS darkos_svckit)

if(DARKOS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    include(CTest)
    add_subdirectory(tests)
endif()
