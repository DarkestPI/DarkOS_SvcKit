# 安全组件使用成熟密码库提供算法实现，禁止在业务组件中自行实现密码算法。
find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)

add_subdirectory(svc_crypto)
add_subdirectory(svc_keystore)

add_library(svc_security INTERFACE)
target_link_libraries(svc_security INTERFACE svc_crypto svc_keystore)

add_library(DarkOS::Security ALIAS svc_security)
