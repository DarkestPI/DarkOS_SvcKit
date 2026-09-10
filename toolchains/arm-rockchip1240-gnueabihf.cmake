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
# 通用编译标志
# 为链接器 --gc-sections 做准备
# ------------------------------------------------------------
set(CMAKE_C_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} -ffunction-sections -fdata-sections")

set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_CXX_FLAGS_INIT} -ffunction-sections -fdata-sections")

# ------------------------------------------------------------
# Release 优化
# IPC 程序建议 -O2 更稳；想更激进可换 -O3
# ------------------------------------------------------------
set(CMAKE_C_FLAGS_RELEASE_INIT
    "-O2 -DNDEBUG -ffunction-sections -fdata-sections")

set(CMAKE_CXX_FLAGS_RELEASE_INIT
    "-O2 -DNDEBUG -ffunction-sections -fdata-sections")

# ------------------------------------------------------------
# Debug / RelWithDebInfo
# ------------------------------------------------------------
set(CMAKE_C_FLAGS_DEBUG_INIT
    "-O0 -g -DDEBUG -ffunction-sections -fdata-sections")

set(CMAKE_CXX_FLAGS_DEBUG_INIT
    "-O0 -g -DDEBUG -ffunction-sections -fdata-sections")

set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT
    "-O2 -g -DNDEBUG -ffunction-sections -fdata-sections")

set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT
    "-O2 -g -DNDEBUG -ffunction-sections -fdata-sections")

# ------------------------------------------------------------
# 链接优化
# 删除未使用的 section
# ------------------------------------------------------------
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CMAKE_EXE_LINKER_FLAGS_INIT} -Wl,--gc-sections")

set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT
    "-Wl,--gc-sections")

set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO_INIT
    "-Wl,--gc-sections")

# ------------------------------------------------------------
# 可选：CPU 架构优化
# 只有确认工具链默认没有带正确架构参数时才打开。
# 否则可能和 sysroot 里的预编译库 ABI 不一致。
# ------------------------------------------------------------
set(RV1126B_ARCH_FLAGS
    "-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} ${RV1126B_ARCH_FLAGS}")

set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_CXX_FLAGS_INIT} ${RV1126B_ARCH_FLAGS}")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CMAKE_EXE_LINKER_FLAGS_INIT} ${RV1126B_ARCH_FLAGS}")

# ------------------------------------------------------------
# 可选：LTO 链接时优化
# 链接 rockchip 预编译媒体库时可能出 LTO 插件错误。
# 如果链接失败，就去掉下面三行。
# ------------------------------------------------------------
set(CMAKE_C_FLAGS_RELEASE_INIT
    "${CMAKE_C_FLAGS_RELEASE_INIT} -flto")

set(CMAKE_CXX_FLAGS_RELEASE_INIT
    "${CMAKE_CXX_FLAGS_RELEASE_INIT} -flto")

set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT
    "${CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT} -flto")