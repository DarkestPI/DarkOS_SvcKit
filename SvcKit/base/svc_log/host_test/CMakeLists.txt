add_executable(svc_log_host_test
    c_api_compile_test.c
    svc_log_test.cpp
)

target_compile_features(svc_log_host_test PRIVATE c_std_17 cxx_std_17)
target_link_libraries(svc_log_host_test PRIVATE svc_log)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
   OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svc_log_host_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

# 测试程序保留在构建树内，不进入 DarkOS 的发布 bin 目录。
set_target_properties(svc_log_host_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(svc_log_host_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}"
    )
endforeach()

add_test(NAME svc_log.host COMMAND svc_log_host_test)
set_tests_properties(svc_log.host PROPERTIES TIMEOUT 10)

if(TARGET svckit_host_tests)
    add_dependencies(svckit_host_tests svc_log_host_test)
endif()
