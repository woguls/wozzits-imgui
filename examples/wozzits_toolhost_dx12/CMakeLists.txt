# wozzits_toolhost is defined in the parent CMakeLists.txt and already
# resolves window_engine, so no find_package block is needed here.

add_executable(wozzits_toolhost_dx12
    main.cpp
)

target_link_libraries(wozzits_toolhost_dx12
    PRIVATE
        wozzits_toolhost::wozzits_toolhost
        d3d12
        dxgi
        dxguid
        d3dcompiler
)

target_include_directories(wozzits_toolhost_dx12
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends
)

target_compile_features(wozzits_toolhost_dx12 PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(wozzits_toolhost_dx12
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()

if(WOZZITS_ENABLE_V8_SCRIPTING)
    target_link_libraries(wozzits_toolhost_dx12
        PRIVATE
            wozzits_v8::wozzits_v8
    )

    target_compile_definitions(wozzits_toolhost_dx12
        PRIVATE
            WOZZITS_ENABLE_V8_SCRIPTING
    )
endif()

add_custom_command(TARGET wozzits_toolhost_dx12 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/../../../wozzits-window-engine/window_engine/resources"
        "$<TARGET_FILE_DIR:wozzits_toolhost_dx12>/resources"
    COMMENT "Copying Wozzits resources for wozzits_toolhost_dx12"
)
