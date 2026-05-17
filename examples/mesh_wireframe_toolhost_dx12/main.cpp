#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <engine/app_context.h>
#include <engine/engine.h>

#include <engine/assets/mesh/mesh.h>
#include <engine/assets/mesh_asset_module.h>
#include <engine/assets/renderable_asset_module.h>
#include <engine/assets/schema_ids.h>
#include <engine/assets/type_extensions.h>
#include <engine/rendering/renderable_gpu_cache.h>
#include <engine/rendering/renderable_pipeline_cache.h>
#include <engine/rendering/builtin_render_programs.h>
#include <engine/rendering/render_resource_resolver.h>

#include <event/platform_event.h>
#include <gpu/gpu.h>
#include <gpu/dx12/dx12.h>
#include <gpu/dx12/dx12_internal.h>

#include <render/frame/render_frame.h>
#include <scene/compile/compiled_scene.h>

#include <math/mat4.h>
#include <math/vec3.h>
#include <math/camera.h>
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

        const wz::math::Mat4 view =
            wz::math::look_at_dx(eye, target, { 0.f, 1.f, 0.f });

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

        wz::engine::rendering::RenderableGpuCache     renderable_cache{};
        wz::engine::rendering::RenderablePipelineCache pipeline_cache{};
        wz::engine::rendering::RenderResourceResolver  resolver{};

        wz::scene::MeshHandle mesh_handle{ wz::scene::INVALID_MESH };

        OrbitCamera camera{};
    };


    // ── Scene init ────────────────────────────────────────────────────────────

    bool init_scene(MeshWireframeState& state)
    {
        using namespace wz::engine::assets;
        using namespace wz::engine::rendering;

        EngineAssetLibrary& assets = *state.ctx.assets;

        const wz::asset::AssetKey rock_file =
            assets.files().register_file_node(
                "gltf/low_poly_rock.glb",
                kRawFileSchema,
                kAssetTypeRawFile);

        if (rock_file == wz::asset::AssetKey{})
            return false;

        MeshAsset mesh = assets.meshes().create_glb_mesh({
            .name = "wireframe/low_poly_rock",
            .source_file = rock_file,
            .mesh_index = 0,
            });

        if (!mesh.valid())
            return false;

        RenderableAsset renderable = assets.renderables().create_mesh_wireframe({
            .name = "wireframe/low_poly_rock_renderable",
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

        // Realize GPU resources and pipeline.
        const auto handle = assets.renderables().get_renderable(renderable);
        const auto prepared = state.renderable_cache.realize(
            state.ctx.device, assets, handle);

        if (!prepared.valid())
            return false;

        if (!state.pipeline_cache.realize(
            state.ctx.device, assets, prepared.program, shaders))
            return false;

        state.mesh_handle =
            state.resolver.register_mesh(prepared.gpu_resource, prepared.program);

        return true;
    }


    // ── Per-frame wireframe render ────────────────────────────────────────────

    void render_wireframe(MeshWireframeState& state)
    {
        const int w = static_cast<int>(
            wz::gpu::dx12::internal::get_width(state.ctx.device));
        const int h = static_cast<int>(
            wz::gpu::dx12::internal::get_height(state.ctx.device));

        wz::render::DrawCommand cmd{};
        cmd.stage = wz::render::PipelineStage::OpaqueGeometry;
        cmd.kind  = wz::render::DrawCommandKind::Mesh;
        cmd.mesh  = state.mesh_handle;
        cmd.world = wz::math::mat4_identity();

        wz::render::RenderFrameView frame{};
        frame.opaque               = std::span<const wz::render::DrawCommand>(&cmd, 1);
        frame.view.view_projection = make_view_proj(state.camera, w, h);

        wz::gpu::dx12::submit_render_frame(
            state.ctx.device, frame, state.resolver, state.pipeline_cache);
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
