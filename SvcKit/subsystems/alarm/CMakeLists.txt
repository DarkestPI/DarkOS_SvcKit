add_library(svckit_alarm STATIC
    src/core/alarm_manager.cpp
    src/rule/filter_rule.cpp
    src/action/notify_action.cpp)

target_include_directories(svckit_alarm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>)
target_compile_features(svckit_alarm PUBLIC cxx_std_17)
target_link_libraries(svckit_alarm PUBLIC svckit_runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_alarm PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
darkos_prepare_target_output(svckit_alarm)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(svc_alarm_test tests/alarm_test.cpp)
    target_link_libraries(svc_alarm_test PRIVATE svckit_alarm)
    target_compile_features(svc_alarm_test PRIVATE cxx_std_17)
    add_test(NAME svc_alarm.lifecycle COMMAND svc_alarm_test)
    set_tests_properties(svc_alarm.lifecycle PROPERTIES TIMEOUT 5)
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests svc_alarm_test)
    endif()
endif()
