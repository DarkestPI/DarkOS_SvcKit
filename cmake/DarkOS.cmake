# 同一个顶层构建只能装载一次 SDK，防止应用或组件重复 include 后重复定义目标。
include_guard(GLOBAL)

# 本文件固定放在 <SDK_ROOT>/cmake/，因此可以可靠地反推出 SDK 根目录。
# INTERNAL 缓存用于让所有子目录读取同一个规范化后的绝对路径，同时不在
# cmake-gui 的普通选项中暴露这个内部变量。
get_filename_component(DARKOS_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(DARKOS_SDK_ROOT "${DARKOS_SDK_ROOT}" CACHE INTERNAL "DarkOS SDK root")

message(STATUS "DarkOS SDK root: ${DARKOS_SDK_ROOT}")

# SDK 功能开关。骨架阶段默认只建立目标依赖图；对应源码和第三方依赖准备好后，
# Application 可以通过 -D<OPTION>=ON 启用实际实现、示例和测试。
option(DARKOS_BUILD_TESTS "Build DarkOS SDK tests" OFF)
option(DARKOS_BUILD_EXAMPLES "Build DarkOS SDK examples" OFF)
option(DARKOS_BUILD_HAL_IMPLEMENTATIONS "Build the selected platform HAL implementation" OFF)
option(DARKOS_BUILD_PROTOCOL_IMPLEMENTATIONS "Build protocol implementations instead of skeleton targets" OFF)
option(DARKOS_BUILD_SECURITY_COMPONENTS "Build SvcKit crypto and keystore components" OFF)
option(DARKOS_ENABLE_OUTPUT_LAYOUT "Use the DarkOS bin/lib/etc output layout" ON)
option(DARKOS_ENABLE_LTO "Enable link-time optimization for Release builds" OFF)

# LTO 对日常交叉编译和链接影响明显，因此默认关闭，只在发布构建显式启用。
# 使用 CMake 的 IPO 属性而不是手写 -flto，以便由具体编译器选择正确参数。
if(DARKOS_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT darkos_lto_supported OUTPUT darkos_lto_error
        LANGUAGES C CXX)
    if(NOT darkos_lto_supported)
        message(FATAL_ERROR "DARKOS_ENABLE_LTO is ON, but IPO is unavailable: ${darkos_lto_error}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "DarkOS Release LTO: enabled")
endif()

# 必须在调用方的顶层目录启用 CTest，否则子目录中的 add_test() 虽然能生成
# 测试程序，但从 Application 构建目录执行 ctest 时无法发现测试。
if(DARKOS_BUILD_TESTS)
    enable_testing()
endif()

# 所有 Application 共用的发布根目录。默认组装到仓库根 output/，也可以通过
# -DDARKOS_OUTPUT_ROOT=/path/to/rootfs 覆盖；交叉编译建议使用 output/<board>。
set(DARKOS_OUTPUT_ROOT
    "${DARKOS_SDK_ROOT}/output"
    CACHE PATH
    "DarkOS application staging root")

if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    # 无论调用者传入相对路径还是绝对路径，后续规则都只使用规范化绝对路径。
    get_filename_component(DARKOS_OUTPUT_ROOT "${DARKOS_OUTPUT_ROOT}" ABSOLUTE)

    # 编译阶段直接形成目标文件系统布局：程序进 bin，库进 lib。
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DARKOS_OUTPUT_ROOT}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DARKOS_OUTPUT_ROOT}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${DARKOS_OUTPUT_ROOT}/lib")

    # 多配置生成器不再自动追加 Debug/Release 子目录。
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${DARKOS_OUTPUT_ROOT}/bin")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} "${DARKOS_OUTPUT_ROOT}/lib")
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config} "${DARKOS_OUTPUT_ROOT}/lib")
    endforeach()

    # etc 不是编译产物，但提前创建可以保证空配置目录也存在于 staging root。
    file(MAKE_DIRECTORY
        "${DARKOS_OUTPUT_ROOT}/bin"
        "${DARKOS_OUTPUT_ROOT}/lib"
        "${DARKOS_OUTPUT_ROOT}/etc")

    # 仅覆盖 CMake 的默认安装前缀；调用者显式传入 --prefix 或
    # -DCMAKE_INSTALL_PREFIX 时保持调用者的选择。
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "${DARKOS_OUTPUT_ROOT}" CACHE PATH
            "DarkOS install prefix" FORCE)
    endif()

    message(STATUS "DarkOS output root: ${DARKOS_OUTPUT_ROOT}")
endif()

# 给会产生文件的目标增加链接前目录检查。与配置阶段的 file(MAKE_DIRECTORY)
# 不同，这条命令会在每次真正链接/归档前运行，因此 output/ 被手动删除后，
# 直接执行 make 也可以自动恢复 bin/lib/etc。
function(darkos_prepare_target_output target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "darkos_prepare_target_output: target '${target}' does not exist")
    endif()

    if(DARKOS_ENABLE_OUTPUT_LAYOUT)
        add_custom_command(TARGET "${target}" PRE_LINK
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${DARKOS_OUTPUT_ROOT}/bin"
                "${DARKOS_OUTPUT_ROOT}/lib"
                "${DARKOS_OUTPUT_ROOT}/etc"
            COMMENT "Preparing DarkOS output directories")
    endif()
endfunction()

# Application 在 add_executable() 之后调用此函数：
#   darkos_add_application(${PROJECT_NAME})
# 它负责安装目标，并把 <application>/etc/ 同步到 staging root 的 etc/。
function(darkos_add_application target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "darkos_add_application: target '${target}' does not exist")
    endif()

    darkos_prepare_target_output("${target}")

    install(TARGETS "${target}"
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

    if(DARKOS_ENABLE_OUTPUT_LAYOUT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/etc")
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/etc"
                "${DARKOS_OUTPUT_ROOT}/etc"
            COMMENT "Staging ${target} configuration")
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/etc")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/etc/" DESTINATION etc)
    endif()
endfunction()

# 必须先引入 Platform，因为 SvcKit 的媒体、音频、外设等目标依赖 HAL 接口。
# 显式指定 binary_dir，允许 SDK 源码位于当前 Application 源码树之外。
if(NOT TARGET DarkOS::Platform)
    add_subdirectory(
        "${DARKOS_SDK_ROOT}/platform"
        "${CMAKE_BINARY_DIR}/darkos/platform"
        EXCLUDE_FROM_ALL
    )
endif()

# SvcKit 最后引入，并向 Application 暴露 DarkOS::SvcKit 及细粒度组件目标。
if(NOT TARGET DarkOS::SvcKit)
    add_subdirectory(
        "${DARKOS_SDK_ROOT}/SvcKit"
        "${CMAKE_BINARY_DIR}/darkos/SvcKit"
        EXCLUDE_FROM_ALL
    )
endif()
