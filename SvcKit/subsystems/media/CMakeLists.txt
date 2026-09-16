add_library(svckit_media STATIC
    src/core/media_pipeline.cpp
    src/core/media_manager.cpp
    src/factory/media_factory.cpp
    src/codec/g711_audio_codec.cpp
)

target_include_directories(svckit_media
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(svckit_media PUBLIC cxx_std_17)
target_link_libraries(svckit_media
    PUBLIC
        svckit_base
    PRIVATE
        hardware_base
        audio_interfaces
        camera_interfaces
        codec_interfaces
        svc_log
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_media PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

darkos_prepare_target_output(svckit_media)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING AND TARGET hal_host_x86)
    add_executable(svc_media_pipeline_test
        tests/media_pipeline_test.cpp
    )
    target_link_libraries(svc_media_pipeline_test PRIVATE svckit_media)
    target_compile_features(svc_media_pipeline_test PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(svc_media_pipeline_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_dependencies(svc_media_pipeline_test hal_host_x86)

    add_test(NAME svc_media.pipeline
        COMMAND "${CMAKE_COMMAND}" -E env
            "DARKOS_HAL_VARIANT=host_x86"
            "DARKOS_HAL_LIBRARY_PATH=$<TARGET_FILE_DIR:hal_host_x86>"
            "$<TARGET_FILE:svc_media_pipeline_test>")
    set_tests_properties(svc_media.pipeline PROPERTIES TIMEOUT 10)

    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests svc_media_pipeline_test)
    endif()
endif()
