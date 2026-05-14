#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <mutex>
#include <string>
#include <vector>

#include <engine/engine.h>
#include <engine/game_app.h>

#include <logging/logging.h>
#include <logging/logger.h>

#include <window/window2.h>

#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>

#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <engine/game_app_benchmark.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam);

#include <d3d12.h>

#include <cassert>
#include <cstdint>
#include <iostream>

namespace
{
    struct ImGuiDx12SrvAllocator
    {
        ID3D12DescriptorHeap* heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type =
            D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;

        D3D12_CPU_DESCRIPTOR_HANDLE heap_start_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE heap_start_gpu{};

        UINT heap_handle_increment = 0;
        ImVector<int> free_indices;

        void create(
            ID3D12Device* device,
            ID3D12DescriptorHeap* descriptor_heap)
        {
            assert(device != nullptr);
            assert(descriptor_heap != nullptr);
            assert(heap == nullptr);
            assert(free_indices.empty());

            heap = descriptor_heap;

            const D3D12_DESCRIPTOR_HEAP_DESC desc =
                heap->GetDesc();

            heap_type = desc.Type;

            heap_start_cpu =
                heap->GetCPUDescriptorHandleForHeapStart();

            heap_start_gpu =
                heap->GetGPUDescriptorHandleForHeapStart();

            heap_handle_increment =
                device->GetDescriptorHandleIncrementSize(heap_type);

            free_indices.reserve(
                static_cast<int>(desc.NumDescriptors));

            for (int i = static_cast<int>(desc.NumDescriptors); i > 0; --i)
                free_indices.push_back(i - 1);
        }

        void destroy()
        {
            heap = nullptr;
            free_indices.clear();
        }

        void alloc(
            D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
        {
            assert(out_cpu != nullptr);
            assert(out_gpu != nullptr);
            assert(free_indices.Size > 0);

            const int index = free_indices.back();
            free_indices.pop_back();

            out_cpu->ptr =
                heap_start_cpu.ptr +
                static_cast<SIZE_T>(index) * heap_handle_increment;

            out_gpu->ptr =
                heap_start_gpu.ptr +
                static_cast<UINT64>(index) * heap_handle_increment;
        }

        void free(
            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE gpu)
        {
            const int cpu_index =
                static_cast<int>(
                    (cpu.ptr - heap_start_cpu.ptr) /
                    heap_handle_increment);

            const int gpu_index =
                static_cast<int>(
                    (gpu.ptr - heap_start_gpu.ptr) /
                    heap_handle_increment);

            assert(cpu_index == gpu_index);

            free_indices.push_back(cpu_index);
        }
    };

    struct ConsoleLine
    {
        wz::LogLevel level = wz::LogLevel::Info;
        uint64_t sequence = 0;
        uint64_t event_ticks = 0;
        std::string text;
    };

    struct ToolConsole
    {
        std::mutex mutex;
        std::vector<ConsoleLine> lines;
        bool auto_scroll = true;
        bool scroll_to_bottom = false;

        void push(const wz::logging::LogRecordView& record)
        {
            if (record.text == nullptr)
                return;

            std::lock_guard lock(mutex);

            lines.push_back(ConsoleLine{
                .level = record.level,
                .sequence = record.sequence,
                .event_ticks = record.event_ticks,
                .text = std::string(
                    record.text,
                    record.text + record.text_size),
                });

            constexpr std::size_t kMaxLines = 4000;

            if (lines.size() > kMaxLines)
            {
                lines.erase(
                    lines.begin(),
                    lines.begin()
                    + static_cast<std::ptrdiff_t>(
                        lines.size() - kMaxLines));
            }

            scroll_to_bottom = true;
        }
    };

    struct ToolhostState
    {
        wz::app::GameApp app{};

        ID3D12DescriptorHeap* imgui_srv_heap = nullptr;
        ImGuiDx12SrvAllocator imgui_srv_allocator{};

        ToolConsole console{};

        bool imgui_ready = false;
    };

