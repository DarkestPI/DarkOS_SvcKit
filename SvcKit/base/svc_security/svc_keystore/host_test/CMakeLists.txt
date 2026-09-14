add_executable(svc_keystore_host_test
    svc_keystore_test.cpp
)

target_compile_features(svc_keystore_host_test PRIVATE cxx_std_17)
target_link_libraries(svc_keystore_host_test PRIVATE svc_keystore)
target_compile_options(svc_keystore_host_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

set_target_properties(svc_keystore_host_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME svc_keystore.host COMMAND svc_keystore_host_test)
set_tests_properties(svc_keystore.host PROPERTIES TIMEOUT 10)

if(TARGET svckit_host_tests)
    add_dependencies(svckit_host_tests svc_keystore_host_test)
endif()
