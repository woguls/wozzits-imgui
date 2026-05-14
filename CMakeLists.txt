cmake_minimum_required(VERSION 3.20)

project(wozzits_imgui LANGUAGES CXX)

option(WOZZITS_IMGUI_BUILD_TESTS "Build wozzits-imgui tests" ON)
option(WOZZITS_IMGUI_BUILD_EXAMPLES "Build wozzits-imgui examples" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(WOZZITS_IMGUI_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")

if(NOT EXISTS "${WOZZITS_IMGUI_EXTERNAL_DIR}/imgui.h")
    message(FATAL_ERROR
        "Dear ImGui submodule not found. Run:\n"
        "  git submodule update --init --recursive"
    )
endif()


add_library(wozzits_imgui STATIC
    src/imgui/imgui_context.cpp

    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
)

add_library(wozzits_imgui::wozzits_imgui ALIAS wozzits_imgui)

target_include_directories(wozzits_imgui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"

    PRIVATE
        "${WOZZITS_IMGUI_EXTERNAL_DIR}"
)

target_compile_features(wozzits_imgui PUBLIC cxx_std_20)

if(WOZZITS_IMGUI_BUILD_TESTS)
    enable_testing()

    add_executable(imgui_context_smoke
        tests/imgui/imgui_context_smoke.cpp
    )

    target_link_libraries(imgui_context_smoke
        PRIVATE
            wozzits_imgui::wozzits_imgui
    )

    add_test(NAME imgui_context_smoke COMMAND imgui_context_smoke)
endif()

if(WOZZITS_IMGUI_BUILD_EXAMPLES)
    add_subdirectory(examples/hello_imgui_win32_dx12)
    add_subdirectory(examples/wozzits_toolhost_dx12)
    add_subdirectory(examples/mesh_asset_tool_dx12)
    add_subdirectory(examples/scalar_field_tool_dx12)
endif()