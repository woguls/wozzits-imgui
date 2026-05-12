add_executable(hello_imgui_win32_dx12
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/examples/example_win32_directx12/main.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/imgui_demo.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends/imgui_impl_win32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends/imgui_impl_dx12.cpp
)

target_link_libraries(hello_imgui_win32_dx12
    PRIVATE
        wozzits_imgui::wozzits_imgui

        d3d12
        dxgi
        dxguid
        d3dcompiler
)

target_include_directories(hello_imgui_win32_dx12
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends
)

target_compile_features(hello_imgui_win32_dx12 PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(hello_imgui_win32_dx12
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()