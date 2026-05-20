# wozzits_toolhost is defined in the parent CMakeLists.txt and already
# resolves window_engine, so no find_package block is needed here.

add_executable(scene_benchmark_toolhost
    main.cpp
)

target_link_libraries(scene_benchmark_toolhost
    PRIVATE
        wozzits_toolhost::wozzits_toolhost
        d3d12
        dxgi
        dxguid
        d3dcompiler
)

target_include_directories(scene_benchmark_toolhost
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends
)

target_compile_features(scene_benchmark_toolhost PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(scene_benchmark_toolhost
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()

if(WOZZITS_ENABLE_V8_SCRIPTING)
    target_link_libraries(scene_benchmark_toolhost
        PRIVATE
            wozzits_v8::wozzits_v8
    )

    target_compile_definitions(scene_benchmark_toolhost
        PRIVATE
            WOZZITS_ENABLE_V8_SCRIPTING
    )
endif()

add_custom_command(TARGET scene_benchmark_toolhost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/../../../wozzits-window-engine/window_engine/resources"
        "$<TARGET_FILE_DIR:scene_benchmark_toolhost>/resources"
    COMMENT "Copying Wozzits resources for scene_benchmark_toolhost"
)

add_custom_command(TARGET scene_benchmark_toolhost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/../gaussian_splat_toolhost_dx12/splats"
        "$<TARGET_FILE_DIR:scene_benchmark_toolhost>/resources/splats"
    COMMENT "Copying splat PLY files for scene_benchmark_toolhost"
)
