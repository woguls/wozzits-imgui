#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <window/window2.h>
#include <event/platform_event.h>

#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>

#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <d3d12.h>

#include <cassert>
#include <cstdint>
#include <iostream>

namespace
{
    struct ImGuiDx12SrvAllocator
    {
        ID3D12DescriptorHeap* heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
        D3D12_CPU_DESCRIPTOR_HANDLE heap_start_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE heap_start_gpu{};
        UINT heap_handle_increment = 0;
        ImVector<int> free_indices;

        void create(ID3D12Device* device, ID3D12DescriptorHeap* descriptor_heap)
        {
            assert(heap == nullptr);
            assert(free_indices.empty());

            heap = descriptor_heap;

            D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
            heap_type = desc.Type;

            heap_start_cpu = heap->GetCPUDescriptorHandleForHeapStart();
            heap_start_gpu = heap->GetGPUDescriptorHandleForHeapStart();

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
                static_cast<int>((cpu.ptr - heap_start_cpu.ptr) / heap_handle_increment);

            const int gpu_index =
                static_cast<int>((gpu.ptr - heap_start_gpu.ptr) / heap_handle_increment);

            assert(cpu_index == gpu_index);

            free_indices.push_back(cpu_index);
        }
    };

    static ImGuiDx12SrvAllocator g_imgui_srv_allocator{};
}

namespace
{
    ID3D12DescriptorHeap* create_imgui_srv_heap(ID3D12Device* device)
    {
        assert(device != nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 64;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        ID3D12DescriptorHeap* heap = nullptr;
        const HRESULT hr =
            device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));

        assert(SUCCEEDED(hr));
        assert(heap != nullptr);

        return heap;
    }

    void poll_window_events(
        const wz::window::WindowHandle& window,
        bool& running,
        wz::gpu::Device& device)
    {
        wz::window::pump_messages();

        PlatformEvent event{};

        while (wz::window::poll_event(window, event))
        {
            switch (event.type)
            {
            case PlatformEvent::Type::Close:
                running = false;
                break;

            case PlatformEvent::Type::Resize:
                if (event.resize.width > 0 && event.resize.height > 0)
                {
                    wz::gpu::resize(
                        device,
                        event.resize.width,
                        event.resize.height);
                }
                break;

            default:
                break;
            }
        }

        if (wz::window::window_should_close(window))
            running = false;
    }
}

int main()
{
    wz::window::WindowDesc desc{};
    desc.title = "Wozzits ImGui Toolhost";
    desc.width = 1280;
    desc.height = 720;

    wz::window::WindowHandle window =
        wz::window::create_window(desc);

    void* hwnd =
        wz::window::get_native_handle(window);

    assert(hwnd != nullptr);

    wz::gpu::Device device =
        wz::gpu::create_device(window);

    assert(device.valid());

    ID3D12Device* dx_device =
        wz::gpu::dx12::internal::get_device(device);

    ID3D12GraphicsCommandList* cmd =
        wz::gpu::dx12::internal::get_command_list(device);

    ID3D12CommandQueue* queue =
        wz::gpu::dx12::internal::get_command_queue(device);

    assert(dx_device != nullptr);
    assert(cmd != nullptr);
    assert(queue != nullptr);


    ID3D12DescriptorHeap* imgui_srv_heap =
        create_imgui_srv_heap(dx_device);

    g_imgui_srv_allocator.create(dx_device, imgui_srv_heap);



    IMGUI_CHECKVERSION();

    const bool imgui_context_created =
        wz::imgui::create_context();

    assert(imgui_context_created);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Keep docking/viewports off for now.

    assert(imgui_context_created);
    assert(wz::imgui::has_context());

    ImGui::StyleColorsDark();

    const bool win32_ok =
        ImGui_ImplWin32_Init(hwnd);

    assert(win32_ok);

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = dx_device;
    init_info.CommandQueue =
        wz::gpu::dx12::internal::get_command_queue(device);
    init_info.NumFramesInFlight =
        static_cast<int>(wz::gpu::dx12::internal::get_backbuffer_count());
    init_info.RTVFormat =
        wz::gpu::dx12::internal::get_backbuffer_format();
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;

    init_info.SrvDescriptorHeap = imgui_srv_heap;

    init_info.SrvDescriptorAllocFn =
        [](ImGui_ImplDX12_InitInfo*,
            D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
        {
            g_imgui_srv_allocator.alloc(out_cpu, out_gpu);
        };

    init_info.SrvDescriptorFreeFn =
        [](ImGui_ImplDX12_InitInfo*,
            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE gpu)
        {
            g_imgui_srv_allocator.free(cpu, gpu);
        };

    const bool dx12_ok =
        ImGui_ImplDX12_Init(&init_info);

    assert(dx12_ok);

    std::cout << "Wozzits ImGui toolhost running\n";

    bool running = true;
    std::uint64_t frame_index = 0;

    while (running)
    {
        poll_window_events(window, running, device);

        if (!running)
            break;

        wz::gpu::begin_frame(device);
        wz::gpu::clear(device, 0.0f, 0.15f, 0.35f, 1.0f);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGuiIO& _io = ImGui::GetIO();
        _io.DisplaySize = ImVec2(
            static_cast<float>(wz::gpu::dx12::internal::get_width(device)),
            static_cast<float>(wz::gpu::dx12::internal::get_height(device)));

        _io.DeltaTime = 1.0f / 60.0f;

        ImGui::NewFrame();
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(40.0f, 40.0f),
            ImVec2(300.0f, 160.0f),
            IM_COL32(255, 0, 0, 255));

        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(420.0f, 180.0f), ImGuiCond_Always);
        ImGui::Begin(
            "Wozzits Toolhost",
            nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        ImGui::Text("Hello from Wozzits ImGui");
        ImGui::Text(
            "Frame: %llu",
            static_cast<unsigned long long>(frame_index));
        ImGui::Button("Visible button");
        ImGui::Text("This executable lives above window-engine.");


        ImGui::End();

        ImGui::Render();

        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

        ImGuiIO& io = ImGui::GetIO();

        ImDrawData* draw_data = ImGui::GetDrawData();

        if (draw_data->Textures != nullptr)
        {
            for (ImTextureData* tex : *draw_data->Textures)
            {
                if (tex->Status != ImTextureStatus_OK)
                {
                    ImGui_ImplDX12_UpdateTexture(tex);
                }
            }
        }

        ID3D12DescriptorHeap* heaps[] =
        {
            imgui_srv_heap
        };

        cmd = wz::gpu::dx12::internal::get_command_list(device);
        assert(cmd != nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            wz::gpu::dx12::internal::get_current_rtv(device);

        cmd->OMSetRenderTargets(
            1,
            &rtv,
            FALSE,
            nullptr);

        cmd->SetDescriptorHeaps(1, heaps);

        ImGui_ImplDX12_RenderDrawData(
            draw_data,
            cmd);

        wz::gpu::end_frame(device);
        wz::gpu::present(device);

        ++frame_index;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();

    wz::imgui::destroy_context();

    if (imgui_srv_heap)
    {
        imgui_srv_heap->Release();
        imgui_srv_heap = nullptr;
    }

    wz::gpu::destroy_device(device);
    wz::window::destroy_window(window);

    return 0;
}