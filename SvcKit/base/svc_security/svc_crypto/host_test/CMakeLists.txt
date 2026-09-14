add_executable(svc_crypto_host_test
    c_api_compile_test.c
    svc_crypto_test.cpp
)

target_compile_features(svc_crypto_host_test PRIVATE c_std_17 cxx_std_17)
target_link_libraries(svc_crypto_host_test PRIVATE svc_crypto)
target_compile_options(svc_crypto_host_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

set_target_properties(svc_crypto_host_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME svc_crypto.host COMMAND svc_crypto_host_test)
set_tests_properties(svc_crypto.host PROPERTIES TIMEOUT 10)

if(TARGET svckit_host_tests)
    add_dependencies(svckit_host_tests svc_crypto_host_test)
endif()
