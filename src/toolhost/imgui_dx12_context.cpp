// src/toolhost/imgui_dx12_context.cpp

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <wozzits/toolhost/imgui_dx12_context.h>

#include <gpu/dx12/dx12_internal.h>
#include <window/window2.h>
#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <d3d12.h>

#include <cassert>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace wz::toolhost
{
    // ── SRV descriptor heap allocator ────────────────────────────────────────

    namespace detail
    {
        struct ImGuiDx12SrvAllocator
        {
            ID3D12DescriptorHeap*       heap                = nullptr;
            D3D12_DESCRIPTOR_HEAP_TYPE  heap_type           = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
            D3D12_CPU_DESCRIPTOR_HANDLE heap_start_cpu      = {};
            D3D12_GPU_DESCRIPTOR_HANDLE heap_start_gpu      = {};
            UINT                        heap_handle_increment = 0;
            ImVector<int>               free_indices;

            void create(ID3D12Device* device, ID3D12DescriptorHeap* descriptor_heap)
            {
                assert(device != nullptr);
                assert(descriptor_heap != nullptr);
                assert(heap == nullptr);
                assert(free_indices.empty());

                heap = descriptor_heap;

                const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
                heap_type         = desc.Type;
                heap_start_cpu    = heap->GetCPUDescriptorHandleForHeapStart();
                heap_start_gpu    = heap->GetGPUDescriptorHandleForHeapStart();
                heap_handle_increment =
                    device->GetDescriptorHandleIncrementSize(heap_type);

                free_indices.reserve(static_cast<int>(desc.NumDescriptors));
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
                const int cpu_index = static_cast<int>(
                    (cpu.ptr - heap_start_cpu.ptr) / heap_handle_increment);
                const int gpu_index = static_cast<int>(
                    (gpu.ptr - heap_start_gpu.ptr) / heap_handle_increment);
                assert(cpu_index == gpu_index);
                free_indices.push_back(cpu_index);
            }
        };
    } // namespace detail

    // ── File-local context pointer for ImGui callbacks ────────────────────────
    //
    // ImGui_ImplDX12 SRV alloc/free callbacks are bare function pointers with
    // only an ImGui_ImplDX12_InitInfo* context.  We store a file-local pointer
    // to reach the allocator from inside the lambdas.

    static ImGuiDx12ToolhostContext* g_imgui_dx12_context = nullptr;

    // ── Helpers ───────────────────────────────────────────────────────────────

    namespace
    {
        ID3D12DescriptorHeap* create_srv_heap(ID3D12Device* device)
        {
            assert(device != nullptr);

            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = 64;
            desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            ID3D12DescriptorHeap* heap = nullptr;
            const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
            assert(SUCCEEDED(hr));
            assert(heap != nullptr);
            return heap;
        }
    }

    // ── ImGuiDx12ToolhostContext ──────────────────────────────────────────────

    bool ImGuiDx12ToolhostContext::init(
        wz::window::WindowHandle window,
        wz::gpu::Device&         device)
    {
        void* hwnd = window.native;
        if (hwnd == nullptr)
            return false;

        ID3D12Device* dx_device = wz::gpu::dx12::internal::get_device(device);
        if (dx_device == nullptr)
            return false;

        srv_heap_      = create_srv_heap(dx_device);
        srv_allocator_ = new detail::ImGuiDx12SrvAllocator{};
        srv_allocator_->create(dx_device, srv_heap_);

        g_imgui_dx12_context = this;

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
            style.WindowRounding               = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w  = 0.85f;
        }

        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd))
            return false;

        wz::window::set_native_message_hook(
            window,
            [](void* h, unsigned int msg, uintptr_t wp, intptr_t lp, void*) -> bool
            {
                return ImGui_ImplWin32_WndProcHandler(
                    static_cast<HWND>(h),
                    static_cast<UINT>(msg),
                    static_cast<WPARAM>(wp),
                    static_cast<LPARAM>(lp)) != 0;
            },
            nullptr);

        ImGui_ImplDX12_InitInfo init_info{};
        init_info.Device          = dx_device;
        init_info.CommandQueue    = wz::gpu::dx12::internal::get_command_queue(device);
        init_info.NumFramesInFlight =
            static_cast<int>(wz::gpu::dx12::internal::get_backbuffer_count());
        init_info.RTVFormat       = wz::gpu::dx12::internal::get_backbuffer_format();
        init_info.DSVFormat       = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = srv_heap_;

        init_info.SrvDescriptorAllocFn =
            [](ImGui_ImplDX12_InitInfo*,
               D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
               D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
            {
                assert(g_imgui_dx12_context != nullptr);
                g_imgui_dx12_context->srv_allocator_->alloc(out_cpu, out_gpu);
            };

        init_info.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*,
               D3D12_CPU_DESCRIPTOR_HANDLE cpu,
               D3D12_GPU_DESCRIPTOR_HANDLE gpu)
            {
                assert(g_imgui_dx12_context != nullptr);
                g_imgui_dx12_context->srv_allocator_->free(cpu, gpu);
            };

        if (!ImGui_ImplDX12_Init(&init_info))
            return false;

        ready_ = true;
        return true;
    }

    void ImGuiDx12ToolhostContext::shutdown(wz::window::WindowHandle window)
    {
        if (ready_)
        {
            ImGui_ImplDX12_Shutdown();
            wz::window::set_native_message_hook(window, nullptr, nullptr);
            ImGui_ImplWin32_Shutdown();
            ready_ = false;
        }

        if (wz::imgui::has_context())
            wz::imgui::destroy_context();

        if (srv_allocator_ != nullptr)
        {
            srv_allocator_->destroy();
            delete srv_allocator_;
            srv_allocator_ = nullptr;
        }

        if (srv_heap_ != nullptr)
        {
            srv_heap_->Release();
            srv_heap_ = nullptr;
        }

        if (g_imgui_dx12_context == this)
            g_imgui_dx12_context = nullptr;
    }

    void ImGuiDx12ToolhostContext::begin_frame(
        wz::gpu::Device&                device,
        const wz::engine::FrameContext& fctx)
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGuiIO& io = ImGui::GetIO();

        io.DisplaySize = ImVec2(
            static_cast<float>(wz::gpu::dx12::internal::get_width(device)),
            static_cast<float>(wz::gpu::dx12::internal::get_height(device)));

        const double dt = fctx.frame.delta_seconds();
        io.DeltaTime = dt > 0.0 ? static_cast<float>(dt) : (1.0f / 60.0f);

        ImGui::NewFrame();
    }

    void ImGuiDx12ToolhostContext::render(wz::gpu::Device& device)
    {
        ImDrawData* draw_data = ImGui::GetDrawData();

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

        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        ID3D12DescriptorHeap* heaps[] = { srv_heap_ };
        cmd->SetDescriptorHeaps(1, heaps);

        ImGui_ImplDX12_RenderDrawData(draw_data, cmd);

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

} // namespace wz::toolhost
