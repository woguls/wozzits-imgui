#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <engine/app_context.h>
#include <engine/engine.h>

#include <engine/assets/gaussian_splat_asset_module.h>
#include <engine/assets/gaussian_splat/gaussian_splat_cloud.h>
#include <engine/assets/file_carrier_asset_module.h>
#include <engine/assets/renderable_asset_module.h>
#include <engine/assets/schema_ids.h>
#include <engine/assets/type_extensions.h>
#include <engine/assets/render_program/render_program_asset_module.h>
#include <engine/rendering/renderable_gpu_cache.h>
#include <engine/rendering/renderable_pipeline_cache.h>
#include <engine/rendering/render_program_pipeline_cache.h>
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
#include <span>
#include <string>

namespace
{
    // ── Camera math ───────────────────────────────────────────────────────────

    struct OrbitCamera
    {
        float yaw      = 0.0f;
        float pitch    = 0.25f;
        float distance = 4.0f;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float target_z = 0.0f;
    };

    // Fit the camera to a bounding box: centre the target on the midpoint and
    // set the distance to the diagonal so the whole cloud is in frame.
    OrbitCamera fit_camera_to_bounds(
        const wz::engine::assets::GaussianSplatBounds& bounds)
    {
        OrbitCamera cam{};

        if (!bounds.valid)
            return cam;

        cam.target_x = (bounds.min[0] + bounds.max[0]) * 0.5f;
        cam.target_y = (bounds.min[1] + bounds.max[1]) * 0.5f;
        cam.target_z = (bounds.min[2] + bounds.max[2]) * 0.5f;

        const float dx = bounds.max[0] - bounds.min[0];
        const float dy = bounds.max[1] - bounds.min[1];
        const float dz = bounds.max[2] - bounds.min[2];
        cam.distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        return cam;
    }

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

        // Derive near/far from distance so they stay sensible at any scale.
        // near = 0.1% of distance (min 0.001); far = 4× distance (min 100).
        const float near_plane = (std::max)(0.001f, cam.distance * 0.001f);
        const float far_plane  = (std::max)(100.0f, cam.distance * 4.0f);

        const wz::math::Mat4 proj =
            wz::math::projection_perspective_dx(
                70.0f * kPi / 180.0f, aspect, near_plane, far_plane);

