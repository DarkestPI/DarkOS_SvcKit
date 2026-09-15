add_library(loader_test_hal SHARED fake_hal.c)
target_link_libraries(loader_test_hal PRIVATE hardware_base)
set_target_properties(loader_test_hal PROPERTIES
    OUTPUT_NAME "hal.loader_test"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(loader_test_hal PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}"
    )
endforeach()

add_executable(hardware_loader_test loader_test.c)
target_link_libraries(hardware_loader_test PRIVATE hardware_base)
target_compile_definitions(hardware_loader_test PRIVATE
    TEST_HAL_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(hardware_loader_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(hardware_loader_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}"
    )
endforeach()
add_dependencies(hardware_loader_test loader_test_hal)

add_test(NAME platform.hardware_loader COMMAND hardware_loader_test)
set_tests_properties(platform.hardware_loader PROPERTIES TIMEOUT 10)

if(TARGET svckit_host_tests)
    add_dependencies(svckit_host_tests hardware_loader_test)
endif()
