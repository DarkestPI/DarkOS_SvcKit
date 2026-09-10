# 串口控制面适配器（uart）：只处理行协议，serial HAL 由 Services/peripheral 持有。
if(DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS)
    add_library(darkos_uart STATIC src/UartAdapter.cpp)
    target_include_directories(darkos_uart PUBLIC include)
    target_link_libraries(darkos_uart
        PUBLIC svckit_base svckit_control
        PRIVATE svckit_control_events)
    darkos_prepare_target_output(darkos_uart)
else()
    add_library(darkos_uart INTERFACE)
endif()

add_library(DarkOS::UART ALIAS darkos_uart)

# 自测：openpty 主从对打通行协议全命令（仅 host 构建，见根 CMakeLists 开关）
if(DARKOS_BUILD_TESTS AND DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS)
    add_executable(uart_probe tests/uart_probe.cpp)
    target_link_libraries(uart_probe PRIVATE darkos_uart svckit_peripheral)
endif()
