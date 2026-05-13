#include <engine/game_app.h>
#include <engine/engine.h>

#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>

#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

int main()
{
    wz::app::GameApp app{};

    if (!wz::app::init(app))
        return 1;

    // Use app.ctx.window and app.ctx.device for ImGui backend init.
    void* hwnd = wz::window::get_native_handle(app.ctx.window);

    ID3D12Device* dx_device =
        wz::gpu::dx12::internal::get_device(app.ctx.device);

    ID3D12DescriptorHeap* imgui_srv_heap =
        create_imgui_srv_heap(dx_device);

    g_imgui_srv_allocator.create(dx_device, imgui_srv_heap);

    IMGUI_CHECKVERSION();
    wz::imgui::create_context();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = dx_device;
    init_info.CommandQueue =
        wz::gpu::dx12::internal::get_command_queue(app.ctx.device);
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

    ImGui_ImplDX12_Init(&init_info);

    // Run loop needs FrameContext creation.
    // See note below.

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    wz::imgui::destroy_context();

    g_imgui_srv_allocator.destroy();
    imgui_srv_heap->Release();

    wz::app::shutdown(app);
    return 0;
}