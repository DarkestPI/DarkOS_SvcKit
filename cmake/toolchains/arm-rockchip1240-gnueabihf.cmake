# ============================================================
# RV1126B Linux IPC 交叉编译工具链
# 目标：ARMv7-A / gnueabihf
# 用法：
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=/path/to/rv1126b.toolchain.cmake
#   cmake --build build -j
# ============================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ------------------------------------------------------------
# SDK / 工具链 / 媒体库路径
# ------------------------------------------------------------
set(DARKOS_RV1126B_SDK
    "/tftpboot/RV1126B-IPC50/rv1126b_linux_ipc_v1.2.0"
    CACHE PATH "RV1126B IPC SDK 根目录")

set(DARKOS_RV1126B_TC
    "${DARKOS_RV1126B_SDK}/tools/linux/toolchain/arm-rockchip1240-linux-gnueabihf"
    CACHE PATH "RV1126B 交叉工具链根目录")

# 板子媒体库根（include/ + lib/）：rockit/mpp/rkaiq/rga，供 rockchip HAL 链接
set(DARKOS_RV1126B_MEDIA_ROOT
    "${DARKOS_RV1126B_SDK}/output/out/media_out"
    CACHE PATH "RV1126B 媒体库根目录（include + lib）")

# RV1126B 的 RKNN C API 与 ARMhf Runtime。Rockchip Platform HAL 会从这个
# 目录查找 include/rknn_api.h 和 Linux/armhf/librknnrt.so，并导出 inference SPI。
set(DARKOS_RV1126B_RKNN_ROOT
    "${DARKOS_RV1126B_SDK}/project/app/testdemo/yoloworld_demo/3rdparty/rknpu2"
    CACHE PATH "RV1126B RKNN SDK 根目录")

# ------------------------------------------------------------
# 交叉编译器
# ------------------------------------------------------------
set(CMAKE_C_COMPILER
    "${DARKOS_RV1126B_TC}/bin/arm-rockchip1240-linux-gnueabihf-gcc")

set(CMAKE_CXX_COMPILER
    "${DARKOS_RV1126B_TC}/bin/arm-rockchip1240-linux-gnueabihf-g++")

# ------------------------------------------------------------
# sysroot 与查找路径
# 工具链自带 sysroot（libc）
# rockchip 媒体库经 DARKOS_RV1126B_MEDIA_ROOT 显式引用
# ------------------------------------------------------------
set(CMAKE_SYSROOT
    "${DARKOS_RV1126B_TC}/arm-rockchip1240-linux-gnueabihf/sysroot")

set(CMAKE_FIND_ROOT_PATH
    "${CMAKE_SYSROOT}"
    "${DARKOS_RV1126B_MEDIA_ROOT}")

# 程序在宿主机找；库、头文件、包只在交叉 sysroot / 媒体库中找
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ------------------------------------------------------------
# 默认构建类型：Release
# 如果命令行没有指定 -DCMAKE_BUILD_TYPE=xxx，则默认 Release
# ------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------
# 可选：CPU 架构优化
# 只有确认工具链默认没有带正确架构参数时才打开。
# 否则可能和 sysroot 里的预编译库 ABI 不一致。
# ------------------------------------------------------------
set(RV1126B_ARCH_FLAGS
    "-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")

# ------------------------------------------------------------
# 编译与链接参数
#
# 工具链文件可能被 CMake/try_compile 多次载入，因此这里使用确定值初始化，
# 不引用旧值做字符串追加，避免架构参数重复。CACHE 不使用 FORCE：调用者
# 显式传入 -DCMAKE_<LANG>_FLAGS_<CONFIG> 时仍然拥有更高优先级。
# LTO 由 DarkOS.cmake 的 DARKOS_ENABLE_LTO 统一控制；日常构建默认关闭，
# 避免拖慢链接以及触发 Rockchip 预编译媒体库的 LTO 插件兼容问题。
# ------------------------------------------------------------
set(DARKOS_COMMON_COMPILE_FLAGS
    "-ffunction-sections -fdata-sections ${RV1126B_ARCH_FLAGS}")

set(CMAKE_C_FLAGS_INIT "${DARKOS_COMMON_COMPILE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${DARKOS_COMMON_COMPILE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--gc-sections")

set(CMAKE_C_FLAGS_DEBUG
    "-O0 -g -DDEBUG"
    CACHE STRING "C flags for Debug")
set(CMAKE_CXX_FLAGS_DEBUG
    "-O0 -g -DDEBUG"
    CACHE STRING "C++ flags for Debug")

set(CMAKE_C_FLAGS_RELEASE
    "-O2 -DNDEBUG"
    CACHE STRING "C flags for Release")
set(CMAKE_CXX_FLAGS_RELEASE
    "-O2 -DNDEBUG"
    CACHE STRING "C++ flags for Release")

# 迁移由旧版工具链生成的缓存，同时保留调用者自定义的 Release flags。
if(CMAKE_C_FLAGS_RELEASE STREQUAL "-O2 -DNDEBUG -flto")
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING
        "C flags for Release" FORCE)
endif()
if(CMAKE_CXX_FLAGS_RELEASE STREQUAL "-O2 -DNDEBUG -flto")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING
        "C++ flags for Release" FORCE)
endif()

set(CMAKE_C_FLAGS_RELWITHDEBINFO
    "-O2 -g -DNDEBUG"
    CACHE STRING "C flags for RelWithDebInfo")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "-O2 -g -DNDEBUG"
    CACHE STRING "C++ flags for RelWithDebInfo")

set(CMAKE_C_FLAGS_MINSIZEREL
    "-Os -DNDEBUG"
    CACHE STRING "C flags for MinSizeRel")
set(CMAKE_CXX_FLAGS_MINSIZEREL
    "-Os -DNDEBUG"
    CACHE STRING "C++ flags for MinSizeRel")