        return wz::math::mul(proj, view);
    }


    // ── Application state ─────────────────────────────────────────────────────

    struct GaussianSplatState
    {
        wz::engine::AppContext ctx{};

        wz::toolhost::ImGuiDx12ToolhostContext imgui{};
        wz::toolhost::ToolConsole              console{};
        wz::toolhost::BenchmarkRecorder        recorder{};

        wz::engine::rendering::RenderableGpuCache       renderable_cache{};
        wz::engine::rendering::RenderablePipelineCache  pipeline_cache{};
        wz::engine::rendering::RenderProgramPipelineCache render_program_cache{};
        wz::engine::rendering::RenderResourceResolver   resolver{};

        wz::scene::SplatHandle splat_handle{ wz::scene::INVALID_SPLAT };

        OrbitCamera camera{};
        OrbitCamera default_camera{};

        float base_splat_pixel_size = 100.0f;
    };


    // ── Scene init ────────────────────────────────────────────────────────────

    bool init_scene(GaussianSplatState& state)
    {
        using namespace wz::engine::assets;
        using namespace wz::engine::rendering;


        EngineAssetLibrary& assets = *state.ctx.assets;

        // Load a real 3DGS PLY file from disk (binary or ASCII — both supported).
        const wz::asset::AssetKey ply_file =
            assets.files().register_file_node(
                "splats/Tree.ply",
                kRawFileSchema,
                kAssetTypeRawFile);
        if (ply_file == wz::asset::AssetKey{})
            return false;

        GaussianSplatCloudAsset splat_cloud =
            assets.gaussian_splats().create_from_ply({
                .name        = "splat/sofa",
                .source_file = ply_file,
            });
        if (!splat_cloud.valid())
            return false;

        RenderableAsset renderable =
            assets.renderables().create_gaussian_splat_debug({
                .name        = "splat/debug_sphere_renderable",
                .splat_cloud = splat_cloud,
            });
        if (!renderable.valid())
            return false;

        ShaderPairDesc shader_desc{};
        if (!get_builtin_shader_pair_desc(
                BuiltinRenderProgram::GaussianSplatDebug, shader_desc))
            return false;

        ShaderPairAsset shaders = assets.shaders().create_shader_pair(shader_desc);
        if (!shaders.valid())
            return false;

        wz::engine::assets::RenderProgramAsset render_program_asset =
            assets.render_programs().create_builtin({
                .name          = "program/gaussian_splat_debug",
                .program       = BuiltinRenderProgram::GaussianSplatDebug,
                .vertex_shader = shaders.vertex_shader,
                .pixel_shader  = shaders.pixel_shader,
            });
        if (!render_program_asset.valid())
            return false;

        if (!assets.commit())
            return false;

        const auto report = assets.resolve_all();
        if (!report.ok())
            return false;

        const auto program_handle =
            assets.render_programs().get_render_program(render_program_asset);
        if (!program_handle.valid())
            return false;

        if (!state.render_program_cache.realize(
                state.ctx.device, assets.render_programs().table(), program_handle))
            return false;

        // Fit the orbit camera to the imported cloud so any PLY file is visible
        // regardless of its coordinate range. Store as the reset target too.
        {
            const auto cloud_handle = assets.gaussian_splats().get_cloud(splat_cloud);
            if (cloud_handle.valid())
            {
                const auto* cloud_data =
                    assets.gaussian_splats().get_cloud_data(cloud_handle);
                if (cloud_data && cloud_data->bounds.valid)
                {
                    state.camera         = fit_camera_to_bounds(cloud_data->bounds);
                    state.default_camera = state.camera;
                }
            }
        }

        // Realize GPU resources and pipeline.
        const auto handle = assets.renderables().get_renderable(renderable);
        auto prepared     = state.renderable_cache.realize(
            state.ctx.device, assets, handle);
        if (!prepared.valid())
            return false;

        // Legacy pipeline_cache kept as fallback; new path uses render_program_cache.
        if (!state.pipeline_cache.realize(
                state.ctx.device, assets, prepared.program, shaders))
            return false;

        prepared.render_program = program_handle;

        // Register the GPU resource with the resolver (program stored alongside).
        // The returned SplatHandle goes into DrawCommand::splats_buffer each frame.
        state.splat_handle =
            state.resolver.register_splat_cloud(
                prepared.gpu_resource, prepared.program, prepared.render_program);

        return true;
    }


    // ── Per-frame render (Track A path) ──────────────────────────────────────

    void render_splats(GaussianSplatState& state)
    {
        const int w = static_cast<int>(
            wz::gpu::dx12::internal::get_width(state.ctx.device));
        const int h = static_cast<int>(
            wz::gpu::dx12::internal::get_height(state.ctx.device));

        // Build a single DrawCommand referencing the splat cloud via the resolver.
        wz::render::DrawCommand cmd{};
        cmd.stage         = wz::render::PipelineStage::Splat;
        cmd.kind          = wz::render::DrawCommandKind::GaussianSplats;
        cmd.splats_buffer = state.splat_handle;
        cmd.world         = wz::math::mat4_identity();

        // Build a minimal RenderFrameView — no opaque or transparent spans.
        wz::render::RenderFrameView frame{};
        frame.splats               = std::span<const wz::render::DrawCommand>(&cmd, 1);
        frame.view.view_projection = make_view_proj(state.camera, w, h);

        // Submit: prefers render_program_cache when handle is valid, falls back
        // to legacy pipeline_cache for renderables without a render program.
        wz::gpu::dx12::submit_render_frame(
            state.ctx.device, frame, state.resolver,
            state.pipeline_cache, state.render_program_cache);
    }


    // ── ImGui panel ───────────────────────────────────────────────────────────

    static const wz::toolhost::BenchmarkExportConfig k_export_config = {
        .name_prefix    = "splat/recording",
        .frame_column   = "frame",
        .metric_columns = { "dt_ms", "fps" },
    };

    void draw_splat_panel(
        GaussianSplatState&             state,
        const wz::engine::FrameContext& fctx)
    {
        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 400.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Gaussian Splat Toolhost");

        ImGui::Text("splats/sofa_ascii.ply — Track A renderer via RenderResourceResolver");
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
            ImGui::SliderFloat("Yaw",   &state.camera.yaw,   -3.14159f, 3.14159f);
            ImGui::SliderFloat("Pitch", &state.camera.pitch, -1.50f,    1.50f);

            // Step at 1% of current distance so dragging feels natural at any scale.
            const float dist_step = (std::max)(0.001f, state.camera.distance * 0.01f);
            ImGui::DragFloat("Distance", &state.camera.distance, dist_step, 0.001f, 0.0f);

            const float target_step = dist_step;
            ImGui::DragFloat3("Target", &state.camera.target_x, target_step);

            if (ImGui::Button("Reset Camera"))
                state.camera = state.default_camera;
        }

        if (ImGui::CollapsingHeader("Splat Debug Draw", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat(
                "Base splat pixel size",
                &state.base_splat_pixel_size,
                1.0f,
                256.0f,
                "%.1f px");

            if (ImGui::Button("Reset Splat Size"))
                state.base_splat_pixel_size = 32.0f;
        }

        if (ImGui::CollapsingHeader("Record / Export"))
        {
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

                state.console.push_string("[splat] " + state.recorder.last_status);
            }
        }

        ImGui::End();
    }

} // namespace


// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    GaussianSplatState state{};

    state.recorder.configure({ "frame", "dt_ms", "fps" });

    // ── Engine context ────────────────────────────────────────────────────────
    if (!wz::engine::init(state.ctx, {
            .window        = { .title = "Gaussian Splat Toolhost", .width = 1280, .height = 720 },
            .resource_root = "resources",
        }))
    {
        return 1;
    }

    wz::logging::set_log_sink(
        state.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            static_cast<GaussianSplatState*>(user)->console.push(record);
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
        wz::gpu::clear(state.ctx.device, 0.02f, 0.02f, 0.05f, 1.0f);

        render_splats(state);

        state.imgui.begin_frame(state.ctx.device, fctx);

        draw_splat_panel(state, fctx);
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
