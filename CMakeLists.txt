cmake_minimum_required(VERSION 3.20)

project(wozzits_imgui LANGUAGES CXX)

option(WOZZITS_IMGUI_BUILD_TESTS "Build wozzits-imgui tests" ON)
option(WOZZITS_IMGUI_BUILD_EXAMPLES "Build wozzits-imgui examples" ON)
option(WOZZITS_ENABLE_V8_SCRIPTING "Build with V8 scripting support" ON)

if(WOZZITS_ENABLE_V8_SCRIPTING)
    # V8 is built in Release mode; its objects embed _ITERATOR_DEBUG_LEVEL=0.
    # Force the same value project-wide so lld-link sees no mismatch.
    add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    if(WOZZITS_ENABLE_V8_SCRIPTING)
        # V8 uses static CRT (/MT) — its allocator shim requires it.  Force /MT
        # for all configs so every object in the link agrees.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
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

    # ── Resolve window_engine (required by wozzits_toolhost) ──────────────────
    if(NOT TARGET window_engine)
        find_package(window_engine QUIET)
        if(NOT TARGET window_engine)
            if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../wozzits-window-engine/CMakeLists.txt")
                add_subdirectory(
                    "${CMAKE_CURRENT_LIST_DIR}/../wozzits-window-engine"
                    "${CMAKE_BINARY_DIR}/window_engine_build"
                )
            else()
                message(FATAL_ERROR
                    "window_engine not found — required by wozzits_toolhost. "
                    "Place wozzits-window-engine as a sibling of wozzits-imgui.")
            endif()
        endif()
    endif()

    # ── wozzits_toolhost ──────────────────────────────────────────────────────
    #
    # Reusable DX12 + ImGui toolhost infrastructure:
    #   - ToolConsole   (thread-safe log console + ImGui panel)
    #   - ImGuiDx12ToolhostContext  (SRV heap, init/shutdown, begin/render)
    #   - BenchmarkRecorder  (row accumulator + CSV export pipeline)

    add_library(wozzits_toolhost STATIC
        src/toolhost/tool_console.cpp
        src/toolhost/imgui_dx12_context.cpp
        src/toolhost/benchmark_recorder.cpp

        "${WOZZITS_IMGUI_EXTERNAL_DIR}/backends/imgui_impl_win32.cpp"
        "${WOZZITS_IMGUI_EXTERNAL_DIR}/backends/imgui_impl_dx12.cpp"
    )

    add_library(wozzits_toolhost::wozzits_toolhost ALIAS wozzits_toolhost)

    target_include_directories(wozzits_toolhost
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE
            "${WOZZITS_IMGUI_EXTERNAL_DIR}"
            "${WOZZITS_IMGUI_EXTERNAL_DIR}/backends"
    )

    target_link_libraries(wozzits_toolhost
        PUBLIC
            wozzits_imgui::wozzits_imgui
            window_engine
        PRIVATE
            d3d12
            dxgi
            dxguid
    )

    target_compile_features(wozzits_toolhost PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_definitions(wozzits_toolhost
            PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        )
        set_target_properties(wozzits_toolhost
            PROPERTIES FOLDER "toolhost"
        )
    endif()

    # ── Examples ──────────────────────────────────────────────────────────────
    add_subdirectory(examples/hello_imgui_win32_dx12)
    add_subdirectory(examples/wozzits_toolhost_dx12)
    add_subdirectory(examples/mesh_asset_tool_dx12)
    add_subdirectory(examples/scalar_field_tool_dx12)
    add_subdirectory(examples/mesh_wireframe_toolhost_dx12)
    add_subdirectory(examples/gaussian_splat_toolhost_dx12)

endif()

