add_executable(svc_board_host_test
    svc_board_test.cpp
)

target_compile_features(svc_board_host_test PRIVATE cxx_std_17)
target_link_libraries(svc_board_host_test PRIVATE svc_board)
target_compile_options(svc_board_host_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

set_target_properties(svc_board_host_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(svc_board_host_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}"
    )
endforeach()

add_test(NAME svc_board.host COMMAND svc_board_host_test)
set_tests_properties(svc_board.host PROPERTIES TIMEOUT 10)

if(TARGET svckit_host_tests)
    add_dependencies(svckit_host_tests svc_board_host_test)
endif()
