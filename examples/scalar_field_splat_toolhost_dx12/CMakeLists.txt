add_executable(scalar_field_splat_toolhost_dx12
    main.cpp
)

target_link_libraries(scalar_field_splat_toolhost_dx12
    PRIVATE
        wozzits_toolhost::wozzits_toolhost
        d3d12
        dxgi
        dxguid
        d3dcompiler
)

target_include_directories(scalar_field_splat_toolhost_dx12
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends
)

target_compile_features(scalar_field_splat_toolhost_dx12 PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(scalar_field_splat_toolhost_dx12
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()

add_custom_command(TARGET scalar_field_splat_toolhost_dx12 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/../../../wozzits-window-engine/window_engine/resources"
        "$<TARGET_FILE_DIR:scalar_field_splat_toolhost_dx12>/resources"
    COMMENT "Copying Wozzits resources for scalar_field_splat_toolhost_dx12"
)
