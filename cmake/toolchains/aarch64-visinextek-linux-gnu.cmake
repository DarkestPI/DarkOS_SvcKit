# Visinextek VS816 (Orion) Linux/AArch64 cross toolchain.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(DARKOS_VS816_RELEASE_ROOT
    "/home/nlj/workspace1/P006_VS816_M2_20260922/customer-rel"
    CACHE PATH "VS816 customer release root")
set(DARKOS_VS816_SDK
    "${DARKOS_VS816_RELEASE_ROOT}/board/package"
    CACHE PATH "VS816 board/package SDK root")
set(DARKOS_VS816_MEDIA_ROOT
    "${DARKOS_VS816_SDK}/vs-mp"
    CACHE PATH "VS816 media SDK root (include/lib)")
set(DARKOS_VS816_SAMPLE_ROOT
    "${DARKOS_VS816_SDK}/build/vs-sample"
    CACHE PATH "VS816 SDK sample source root")
set(DARKOS_VS816_TOOLCHAIN_ROOT
    "${DARKOS_VS816_RELEASE_ROOT}/toolchains/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu"
    CACHE PATH "VS816 AArch64 toolchain root")

set(CMAKE_C_COMPILER
    "${DARKOS_VS816_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER
    "${DARKOS_VS816_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-g++")
set(CMAKE_SYSROOT
    "${DARKOS_VS816_TOOLCHAIN_ROOT}/aarch64-linux-gnu/libc")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" "${DARKOS_VS816_MEDIA_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections -march=armv8-a")
get_filename_component(DARKOS_VS816_CMAKE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_FLAGS_INIT
    "-ffunction-sections -fdata-sections -march=armv8-a -I${DARKOS_VS816_CMAKE_ROOT}/compat/gcc7")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--gc-sections")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--gc-sections")

# The vendor release ships GCC 7.5. It implements the C11 language features
# used by DarkOS but predates GCC's `-std=c17` spelling. Teach CMake to satisfy
# the project's C17 request with the equivalent compiler mode for this SDK.
set(CMAKE_C17_STANDARD_COMPILE_OPTION "-std=c11")
set(CMAKE_C17_EXTENSION_COMPILE_OPTION "-std=gnu11")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG" CACHE STRING "C debug flags")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG" CACHE STRING "C++ debug flags")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "C release flags")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "C++ release flags")
