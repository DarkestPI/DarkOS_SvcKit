# hardware 层：接口定义 -> 加载器 -> 实现 -> 示例
add_subdirectory(interfaces)
add_subdirectory(libhardware)

# 实现按目标二选一：
#   交叉编译（目标板 rv1126）-> 厂商实现 rockchip
#   本机构建（host_x86）       -> 主机参考实现 host_x86
if(CMAKE_CROSSCOMPILING)
    add_subdirectory(rockchip)
else()
    add_subdirectory(host_x86)
endif()

# 验证程序（hal_probe / media_probe / misc_probe）随 DARKOS_BUILD_TESTS 开关
if(DARKOS_BUILD_TESTS)
    add_subdirectory(examples)
endif()
