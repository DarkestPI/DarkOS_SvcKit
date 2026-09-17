add_library(svckit_runtime STATIC
    src/EventLoop.cpp
    src/BufferPool.cpp
    src/Hash.cpp
)

target_include_directories(svckit_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(svckit_runtime PUBLIC cxx_std_17)
target_link_libraries(svckit_runtime PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

darkos_prepare_target_output(svckit_runtime)
add_library(DarkOS::Runtime ALIAS svckit_runtime)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(svc_event_loop_test tests/event_loop_test.cpp)
    target_link_libraries(svc_event_loop_test PRIVATE svckit_runtime)
    target_compile_features(svc_event_loop_test PRIVATE cxx_std_17)
    add_test(NAME svc_runtime.event_loop COMMAND svc_event_loop_test)
    set_tests_properties(svc_runtime.event_loop PROPERTIES TIMEOUT 5)
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests svc_event_loop_test)
    endif()
endif()
