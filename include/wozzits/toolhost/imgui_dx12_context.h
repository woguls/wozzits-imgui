#pragma once

// wozzits/toolhost/imgui_dx12_context.h

#include <engine/engine.h>
#include <gpu/gpu.h>
#include <window/window_types.h>

// Forward-declare the COM interface — consumers do not need <d3d12.h>.
struct ID3D12DescriptorHeap;

namespace wz::toolhost
{
    namespace detail { struct ImGuiDx12SrvAllocator; }

    // Owns the ImGui DX12 backend lifetime and the SRV descriptor heap used
    // for ImGui textures.  One instance per window/device pair.
    //
    // Typical per-frame usage:
    //   ctx.begin_frame(device, fctx);
    //   // ... ImGui::* calls ...
    //   ImGui::Render();
    //   ctx.render(device);
    class ImGuiDx12ToolhostContext
    {
    public:
        ImGuiDx12ToolhostContext() = default;

        ImGuiDx12ToolhostContext(const ImGuiDx12ToolhostContext&)            = delete;
        ImGuiDx12ToolhostContext& operator=(const ImGuiDx12ToolhostContext&) = delete;

        bool init(
            wz::window::WindowHandle window,
            wz::gpu::Device&         device);

        void shutdown(
            wz::window::WindowHandle window);

        void begin_frame(
            wz::gpu::Device& device,
            float            delta_seconds);

        void begin_frame(
            wz::gpu::Device&                device,
            const wz::engine::FrameContext& fctx);

        void render(wz::gpu::Device& device);

        bool ready() const noexcept { return ready_; }

    private:
        ID3D12DescriptorHeap*          srv_heap_      = nullptr;
        detail::ImGuiDx12SrvAllocator* srv_allocator_ = nullptr;
        bool                           ready_         = false;
    };

} // namespace wz::toolhost
