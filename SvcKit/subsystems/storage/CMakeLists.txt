add_library(svckit_storage STATIC
    src/core/storage_manager.cpp)

target_include_directories(svckit_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>)
target_compile_features(svckit_storage PUBLIC cxx_std_17)
target_link_libraries(svckit_storage PUBLIC svckit_media)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_storage PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

darkos_prepare_target_output(svckit_storage)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(svc_storage_test tests/storage_test.cpp)
    target_link_libraries(svc_storage_test PRIVATE svckit_storage)
    target_compile_features(svc_storage_test PRIVATE cxx_std_17)
    add_test(NAME svc_storage.recording COMMAND svc_storage_test)
    set_tests_properties(svc_storage.recording PROPERTIES TIMEOUT 5)
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests svc_storage_test)
    endif()
endif()
