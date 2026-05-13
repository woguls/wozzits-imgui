# ----------------------
# dependency: window_engine
# ----------------------
if(NOT TARGET window_engine)

    find_package(window_engine QUIET)

    if(NOT TARGET window_engine)
        if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../wozzits-window-engine/CMakeLists.txt")
            add_subdirectory(
                "${CMAKE_CURRENT_LIST_DIR}/../../../wozzits-window-engine"
                "${CMAKE_BINARY_DIR}/window_engine_build"
            )
        else()
            message(FATAL_ERROR "window_engine not found")
        endif()
    endif()

endif()

add_executable(wozzits_toolhost_dx12
    main.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends/imgui_impl_win32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/backends/imgui_impl_dx12.cpp
)

target_link_libraries(wozzits_toolhost_dx12
    PRIVATE
        wozzits_imgui::wozzits_imgui
        window_engine

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