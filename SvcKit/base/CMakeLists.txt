# SvcKit 基础组件聚合目标：供上层统一链接基础能力。
find_package(Threads REQUIRED)

add_subdirectory(svc_log)
add_subdirectory(svc_board)

add_library(svckit_base INTERFACE)
target_link_libraries(svckit_base INTERFACE svc_log svc_board)

if(DARKOS_BUILD_SECURITY_COMPONENTS)
    add_subdirectory(svc_security)
    target_link_libraries(svckit_base INTERFACE svc_security)
endif()

add_library(DarkOS::Base ALIAS svckit_base)
