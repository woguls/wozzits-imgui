/*
This tool should become the future Gaea import inspection surface.
Because ScalarFieldFileDesc already has path, width, height, depth, format, and domain_kind,
it maps naturally to raw float heightfields exported from Gaea later. 
see: scalar_field_asset_module.h
*/

#include <Windows.h>
#include <d3d12.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>

#include <window/window2.h>
#include <event/platform_event.h>

#include <logging/logger.h>

#include <gpu/gpu.h>
#include <gpu/gpu_types.h>
#include <gpu/scalar_field_texture.h>
#include <gpu/dx12/dx12.h>
#include <gpu/dx12/dx12_internal.h>

#include <engine/assets/engine_asset_library.h>
#include <engine/assets/scalar_field/scalar_field.h>
#include <engine/assets/scalar_field_asset_module.h>

#include <file/filesystem.h>

#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>



extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam);

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

struct ScalarFieldToolView
{
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float zoom = 1.0f;

    float display_min = 0.0f;
    float display_max = 1.0f;
    bool normalize_for_display = true;
};

struct ScalarFieldToolState
{
    wz::Logger logger{};

    wz::window::WindowHandle window{};
    wz::gpu::Device device{};

    std::unique_ptr<wz::engine::assets::EngineAssetLibrary> assets;

    wz::engine::assets::ScalarFieldAsset scalar_asset{};
    wz::engine::assets::ScalarFieldHandle scalar_handle{};
    const wz::engine::assets::ScalarFieldData* scalar_data = nullptr;

    wz::gpu::GPUHandle scalar_texture{};

    ScalarFieldToolView view{};

    ID3D12DescriptorHeap* imgui_srv_heap = nullptr;
    ImGuiDx12SrvAllocator imgui_srv_allocator{};
    bool imgui_ready = false;
};

static ScalarFieldToolState* g_scalar_field_tool_state = nullptr;

float make_diagnostic_scalar_field_value(
    uint32_t x,
    uint32_t y,
    uint32_t w,
    uint32_t h)
{
    const float nx =
        (static_cast<float>(x) / static_cast<float>(w - 1)) * 2.0f - 1.0f;

    const float ny =
        (static_cast<float>(y) / static_cast<float>(h - 1)) * 2.0f - 1.0f;

    constexpr float angle = 0.65f;
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);

    const float rx = nx * ca - ny * sa;
    const float ry = nx * sa + ny * ca;

    const float r =
        std::sqrt(rx * rx + ry * ry);

    const float rings =
        0.5f + 0.5f * std::sin(32.0f * r + 5.0f * rx);

    const float hill =
        std::max(0.0f, 1.0f - r);

    const float diagonal =
        0.15f * (0.5f + 0.5f * std::sin(12.0f * (rx + 0.35f * ry)));

    return hill * rings + diagonal;
}