    ID3D12DescriptorHeap* create_imgui_srv_heap(ID3D12Device* device)
    {
        assert(device != nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 64;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        ID3D12DescriptorHeap* heap = nullptr;

        const HRESULT hr =
            device->CreateDescriptorHeap(
                &desc,
                IID_PPV_ARGS(&heap));

        assert(SUCCEEDED(hr));
        assert(heap != nullptr);

        return heap;
    }

    static ToolhostState* g_toolhost_state = nullptr;

    bool init_imgui_for_app(ToolhostState& state)
    {
        wz::gpu::Device& device =
            state.app.ctx.device;

        void* hwnd =
            wz::window::get_native_handle(state.app.ctx.window);

        if (hwnd == nullptr)
            return false;

        ID3D12Device* dx_device =
            wz::gpu::dx12::internal::get_device(device);

        if (dx_device == nullptr)
            return false;

        state.imgui_srv_heap =
            create_imgui_srv_heap(dx_device);

        state.imgui_srv_allocator.create(
            dx_device,
            state.imgui_srv_heap);

        IMGUI_CHECKVERSION();

        if (!wz::imgui::create_context())
            return false;

        ImGuiIO& io = ImGui::GetIO();

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        ImGuiStyle& style = ImGui::GetStyle();

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 0.85f;
        }

        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd))
            return false;

        wz::window::set_native_message_hook(
            state.app.ctx.window,
            [](void* hwnd,
                unsigned int msg,
                uintptr_t wparam,
                intptr_t lparam,
                void*) -> bool
            {
                return ImGui_ImplWin32_WndProcHandler(
                    static_cast<HWND>(hwnd),
                    static_cast<UINT>(msg),
                    static_cast<WPARAM>(wparam),
                    static_cast<LPARAM>(lparam)) != 0;
            },
            nullptr);
        ImGui_ImplDX12_InitInfo init_info{};
        init_info.Device = dx_device;
        init_info.CommandQueue =
            wz::gpu::dx12::internal::get_command_queue(device);

        init_info.NumFramesInFlight =
            static_cast<int>(
                wz::gpu::dx12::internal::get_backbuffer_count());

        init_info.RTVFormat =
            wz::gpu::dx12::internal::get_backbuffer_format();

        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = state.imgui_srv_heap;

        init_info.SrvDescriptorAllocFn =
            [](ImGui_ImplDX12_InitInfo*,
                D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
            {
                assert(g_toolhost_state != nullptr);
                g_toolhost_state->imgui_srv_allocator.alloc(
                    out_cpu,
                    out_gpu);
            };

        init_info.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*,
                D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                D3D12_GPU_DESCRIPTOR_HANDLE gpu)
            {
                assert(g_toolhost_state != nullptr);
                g_toolhost_state->imgui_srv_allocator.free(
                    cpu,
                    gpu);
            };

        if (!ImGui_ImplDX12_Init(&init_info))
            return false;

