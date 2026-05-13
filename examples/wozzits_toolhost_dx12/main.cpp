#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <engine/engine.h>
#include <engine/game_app.h>

#include <window/window2.h>

#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>

#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

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

    struct ToolhostState
    {
        wz::app::GameApp app{};

        ID3D12DescriptorHeap* imgui_srv_heap = nullptr;
        ImGuiDx12SrvAllocator imgui_srv_allocator{};

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

        draw_toolhost_panel(state, fctx);

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

    wz::app::shutdown(state.app);

    g_toolhost_state = nullptr;

    return 0;
}