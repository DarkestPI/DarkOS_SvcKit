# cJSON 采用官方单文件集成方式；只编译核心解析器，cJSON_Utils 当前未使用。
add_library(cjson STATIC
    cJSON/cJSON.c
)
target_include_directories(cjson PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cJSON>
    $<INSTALL_INTERFACE:include/cjson>
)
target_compile_features(cjson PUBLIC c_std_17)
target_compile_definitions(cjson PRIVATE CJSON_NESTING_LIMIT=64)
set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
darkos_prepare_target_output(cjson)

add_library(DarkOS::cJSON ALIAS cjson)
