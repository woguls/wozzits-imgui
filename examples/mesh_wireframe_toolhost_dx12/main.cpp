#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <engine/app_context.h>
#include <engine/engine.h>

#include <engine/assets/mesh/mesh.h>
#include <engine/assets/mesh_asset_module.h>
#include <engine/assets/renderable_asset_module.h>
#include <engine/rendering/renderable_gpu_cache.h>
#include <engine/rendering/renderable_debug_runtime.h>
#include <engine/rendering/builtin_render_programs.h>

#include <event/platform_event.h>
#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>
#include <gpu/dx12/dx12_mesh_wireframe_debug.h>

#include <math/mat4.h>
#include <math/projection.h>

#include <logging/logging.h>

#include <wozzits/toolhost/tool_console.h>
#include <wozzits/toolhost/imgui_dx12_context.h>
#include <wozzits/toolhost/benchmark_recorder.h>

#include <imgui.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>

namespace
{
    // ── Camera math ───────────────────────────────────────────────────────────

    struct OrbitCamera
    {
        float yaw      = 0.0f;
        float pitch    = 0.25f;
        float distance = 6.0f;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float target_z = 0.0f;
    };

    wz::math::Mat4 make_view_proj(const OrbitCamera& cam, int width, int height)
    {
        constexpr float kPi = 3.14159265358979323846f;

        const float aspect =
            (width > 0 && height > 0)
            ? static_cast<float>(width) / static_cast<float>(height)
            : 1280.0f / 720.0f;

        const wz::math::Vec3 target{ cam.target_x, cam.target_y, cam.target_z };

        const float cp = std::cos(cam.pitch);
        const float sp = std::sin(cam.pitch);
        const float cy = std::cos(cam.yaw);
        const float sy = std::sin(cam.yaw);

        const wz::math::Vec3 eye{
            target.x + cam.distance * cp * sy,
            target.y + cam.distance * sp,
            target.z - cam.distance * cp * cy,
        };

        // look-at (DX convention: z-forward into screen)
        auto sub = [](const wz::math::Vec3& a, const wz::math::Vec3& b) {
            return wz::math::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
        };
        auto dot = [](const wz::math::Vec3& a, const wz::math::Vec3& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        auto cross = [](const wz::math::Vec3& a, const wz::math::Vec3& b) {
            return wz::math::Vec3{
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x,
            };
        };
        auto normalize = [&](const wz::math::Vec3& v) {
            const float len = std::sqrt(dot(v, v));
            const float inv = len > 1e-6f ? 1.0f / len : 1.0f;
            return wz::math::Vec3{ v.x * inv, v.y * inv, v.z * inv };
        };

        const wz::math::Vec3 z = normalize(sub(target, eye));
        const wz::math::Vec3 x = normalize(cross(wz::math::Vec3{0,1,0}, z));
        const wz::math::Vec3 y = cross(z, x);

        wz::math::Mat4 view = wz::math::mat4_identity();
        view.m[0]  = x.x;  view.m[1]  = y.x;  view.m[2]  = z.x;  view.m[3]  = 0.f;
        view.m[4]  = x.y;  view.m[5]  = y.y;  view.m[6]  = z.y;  view.m[7]  = 0.f;
        view.m[8]  = x.z;  view.m[9]  = y.z;  view.m[10] = z.z;  view.m[11] = 0.f;
        view.m[12] = -dot(x, eye);
        view.m[13] = -dot(y, eye);
        view.m[14] = -dot(z, eye);
        view.m[15] = 1.f;

        const wz::math::Mat4 proj =
            wz::math::projection_perspective_dx(70.0f * kPi / 180.0f, aspect, 0.1f, 100.0f);

        return wz::math::mul(proj, view);
    }


    // ── Application state ─────────────────────────────────────────────────────

    struct MeshWireframeState
    {
        wz::engine::AppContext ctx{};

        wz::toolhost::ImGuiDx12ToolhostContext imgui{};
        wz::toolhost::ToolConsole              console{};
        wz::toolhost::BenchmarkRecorder        recorder{};

        wz::engine::rendering::RenderableGpuCache   renderable_cache{};
        wz::engine::rendering::DebugRenderableSetup debug_setup{};

        OrbitCamera camera{};
    };


    // ── Scene init ────────────────────────────────────────────────────────────

    bool init_scene(MeshWireframeState& state)
    {
        using namespace wz::engine::assets;
        using namespace wz::engine::rendering;

        EngineAssetLibrary& assets = *state.ctx.assets;

        MeshAsset mesh = assets.meshes().create_procedural_mesh({
            .name = "wireframe/cube",
            .kind = ProceduralMeshKind::Cube,
        });

        if (!mesh.valid())
            return false;

        RenderableAsset renderable = assets.renderables().create_mesh_wireframe({
            .name = "wireframe/cube_renderable",
            .mesh = mesh,
        });

        if (!renderable.valid())
            return false;

        ShaderPairDesc shader_desc{};
        if (!get_builtin_shader_pair_desc(
                BuiltinRenderProgram::MeshWireframeDebug, shader_desc))
            return false;

        ShaderPairAsset shaders = assets.shaders().create_shader_pair(shader_desc);

        if (!shaders.valid())
            return false;

        if (!assets.commit())
            return false;

        const auto report = assets.resolve_all();
        if (!report.ok())
            return false;

        return setup_debug_renderable_context(
            state.ctx.device,
            assets,
            state.renderable_cache,
            renderable,
            shaders,
            state.debug_setup);
    }


    // ── Per-frame wireframe render ────────────────────────────────────────────

    void render_wireframe(MeshWireframeState& state)
    {
        const int w = static_cast<int>(
            wz::gpu::dx12::internal::get_width(state.ctx.device));
        const int h = static_cast<int>(
            wz::gpu::dx12::internal::get_height(state.ctx.device));

        wz::gpu::dx12::MeshWireframeDebugView view{};

        const wz::math::Mat4 world    = wz::math::mat4_identity();
        const wz::math::Mat4 view_proj = make_view_proj(state.camera, w, h);

        for (int i = 0; i < 16; ++i) view.world[i]     = world.m[i];
        for (int i = 0; i < 16; ++i) view.view_proj[i] = view_proj.m[i];

        wz::gpu::dx12::submit_mesh_wireframe_debug_frame(state.ctx.device, view);
    }


    // ── ImGui panels ──────────────────────────────────────────────────────────

    static const wz::toolhost::BenchmarkExportConfig k_export_config = {
        .name_prefix    = "wireframe/recording",
        .frame_column   = "frame",
        .metric_columns = { "dt_ms", "fps" },
    };

    void draw_wireframe_panel(
        MeshWireframeState&             state,
        const wz::engine::FrameContext& fctx)
    {
        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 380.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Mesh Wireframe");

        ImGui::Text("Mesh wireframe debug viewer");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Frame: %llu",
                static_cast<unsigned long long>(fctx.frame.index));
            ImGui::Text("dt: %.4f ms",
                fctx.frame.delta_seconds() * 1000.0);
            ImGui::Text("FPS: %.1f",
                fctx.frame.delta_seconds() > 0.0
                    ? 1.0 / fctx.frame.delta_seconds()
                    : 0.0);
        }

        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Yaw",      &state.camera.yaw,      -3.14159f, 3.14159f);
            ImGui::SliderFloat("Pitch",    &state.camera.pitch,    -1.50f,    1.50f);
            ImGui::SliderFloat("Distance", &state.camera.distance,  1.0f,     30.0f);
            ImGui::DragFloat3("Target",    &state.camera.target_x,  0.05f);

