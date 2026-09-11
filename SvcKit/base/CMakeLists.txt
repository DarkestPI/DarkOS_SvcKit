# SvcKit 基础库：只依赖标准库和系统线程库，供所有上层组件复用。
find_package(Threads REQUIRED)

add_library(svckit_base STATIC
    src/Log.cpp
)

target_include_directories(svckit_base
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(svckit_base PUBLIC cxx_std_17)
target_link_libraries(svckit_base PUBLIC Threads::Threads)

darkos_prepare_target_output(svckit_base)

add_library(DarkOS::Base ALIAS svckit_base)
