add_library(svc_keystore STATIC
    src/svc_keystore.cpp
)

target_include_directories(svc_keystore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(svc_keystore PUBLIC cxx_std_17)
target_link_libraries(svc_keystore PUBLIC svc_crypto Threads::Threads)

darkos_prepare_target_output(svc_keystore)

add_library(DarkOS::KeyStore ALIAS svc_keystore)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(host_test)
endif()