        state.imgui_ready = true;
        return true;
    }

    void shutdown_imgui(ToolhostState& state)
    {
        if (state.imgui_ready)
        {
            ImGui_ImplDX12_Shutdown();
            wz::window::set_native_message_hook(
                state.app.ctx.window,
                nullptr,
                nullptr);

            ImGui_ImplWin32_Shutdown();
            state.imgui_ready = false;
        }

        if (wz::imgui::has_context())
            wz::imgui::destroy_context();

        state.imgui_srv_allocator.destroy();

        if (state.imgui_srv_heap != nullptr)
        {
            state.imgui_srv_heap->Release();
            state.imgui_srv_heap = nullptr;
        }
    }

    void begin_imgui_frame(
        ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        wz::gpu::Device& device =
            state.app.ctx.device;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGuiIO& io = ImGui::GetIO();

        const UINT width =
            wz::gpu::dx12::internal::get_width(device);

        const UINT height =
            wz::gpu::dx12::internal::get_height(device);

        io.DisplaySize = ImVec2(
            static_cast<float>(width),
            static_cast<float>(height));

        const double dt =
            fctx.frame.delta_seconds();

        io.DeltaTime =
            dt > 0.0 ? static_cast<float>(dt) : (1.0f / 60.0f);

        ImGui::NewFrame();
    }

    const char* log_level_name(wz::LogLevel level)
    {
        switch (level)
        {
        case wz::LogLevel::Debug:
            return "DEBUG";

        case wz::LogLevel::Info:
            return "INFO";

        case wz::LogLevel::Warning:
            return "WARN";

        case wz::LogLevel::Error:
            return "ERROR";

        case wz::LogLevel::Critical:
            return "CRITICAL";
        }

        return "UNKNOWN";
    }

    void draw_console_panel(ToolhostState& state)
    {
        ImGui::SetNextWindowSize(
            ImVec2(760.0f, 360.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::Begin("Wozzits Console");

        if (ImGui::Button("Clear"))
        {
            std::lock_guard lock(state.console.mutex);
            state.console.lines.clear();
            state.console.scroll_to_bottom = false;
        }

        ImGui::SameLine();

        ImGui::Checkbox(
            "Auto-scroll",
            &state.console.auto_scroll);

        ImGui::Separator();

        if (ImGui::BeginChild(
            "console_scroll",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar))
        {
            std::lock_guard lock(state.console.mutex);

            for (const ConsoleLine& line : state.console.lines)
            {
                ImGui::Text("[%s]", log_level_name(line.level));
                ImGui::SameLine();
                ImGui::TextUnformatted(line.text.c_str());
            }

            if (state.console.auto_scroll && state.console.scroll_to_bottom)
            {
                ImGui::SetScrollHereY(1.0f);
                state.console.scroll_to_bottom = false;
            }
        }

        ImGui::EndChild();
        ImGui::End();
    }

    void draw_benchmark_panel(
        const ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        const wz::app::GameAppBenchmarkSnapshot snap =
            wz::app::benchmark_snapshot(
                state.app,
                fctx);

        ImGui::SetNextWindowPos(
            ImVec2(24.0f, 24.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowSize(
            ImVec2(520.0f, 420.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::Begin("Wozzits Benchmark");

        ImGui::Text("Scene compiler / culler benchmark");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Frame: %llu",
                static_cast<unsigned long long>(snap.frame_index));

            ImGui::Text(
                "dt: %.4f ms",
                snap.dt_seconds * 1000.0);

            ImGui::Text(
                "FPS: %.1f",
                snap.fps);
        }

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Debug object ready: %s",
                snap.debug_object_ready ? "yes" : "no");

            ImGui::Text(
                "Compiled scene valid: %s",
                snap.compiled_scene_valid ? "yes" : "no");

            ImGui::Text(
                "Scene nodes: %llu",
                static_cast<unsigned long long>(snap.scene_nodes));

            ImGui::Text(
                "Dirty transform nodes: %llu",
                static_cast<unsigned long long>(snap.dirty_nodes));
        }

        if (ImGui::CollapsingHeader("Jobs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Total jobs: %u", snap.job_count);
            ImGui::Text("Total job time: %.3f ms", snap.total_job_ms);
            ImGui::Text("Render prep time: %.3f ms", snap.render_prep_job_ms);
            ImGui::Text(
                "Slowest: %s %.3f ms",
                snap.slowest_job_name ? snap.slowest_job_name : "<none>",
                snap.slowest_job_ms);

            ImGui::Separator();

            if (ImGui::BeginTable("job timings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("ms");
                ImGui::TableHeadersRow();

                for (uint32_t i = 0; i < snap.job_count; ++i)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(
                        snap.jobs[i].name ? snap.jobs[i].name : "<unnamed>");

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", snap.jobs[i].ms);
                }

                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader("Render prep", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Path: %s",
                snap.render_prep_path ? snap.render_prep_path : "Unknown");
        }

        if (ImGui::CollapsingHeader("Render commands", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Opaque: %llu",
                static_cast<unsigned long long>(snap.opaque_commands));

            ImGui::Text(
                "Splats: %llu",
                static_cast<unsigned long long>(snap.splat_commands));

            ImGui::Text(
                "Transparent: %llu",
                static_cast<unsigned long long>(snap.transparent_commands));

            ImGui::Text(
                "Particles: %llu",
                static_cast<unsigned long long>(snap.particle_commands));

            ImGui::Separator();

            ImGui::Text(
                "Total: %llu",
                static_cast<unsigned long long>(snap.total_commands));
        }

        if (ImGui::CollapsingHeader("Frame allocations", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Owned bytes: %llu",
                static_cast<unsigned long long>(snap.bytes_owned));

            ImGui::Text(
                "Allocated this frame: %llu",
                static_cast<unsigned long long>(
                    snap.bytes_allocated_this_frame));

            ImGui::Text(
                "Reallocations this frame: %u",
                snap.reallocations_this_frame);
        }

        ImGui::End();
    }

    void draw_toolhost_panel(
        const ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        const wz::app::GameApp& app = state.app;

        ImGui::SetNextWindowPos(
            ImVec2(24.0f, 24.0f),
            ImGuiCond_Once);

        ImGui::SetNextWindowSize(
            ImVec2(420.0f, 220.0f),
            ImGuiCond_Once);

        ImGui::Begin("Wozzits Toolhost");

        ImGui::Text("Running GameApp with ImGui overlay");
        ImGui::Separator();

        ImGui::Text(
            "Frame: %llu",
            static_cast<unsigned long long>(fctx.frame.index));

        ImGui::Text(
            "Camera: x=%.3f y=%.3f",
            app.camera.x,
            app.camera.y);

        ImGui::Text(
            "Debug objects ready: %s",
            app.debug_object.ready ? "yes" : "no");

        ImGui::Text(
            "Compiled scene valid: %s",
            app.debug_object.compiled_scene_valid ? "yes" : "no");

        ImGui::End();
    }

    void render_imgui(
        ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        wz::gpu::Device& device =
            state.app.ctx.device;

        begin_imgui_frame(state, fctx);

        draw_benchmark_panel(state, fctx);
        draw_console_panel(state);

        ImGui::Render();

        ImDrawData* draw_data =
            ImGui::GetDrawData();

        if (draw_data->Textures != nullptr)
        {
            for (ImTextureData* tex : *draw_data->Textures)
            {
                if (tex->Status != ImTextureStatus_OK)
                    ImGui_ImplDX12_UpdateTexture(tex);
            }
        }

        ID3D12GraphicsCommandList* cmd =
            wz::gpu::dx12::internal::get_command_list(device);

        assert(cmd != nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            wz::gpu::dx12::internal::get_current_rtv(device);

        cmd->OMSetRenderTargets(
            1,
            &rtv,
            FALSE,
            nullptr);

        ID3D12DescriptorHeap* heaps[] =
        {
            state.imgui_srv_heap
        };

        cmd->SetDescriptorHeaps(1, heaps);

        ImGui_ImplDX12_RenderDrawData(
            draw_data,
            cmd);

        ImGuiIO& io = ImGui::GetIO();

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    void toolhost_update(
        wz::engine::Context& ctx,
        wz::engine::FrameContext& fctx,
        void* user_data)
    {
        auto* state =
            static_cast<ToolhostState*>(user_data);

        assert(state != nullptr);

        wz::app::update(
            ctx,
            fctx,
            state->app);

        if (!ctx.running)
            return;

        wz::gpu::begin_frame(state->app.ctx.device);
        wz::gpu::clear(
            state->app.ctx.device,
            0.0f,
            0.15f,
            0.35f,
            1.0f);

        wz::app::render_contents(
            state->app,
            fctx);

        render_imgui(
            *state,
            fctx);





        wz::gpu::end_frame(state->app.ctx.device);
        wz::gpu::present(state->app.ctx.device);
    }
}

int main()
{
    ToolhostState state{};
    g_toolhost_state = &state;

    if (!wz::app::init(state.app))
    {
        g_toolhost_state = nullptr;
        return 1;
    }

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            auto* state = static_cast<ToolhostState*>(user);

            if (!state)
                return;

            state->console.push(record);
        },
        &state);

    if (!init_imgui_for_app(state))
    {
        shutdown_imgui(state);
        wz::app::shutdown(state.app);
        g_toolhost_state = nullptr;
        return 1;
    }

    wz::engine::run(
        toolhost_update,
        &state);


    shutdown_imgui(state);

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        nullptr,
        nullptr);

    wz::logging::wait_until_idle(state.app.ctx.logger);

    wz::app::shutdown(state.app);

    g_toolhost_state = nullptr;

    return 0;
}