# SvcKit 通用日志组件，底层仅依赖 C++ 标准库和线程库。
add_library(svc_log STATIC
    src/svc_log.cpp
)

target_include_directories(svc_log
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(svc_log PUBLIC cxx_std_17)
target_link_libraries(svc_log PUBLIC Threads::Threads)

darkos_prepare_target_output(svc_log)

add_library(DarkOS::Log ALIAS svc_log)

# host_test 只验证宿主机行为，不加入交叉编译构建。
if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(host_test)
endif()
