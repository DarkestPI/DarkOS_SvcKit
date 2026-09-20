add_library(svckit_iva STATIC
    src/algorithm/algorithm.cpp
    src/algorithm/traditional/motion_algorithm.cpp
    src/algorithm/traditional/region_algorithm.cpp
    src/algorithm/dnn/face_detect.cpp
    src/algorithm/dnn/person_detect.cpp
    src/algorithm/dnn/vehicle_detect.cpp
    src/core/iva_manager.cpp
    src/core/iva_scheduler.cpp
    src/core/iva_stats.cpp
    src/factory/iva_factory.cpp
    src/inference/cpu_engine.cpp
    src/inference/platform_engine.cpp
    src/model/model_manager.cpp
    src/perimeter/intrusion_rule.cpp
    src/perimeter/line_cross_rule.cpp
    src/perimeter/loitering_rule.cpp
    src/tracker/byte_track_tracker.cpp
    src/tracker/deep_sort_tracker.cpp
    src/tracker/kalman_tracker.cpp
)

target_include_directories(svckit_iva PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>)
target_compile_features(svckit_iva PUBLIC cxx_std_17)
target_link_libraries(svckit_iva
    PUBLIC svckit_base
    PRIVATE inference_interfaces)

# SvcKit 只依赖 inference HAL 的通用契约。RKNN、NNIE、联咏等 SDK 由
# platform/vendors/<vendor>/socs/<soc> 的 HAL 实现负责查找和链接。

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_iva PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

darkos_prepare_target_output(svckit_iva)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(svc_iva_test tests/iva_test.cpp)
    target_link_libraries(svc_iva_test PRIVATE svckit_iva)
    target_compile_features(svc_iva_test PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(svc_iva_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME svc_iva.pipeline COMMAND svc_iva_test)
    set_tests_properties(svc_iva.pipeline PROPERTIES TIMEOUT 5)
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests svc_iva_test)
    endif()
endif()