bool init_scalar_field_tool(ScalarFieldToolState& state)
{
    wz::logging::init_logger(state.logger, {});

    wz::window::WindowDesc desc{};
    desc.title = "Wozzits Scalar Field Tool";
    desc.width = 1280;
    desc.height = 720;

    state.window = wz::window::create_window(desc);
    if (!state.window.valid())
        return false;

    state.device = wz::gpu::create_device(state.window);
    if (!state.device.valid())
        return false;

    state.assets =
        std::make_unique<wz::engine::assets::EngineAssetLibrary>(
            state.device,
            state.logger,
            wz::fs::Path{ "resources" });

    using namespace wz::engine::assets;

    state.scalar_asset =
        state.assets->scalar_fields().create_procedural_scalar_field({
            .name = "debug/procedural_scalar_field",
            .width = 512,
            .height = 512,
            .depth = 1,
            .generator = ScalarFieldGenerator::Checkerboard,
            .frequency = 12.0f,
            .amplitude = 1.0f,
            .format = ScalarFieldFormat::Float32,
            .domain_kind = ScalarFieldDomainKind::Spatial2D,
            });

    if (!state.scalar_asset.valid())
        return false;

    auto shaders =
        state.assets->shaders().create_shader_pair({
            .name = "scalar_field_debug",
            .vertex_path = "shaders/scalar_field/scalar_field_vs.hlsl",
            .pixel_path = "shaders/scalar_field/scalar_field_ps.hlsl",
            });

    if (!shaders.valid())
        return false;

    if (!state.assets->commit())
        return false;

    const auto report = state.assets->resolve_all();
    if (!report.ok())
        return false;

    auto shader_handles =
        state.assets->shaders().get_shader_pair(shaders);

    if (!shader_handles.valid())
        return false;

    state.scalar_handle =
        state.assets->scalar_fields().get_scalar_field(state.scalar_asset);

    if (!state.scalar_handle.valid())
        return false;

    state.scalar_data =
        state.assets->scalar_fields().get_scalar_field_data(state.scalar_handle);

    if (!state.scalar_data || !state.scalar_data->valid())
        return false;

    state.view.display_min = state.scalar_data->min_value;
    state.view.display_max = state.scalar_data->max_value;
    state.view.normalize_for_display = true;

    state.scalar_texture =
        wz::gpu::upload_scalar_field_texture(
            state.device,
            *state.scalar_data);

    if (!state.scalar_texture.valid())
        return false;

    wz::gpu::dx12::create_scalar_field_debug_context(
        state.device,
        {
            .vertex_shader = shader_handles.vertex,
            .pixel_shader = shader_handles.pixel,
            .scalar_field_texture = state.scalar_texture,
            .display_min = state.view.display_min,
            .display_max = state.view.display_max,
            .normalize_for_display = state.view.normalize_for_display,
        });

    return true;
}

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

bool init_imgui_for_scalar_field_tool(ScalarFieldToolState& state)
{
    void* hwnd =
        wz::window::get_native_handle(state.window);

    if (hwnd == nullptr)
        return false;

    ID3D12Device* dx_device =
        wz::gpu::dx12::internal::get_device(state.device);

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

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 0.85f;
    }

    if (!ImGui_ImplWin32_Init(hwnd))
        return false;

    wz::window::set_native_message_hook(
        state.window,
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
        wz::gpu::dx12::internal::get_command_queue(state.device);

    init_info.NumFramesInFlight =
        static_cast<int>(
            wz::gpu::dx12::internal::get_backbuffer_count());

    init_info.RTVFormat =
        wz::gpu::dx12::internal::get_backbuffer_format();

    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;

    init_info.SrvDescriptorHeap =
        state.imgui_srv_heap;

    init_info.SrvDescriptorAllocFn =
        [](ImGui_ImplDX12_InitInfo*,
            D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
        {
            assert(g_scalar_field_tool_state != nullptr);

            g_scalar_field_tool_state->imgui_srv_allocator.alloc(
                out_cpu,
                out_gpu);
        };

    init_info.SrvDescriptorFreeFn =
        [](ImGui_ImplDX12_InitInfo*,
            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE gpu)
        {
            assert(g_scalar_field_tool_state != nullptr);

            g_scalar_field_tool_state->imgui_srv_allocator.free(
                cpu,
                gpu);
        };

    if (!ImGui_ImplDX12_Init(&init_info))
        return false;

    state.imgui_ready = true;
    return true;
}

void render_scalar_field_preview(ScalarFieldToolState& state)
{
    wz::gpu::dx12::ScalarFieldDebugView view{};
    view.offset_x = state.view.offset_x;
    view.offset_y = state.view.offset_y;
    view.zoom = state.view.zoom;

    wz::gpu::dx12::submit_scalar_field_debug_frame(
        state.device,
        view);
}

void draw_scalar_field_panel(ScalarFieldToolState& state)
{
    ImGui::SetNextWindowSize(
        ImVec2(460.0f, 380.0f),
        ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("Wozzits Scalar Field Tool");

    ImGui::Text("Scalar field preview");
    ImGui::Separator();

    if (state.scalar_data)
    {
        ImGui::Text(
            "Size: %u x %u x %u",
            state.scalar_data->width,
            state.scalar_data->height,
            state.scalar_data->depth);

        ImGui::Text("Samples: %u", state.scalar_data->count());
        ImGui::Text("Min: %.6f", state.scalar_data->min_value);
        ImGui::Text("Max: %.6f", state.scalar_data->max_value);
    }
    else
    {
        ImGui::TextUnformatted("No scalar field loaded.");
    }

    ImGui::Separator();

    ImGui::DragFloat("Offset X", &state.view.offset_x, 0.01f);
    ImGui::DragFloat("Offset Y", &state.view.offset_y, 0.01f);
    ImGui::SliderFloat("Zoom", &state.view.zoom, 0.1f, 32.0f);

    ImGui::Separator();

    ImGui::Checkbox(
        "Normalize for display",
        &state.view.normalize_for_display);

    ImGui::DragFloat(
        "Display min",
        &state.view.display_min,
        0.01f);

    ImGui::DragFloat(
        "Display max",
        &state.view.display_max,
        0.01f);

    ImGui::TextWrapped(
        "For V1, display min/max are initialized into the DX12 debug context. "
        "Live updates should be added with a small update_scalar_field_debug_display() API.");

    ImGui::End();
}

void poll_scalar_field_tool_events(
    ScalarFieldToolState& state,
    bool& running)
{
    wz::window::pump_messages();

    PlatformEvent event{};

    while (wz::window::poll_event(state.window, event))
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
                    state.device,
                    event.resize.width,
                    event.resize.height);
            }
            break;

        default:
            break;
        }
    }

    if (wz::window::window_should_close(state.window))
        running = false;
}

