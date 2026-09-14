add_library(svc_crypto STATIC
    src/svc_crypto.cpp
)

target_include_directories(svc_crypto
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(svc_crypto PUBLIC cxx_std_17)
target_link_libraries(svc_crypto PUBLIC OpenSSL::Crypto)

darkos_prepare_target_output(svc_crypto)

add_library(DarkOS::Crypto ALIAS svc_crypto)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(host_test)
endif()
