
#include <Windows.h>

#include <mutex>
#include <string>
#include <vector>
#include <cmath>

#include <math/mat4.h>
#include <math/math_types.h>
#include <math/projection.h>

#include <engine/assets/engine_asset_library.h>
#include <engine/assets/mesh/mesh.h>
#include <engine/assets/mesh_asset_module.h>


#include <logging/logging.h>
#include <logging/logger.h>

#include <window/window2.h>

#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>
#include <gpu/mesh.h>
#include <gpu/dx12/dx12_mesh_wireframe_debug.h>

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
    void set_identity(float m[16])
    {
        for (int i = 0; i < 16; ++i)
            m[i] = 0.0f;

        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
    }

    void copy_mat4(float out[16], const wz::math::Mat4& m)
    {
        for (int i = 0; i < 16; ++i)
            out[i] = m.m[i];
    }

    wz::math::Mat4 make_world_translate(float x, float y, float z)
    {
        wz::math::Mat4 world = wz::math::mat4_identity();

        world.m[12] = x;
        world.m[13] = y;
        world.m[14] = z;

        return world;
    }

    wz::math::Mat4 make_debug_view_proj(int width, int height)
    {
        constexpr float Pi = 3.14159265358979323846f;

        const float fov = 70.0f * Pi / 180.0f;

        const float aspect =
            (width > 0 && height > 0)
            ? static_cast<float>(width) / static_cast<float>(height)
            : 800.0f / 600.0f;

        wz::math::Mat4 view = wz::math::mat4_identity();

        // Keep this consistent with the existing game_app debug camera path:
        // objects are placed at positive z, and the view matrix is identity.
        view.m[12] = 0.0f;
        view.m[13] = 0.0f;
        view.m[14] = 0.0f;

        const wz::math::Mat4 projection =
            wz::math::projection_perspective_dx(
                fov,
                aspect,
                0.1f,
                100.0f
            );

        return wz::math::mul(projection, view);
    }

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

    struct MeshAssetToolCamera
    {
        float yaw = 0.0f;
        float pitch = 0.25f;
        float distance = 6.0f;

        float target_x = 0.0f;
        float target_y = 0.0f;
        float target_z = 0.0f;
    };

    struct MeshAssetToolState
    {
        wz::Logger logger{};

        wz::window::WindowHandle window{};
        wz::gpu::Device device{};

        std::unique_ptr<wz::engine::assets::EngineAssetLibrary> assets;

        wz::engine::assets::MeshAsset mesh_asset{};
        wz::engine::assets::MeshHandle mesh_handle{};
        const wz::engine::assets::MeshData* mesh_data = nullptr;

        wz::gpu::GPUHandle gpu_mesh{};

        MeshAssetToolCamera camera{};

        ID3D12DescriptorHeap* imgui_srv_heap = nullptr;
        ImGuiDx12SrvAllocator imgui_srv_allocator{};
        bool imgui_ready = false;
    };

    static MeshAssetToolState* g_mesh_tool_state = nullptr;

    wz::math::Mat4 make_mesh_tool_world()
    {
        return wz::math::mat4_identity();
    }

    wz::math::Vec3 subtract(
        const wz::math::Vec3& a,
        const wz::math::Vec3& b)
    {
        return wz::math::Vec3{
            a.x - b.x,
            a.y - b.y,
            a.z - b.z,
        };
    }

    float dot(
        const wz::math::Vec3& a,
        const wz::math::Vec3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    wz::math::Vec3 cross(
        const wz::math::Vec3& a,
        const wz::math::Vec3& b)
    {
        return wz::math::Vec3{
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    }

    wz::math::Vec3 normalize(
        const wz::math::Vec3& v)
    {
        const float len =
            std::sqrt(dot(v, v));

        if (len <= 0.000001f)
        {
            return wz::math::Vec3{ 0.0f, 0.0f, 1.0f };
        }

        const float inv = 1.0f / len;

        return wz::math::Vec3{
            v.x * inv,
            v.y * inv,
            v.z * inv,
        };
    }

    wz::math::Mat4 make_look_at_dx(
        const wz::math::Vec3& eye,
        const wz::math::Vec3& target,
        const wz::math::Vec3& up)
    {
        const wz::math::Vec3 zaxis =
            normalize(subtract(target, eye));

        const wz::math::Vec3 xaxis =
            normalize(cross(up, zaxis));

        const wz::math::Vec3 yaxis =
            cross(zaxis, xaxis);

        wz::math::Mat4 view = wz::math::mat4_identity();

        view.m[0] = xaxis.x;
        view.m[1] = yaxis.x;
        view.m[2] = zaxis.x;
        view.m[3] = 0.0f;

        view.m[4] = xaxis.y;
        view.m[5] = yaxis.y;
        view.m[6] = zaxis.y;
        view.m[7] = 0.0f;

        view.m[8] = xaxis.z;
        view.m[9] = yaxis.z;
        view.m[10] = zaxis.z;
        view.m[11] = 0.0f;

        view.m[12] = -dot(xaxis, eye);
        view.m[13] = -dot(yaxis, eye);
        view.m[14] = -dot(zaxis, eye);
        view.m[15] = 1.0f;

        return view;
    }

    wz::math::Mat4 make_mesh_tool_view_proj(
        const MeshAssetToolCamera& camera,
        int width,
        int height)
    {
        constexpr float Pi = 3.14159265358979323846f;

        const float fov = 70.0f * Pi / 180.0f;

        const float aspect =
            (width > 0 && height > 0)
            ? static_cast<float>(width) / static_cast<float>(height)
            : 1280.0f / 720.0f;

        const wz::math::Vec3 target{
            camera.target_x,
            camera.target_y,
            camera.target_z,
        };

        const float cp = std::cos(camera.pitch);
        const float sp = std::sin(camera.pitch);
        const float cy = std::cos(camera.yaw);
        const float sy = std::sin(camera.yaw);

        const wz::math::Vec3 eye{
            target.x + camera.distance * cp * sy,
            target.y + camera.distance * sp,
            target.z - camera.distance * cp * cy,
        };

        const wz::math::Vec3 up{
            0.0f,
            1.0f,
            0.0f,
        };

        const wz::math::Mat4 view =
            make_look_at_dx(
                eye,
                target,
                up);

        const wz::math::Mat4 projection =
            wz::math::projection_perspective_dx(
                fov,
                aspect,
                0.1f,
                100.0f);

        return wz::math::mul(projection, view);
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

    bool init_mesh_tool(MeshAssetToolState& state)
    {
        wz::logging::init_logger(state.logger, {});

        wz::window::WindowDesc desc{};
        desc.title = "Wozzits Mesh Asset Tool";
        desc.width = 1280;
        desc.height = 720;

        state.window =
            wz::window::create_window(desc);

        if (!state.window.valid())
            return false;

        state.device =
            wz::gpu::create_device(state.window);

        if (!state.device.valid())
            return false;

        state.assets =
            std::make_unique<wz::engine::assets::EngineAssetLibrary>(
                state.device,
                state.logger,
                wz::fs::Path{ "resources" });

        using namespace wz::engine::assets;

        state.mesh_asset =
            state.assets->meshes().create_procedural_mesh({
                .name = "debug/procedural_cube",
                .kind = ProceduralMeshKind::Cube,
                });

        if (!state.mesh_asset.valid())
            return false;

        auto shaders =
            state.assets->shaders().create_shader_pair({
                .name = "mesh_wireframe_debug",
                .vertex_path = "shaders/mesh_wireframe/mesh_wireframe_vs.hlsl",
                .pixel_path = "shaders/mesh_wireframe/mesh_wireframe_ps.hlsl",
                });

        if (!shaders.valid())
            return false;

        if (!state.assets->commit())
            return false;

        const auto report =
            state.assets->resolve_all();

        if (!report.ok())
            return false;

        auto shader_handles =
            state.assets->shaders().get_shader_pair(shaders);

        if (!shader_handles.valid())
            return false;

        state.mesh_handle =
            state.assets->meshes().get_mesh(state.mesh_asset);

        if (!state.mesh_handle.valid())
            return false;

        state.mesh_data =
            state.assets->meshes().get_mesh_data(state.mesh_handle);

        if (!state.mesh_data || !state.mesh_data->valid())
            return false;

        state.gpu_mesh =
            wz::gpu::upload_mesh(state.device, *state.mesh_data);

        if (!state.gpu_mesh.valid())
            return false;

        wz::gpu::dx12::create_mesh_wireframe_debug_context(
            state.device,
            {
                .vertex_shader = shader_handles.vertex,
                .pixel_shader = shader_handles.pixel,
                .mesh = state.gpu_mesh,
            });

        return true;
    }

    void poll_mesh_tool_events(MeshAssetToolState& state, bool& running)
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

    void render_mesh_preview(MeshAssetToolState& state)
    {
        wz::gpu::dx12::MeshWireframeDebugView view{};

        const wz::math::Mat4 world =
            make_mesh_tool_world();

        const wz::math::Mat4 view_proj =
            make_mesh_tool_view_proj(
                state.camera,
                static_cast<int>(wz::gpu::dx12::internal::get_width(state.device)),
                static_cast<int>(wz::gpu::dx12::internal::get_height(state.device)));

        copy_mat4(view.world, world);
        copy_mat4(view.view_proj, view_proj);

        wz::gpu::dx12::submit_mesh_wireframe_debug_frame(
            state.device,
            view);
    }

    bool init_imgui_for_mesh_tool(MeshAssetToolState& state)
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

        ImGuiStyle& style = ImGui::GetStyle();

        ImGui::StyleColorsDark();

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
        init_info.SrvDescriptorHeap = state.imgui_srv_heap;

        init_info.SrvDescriptorAllocFn =
            [](ImGui_ImplDX12_InitInfo*,
                D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
            {
                assert(g_mesh_tool_state != nullptr);

                g_mesh_tool_state->imgui_srv_allocator.alloc(
                    out_cpu,
                    out_gpu);
            };

        init_info.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*,
                D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                D3D12_GPU_DESCRIPTOR_HANDLE gpu)
            {
                assert(g_mesh_tool_state != nullptr);

                g_mesh_tool_state->imgui_srv_allocator.free(
                    cpu,
                    gpu);
            };

        if (!ImGui_ImplDX12_Init(&init_info))
            return false;

        state.imgui_ready = true;
        return true;
    }

    void shutdown_imgui(MeshAssetToolState& state)
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

    void draw_mesh_asset_panel(MeshAssetToolState& state)
    {
        ImGui::SetNextWindowSize(
            ImVec2(440.0f, 360.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::Begin("Wozzits Mesh Asset Tool");

        ImGui::Text("Mesh preview");
        ImGui::Separator();

        if (state.mesh_data)
        {
            ImGui::Text(
                "Vertices: %u",
                state.mesh_data->vertex_count());

            ImGui::Text(
                "Indices: %u",
                state.mesh_data->index_count());

            ImGui::Text(
                "Triangles: %u",
                state.mesh_data->index_count() / 3);
        }
        else
        {
            ImGui::TextUnformatted("No mesh loaded.");
        }

        ImGui::Separator();

        ImGui::SliderFloat(
            "Yaw",
            &state.camera.yaw,
            -3.14159f,
            3.14159f);

        ImGui::SliderFloat(
            "Pitch",
            &state.camera.pitch,
            -2.90f,
            2.90f);

        ImGui::SliderFloat(
            "Distance",
            &state.camera.distance,
            1.0f,
            30.0f);

        ImGui::DragFloat3(
            "Target",
            &state.camera.target_x,
            0.05f);

        if (ImGui::Button("Reset Camera"))
        {
            state.camera = MeshAssetToolCamera{
                .yaw = 0.0f,
                .pitch = 0.25f,
                .distance = 6.0f,
                .target_x = 0.0f,
                .target_y = 0.0f,
                .target_z = 0.0f,
            };
        }

        ImGui::End();
    }

    void render_imgui(MeshAssetToolState& state)
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

        draw_mesh_asset_panel(state);

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

} // anonymous namespace

int main()
{
    MeshAssetToolState state{};
    g_mesh_tool_state = &state;

    if (!init_mesh_tool(state))
    {
        g_mesh_tool_state = nullptr;
        return 1;
    }

    if (!init_imgui_for_mesh_tool(state))
    {
        wz::gpu::destroy_device(state.device);
        wz::window::destroy_window(state.window);
        wz::logging::shutdown_logger(state.logger);
        g_mesh_tool_state = nullptr;
        return 1;
    }

    bool running = true;

    while (running)
    {
        poll_mesh_tool_events(state, running);

        if (!running)
            break;

        wz::gpu::begin_frame(state.device);
        wz::gpu::clear(state.device, 0.05f, 0.05f, 0.05f, 1.0f);

        render_mesh_preview(state);

        render_imgui(state);

        wz::gpu::end_frame(state.device);
        wz::gpu::present(state.device);
    }

    shutdown_imgui(state);
    wz::gpu::destroy_device(state.device);
    wz::window::destroy_window(state.window);
    wz::logging::shutdown_logger(state.logger);

    g_mesh_tool_state = nullptr;
    return 0;
}