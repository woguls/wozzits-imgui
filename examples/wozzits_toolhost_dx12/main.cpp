#include <window/window2.h>
#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>

#include <d3d12.h>

#include <cassert>
#include <iostream>

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

    std::cout << "Wozzits toolhost native access OK\n";
    std::cout << "HWND: " << hwnd << "\n";
    std::cout << "Backbuffer count: "
        << wz::gpu::dx12::internal::get_backbuffer_count()
        << "\n";

    wz::gpu::destroy_device(device);
    wz::window::destroy_window(window);

    return 0;
}