void render_imgui(ScalarFieldToolState& state)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    const UINT width =
        wz::gpu::dx12::internal::get_width(state.device);

    const UINT height =
        wz::gpu::dx12::internal::get_height(state.device);

    io.DisplaySize = ImVec2(
        static_cast<float>(width),
        static_cast<float>(height));

    if (io.DeltaTime <= 0.0f)
        io.DeltaTime = 1.0f / 60.0f;

    ImGui::NewFrame();

    draw_scalar_field_panel(state);

    ImGui::Render();

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
        wz::gpu::dx12::internal::get_command_list(state.device);

    assert(cmd != nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        wz::gpu::dx12::internal::get_current_rtv(state.device);

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

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void shutdown_imgui(ScalarFieldToolState& state)
{
    if (state.imgui_ready)
    {
        ImGui_ImplDX12_Shutdown();

        wz::window::set_native_message_hook(
            state.window,
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

int main()
{
    ScalarFieldToolState state{};
    g_scalar_field_tool_state = &state;

    if (!init_scalar_field_tool(state))
    {
        g_scalar_field_tool_state = nullptr;
        return 1;
    }

    if (!init_imgui_for_scalar_field_tool(state))
    {
        wz::gpu::destroy_device(state.device);
        wz::window::destroy_window(state.window);
        wz::logging::shutdown_logger(state.logger);

        g_scalar_field_tool_state = nullptr;
        return 1;
    }

    bool running = true;

    while (running)
    {
        poll_scalar_field_tool_events(state, running);

        if (!running)
            break;

        wz::gpu::begin_frame(state.device);
        wz::gpu::clear(state.device, 0.02f, 0.02f, 0.02f, 1.0f);

        render_scalar_field_preview(state);
        render_imgui(state);

        wz::gpu::end_frame(state.device);
        wz::gpu::present(state.device);
    }

    shutdown_imgui(state);

    wz::gpu::destroy_device(state.device);
    wz::window::destroy_window(state.window);
    wz::logging::shutdown_logger(state.logger);

    g_scalar_field_tool_state = nullptr;

    return 0;
}