            if (ImGui::Button("Reset Camera"))
                state.camera = OrbitCamera{};
        }

        if (ImGui::CollapsingHeader("Record / Export"))
        {
            // Accumulate row if recording
            if (state.recorder.recording())
            {
                const double dt  = fctx.frame.delta_seconds();
                const double fps = dt > 0.0 ? 1.0 / dt : 0.0;

                char buf[32];
                auto ds = [&](double v) -> std::string {
                    std::snprintf(buf, sizeof(buf), "%.6g", v);
                    return buf;
                };

                state.recorder.push_row({
                    std::to_string(state.recorder.frame_count()),
                    ds(dt * 1000.0),
                    ds(fps),
                });
            }

            const wz::toolhost::RecorderAction action =
                wz::toolhost::draw_recorder_controls(state.recorder);

            if (action == wz::toolhost::RecorderAction::ExportRequested)
            {
                wz::toolhost::export_recording(
                    state.recorder,
                    state.ctx.device,
                    state.ctx.logger,
                    k_export_config);

                state.console.push_string("[wireframe] " + state.recorder.last_status);
            }
        }

        ImGui::End();
    }

} // namespace


// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    MeshWireframeState state{};

    state.recorder.configure({ "frame", "dt_ms", "fps" });

    // ── Engine context ────────────────────────────────────────────────────────
    if (!wz::engine::init(state.ctx, {
            .window        = { .title = "Mesh Wireframe Toolhost", .width = 1280, .height = 720 },
            .resource_root = "resources",
        }))
    {
        return 1;
    }

    wz::logging::set_log_sink(
        state.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            static_cast<MeshWireframeState*>(user)->console.push(record);
        },
        &state);

    // ── Scene ─────────────────────────────────────────────────────────────────
    if (!init_scene(state))
    {
        wz::engine::shutdown(state.ctx);
        return 1;
    }

    // ── ImGui ─────────────────────────────────────────────────────────────────
    if (!state.imgui.init(state.ctx.window, state.ctx.device))
    {
        state.imgui.shutdown(state.ctx.window);
        wz::engine::shutdown(state.ctx);
        return 1;
    }

    // ── Main loop ─────────────────────────────────────────────────────────────
    wz::engine::run([&state](wz::engine::Context& ctx, wz::engine::FrameContext& fctx)
    {
        wz::window::pump_messages();

        PlatformEvent evt{};
        while (wz::window::poll_event(state.ctx.window, evt))
        {
            if (evt.type == PlatformEvent::Type::Close)
                ctx.running = false;

            if (evt.type == PlatformEvent::Type::Resize &&
                evt.resize.width > 0 && evt.resize.height > 0)
            {
                wz::gpu::resize(state.ctx.device, evt.resize.width, evt.resize.height);
            }
        }

        if (!ctx.running)
            return;

        wz::gpu::begin_frame(state.ctx.device);
        wz::gpu::clear(state.ctx.device, 0.05f, 0.05f, 0.05f, 1.0f);

        render_wireframe(state);

        state.imgui.begin_frame(state.ctx.device, fctx);

        draw_wireframe_panel(state, fctx);
        wz::toolhost::draw_console_panel(state.console);

        ImGui::Render();
        state.imgui.render(state.ctx.device);

        wz::gpu::end_frame(state.ctx.device);
        wz::gpu::present(state.ctx.device);
    });

    // ── Shutdown ──────────────────────────────────────────────────────────────
    state.imgui.shutdown(state.ctx.window);

    state.renderable_cache.clear();

    wz::logging::set_log_sink(state.ctx.logger, nullptr, nullptr);
    wz::logging::wait_until_idle(state.ctx.logger);

    wz::engine::shutdown(state.ctx);

    return 0;
}
