add_library(svc_board STATIC
    src/AppConfig.cpp
    src/BoardConfig.cpp
)

target_include_directories(svc_board
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(svc_board PUBLIC cxx_std_17)
target_link_libraries(svc_board PRIVATE cjson)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svc_board PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
darkos_prepare_target_output(svc_board)

add_library(DarkOS::Board ALIAS svc_board)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(host_test)
endif()
