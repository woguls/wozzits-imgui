#include <Windows.h>

#include <engine/app_context.h>
#include <engine/engine.h>

#include <engine/assets/engine_asset_library.h>
#include <engine/assets/scalar_field/scalar_field.h>
#include <engine/assets/scalar_field_asset_module.h>
#include <engine/assets/gaussian_splat_asset_module.h>
#include <engine/assets/gaussian_splat/gaussian_splat_cloud.h>
#include <engine/assets/renderable_asset_module.h>
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

#include <imgui.h>

#include <cassert>
#include <cmath>
#include <memory>
#include <span>
#include <string>

namespace
{
    // ── Camera ────────────────────────────────────────────────────────────────

    struct OrbitCamera
    {
        float yaw      = 0.5f;
        float pitch    = 0.4f;
        float roll     = 0.0f;
        float distance = 10.0f;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float target_z = 0.0f;
    };

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
        cam.distance = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.9f;
        cam.pitch = 0.4f;
        cam.yaw   = 0.5f;
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

        const float fx = target.x - eye.x;
        const float fy = target.y - eye.y;
        const float fz = target.z - eye.z;
        const float f_inv_len = 1.f / std::sqrt(fx*fx + fy*fy + fz*fz);
        const float fnx = fx*f_inv_len, fny = fy*f_inv_len, fnz = fz*f_inv_len;

        const float nux = -fnx*fny, nuy = 1.f - fny*fny, nuz = -fnz*fny;
        const float nu_inv_len = 1.f / std::sqrt(nux*nux + nuy*nuy + nuz*nuz);
        const float nx = nux*nu_inv_len, ny = nuy*nu_inv_len, nz = nuz*nu_inv_len;

        const float rx = ny*fnz - nz*fny;
        const float ry = nz*fnx - nx*fnz;
        const float rz = nx*fny - ny*fnx;

        const float cr = std::cos(cam.roll);
        const float sr = std::sin(cam.roll);
        const wz::math::Vec3 up{ nx*cr + rx*sr, ny*cr + ry*sr, nz*cr + rz*sr };

        const wz::math::Mat4 view = wz::math::look_at_dx(eye, target, up);

        const float near_plane = (std::max)(0.001f, cam.distance * 0.001f);
        const float far_plane  = (std::max)(100.0f, cam.distance * 4.0f);

        const wz::math::Mat4 proj =
            wz::math::projection_perspective_dx(
                70.0f * kPi / 180.0f, aspect, near_plane, far_plane);

        return wz::math::mul(proj, view);
    }


    // ── Generator descriptors ─────────────────────────────────────────────────

    using SF = wz::engine::assets::ScalarFieldGenerator;

    struct GeneratorEntry
    {
        const char* label;
        SF          generator;
        bool        has_frequency;
    };

    constexpr GeneratorEntry kGenerators[] = {
        { "Gradient X",      SF::GradientX,      false },
        { "Gradient Y",      SF::GradientY,      false },
        { "Radial Gradient", SF::RadialGradient, false },
        { "Checkerboard",    SF::Checkerboard,   true  },
        { "Sine Waves",      SF::SineWaves,      true  },
    };
    constexpr int kGeneratorCount = static_cast<int>(std::size(kGenerators));


    // ── Application state ─────────────────────────────────────────────────────

    struct ScalarFieldSplatState
    {
        wz::engine::AppContext ctx{};

        wz::toolhost::ImGuiDx12ToolhostContext imgui{};
        wz::toolhost::ToolConsole              console{};

        // GPU caches — cleared and rebuilt on each regeneration.
        wz::engine::rendering::RenderableGpuCache        renderable_cache{};
        wz::engine::rendering::RenderablePipelineCache   pipeline_cache{};
        wz::engine::rendering::RenderProgramPipelineCache render_program_cache{};

        // Resolver accumulates entries across regenerations; old entries are
        // orphaned but harmless because only state.splat_handle is rendered.
        wz::engine::rendering::RenderResourceResolver resolver{};

        wz::scene::SplatHandle splat_handle{ wz::scene::INVALID_SPLAT };

        OrbitCamera camera{};
        OrbitCamera default_camera{};

        // ── Editable parameters ───────────────────────────────────────────────

        int   generator_index  = 2;      // Radial Gradient
        int   field_width      = 64;
        int   field_height     = 64;
        float frequency        = 4.0f;
        float amplitude        = 1.0f;

        float height_scale     = 4.0f;
        float step_x           = 0.1f;
        float step_z           = 0.1f;
        float splat_scale      = 0.05f;
        float opacity          = 0.9f;
        bool  normalize_values = true;
        bool  use_threshold    = false;
        float emit_threshold   = 0.0f;

        // ── Display info ──────────────────────────────────────────────────────

        uint64_t last_splat_count = 0;
        bool     scene_ready      = false;
        bool     first_build      = true;
    };


    // ── Scene build ───────────────────────────────────────────────────────────
    //
    // Registers shaders + scalar field + splat cloud + renderable in a single
    // asset session, commits once, resolves, and realizes GPU resources.
    //
    // On regenerate the caller replaces ctx.assets with a fresh library and
    // clears the GPU caches before calling this again, so each call starts
    // from a clean registration state.

    bool build_scene(ScalarFieldSplatState& state)
    {
        using namespace wz::engine::assets;
        using namespace wz::engine::rendering;

        EngineAssetLibrary& assets = *state.ctx.assets;

        // ── Shaders + render program ──────────────────────────────────────────

        ShaderPairDesc shader_desc{};
        if (!get_builtin_shader_pair_desc(
                BuiltinRenderProgram::GaussianSplatDebug, shader_desc))
            return false;

        const ShaderPairAsset shaders =
            assets.shaders().create_shader_pair(shader_desc);
        if (!shaders.valid())
            return false;

        const RenderProgramAsset render_program_asset =
            assets.render_programs().create_builtin({
                .name          = "program/gaussian_splat_debug",
                .program       = BuiltinRenderProgram::GaussianSplatDebug,
                .vertex_shader = shaders.vertex_shader,
                .pixel_shader  = shaders.pixel_shader,
            });
        if (!render_program_asset.valid())
            return false;

        // ── Scalar field ──────────────────────────────────────────────────────

        const GeneratorEntry& gen = kGenerators[state.generator_index];

        const ScalarFieldAsset scalar_field =
            assets.scalar_fields().create_procedural_scalar_field({
                .name      = "field",
                .width     = static_cast<uint32_t>(state.field_width),
                .height    = static_cast<uint32_t>(state.field_height),
                .generator = gen.generator,
                .frequency = state.frequency,
                .amplitude = state.amplitude,
            });
        if (!scalar_field.valid())
            return false;

        // ── Gaussian splat cloud from scalar field ────────────────────────────

        const GaussianSplatCloudAsset splat_cloud =
            assets.gaussian_splats().create_from_scalar_field({
                .name             = "cloud",
                .scalar_field_key = scalar_field.output,
                .height_scale     = state.height_scale,
                .step_x           = state.step_x,
                .step_z           = state.step_z,
                .splat_scale      = state.splat_scale,
                .opacity          = state.opacity,
                .normalize_values = state.normalize_values,
                .use_threshold    = state.use_threshold,
                .emit_threshold   = state.emit_threshold,
            });
        if (!splat_cloud.valid())
            return false;

        // ── Renderable ────────────────────────────────────────────────────────

        const RenderableAsset renderable =
            assets.renderables().create_gaussian_splat_debug({
                .name        = "renderable",
                .splat_cloud = splat_cloud,
            });
        if (!renderable.valid())
            return false;

        // ── Commit + resolve all in one shot ──────────────────────────────────

        if (!assets.commit())
            return false;

        const auto report = assets.resolve_all();
        if (!report.ok())
        {
            state.ctx.logger.error("asset resolve failed — check threshold params");
            return false;
        }

        // ── Render program handle + PSO ───────────────────────────────────────

        const auto render_program_handle =
            assets.render_programs().get_render_program(render_program_asset);
        if (!render_program_handle.valid())
            return false;

        if (!state.render_program_cache.realize(
                state.ctx.device,
                assets.render_programs().table(),
                render_program_handle))
            return false;

        // ── Camera fit on first build ─────────────────────────────────────────

        const auto cloud_handle = assets.gaussian_splats().get_cloud(splat_cloud);
        if (cloud_handle.valid())
        {
            const auto* cloud_data =
                assets.gaussian_splats().get_cloud_data(cloud_handle);

            if (cloud_data)
            {
                state.last_splat_count = cloud_data->splat_count();

                if (state.first_build && cloud_data->bounds.valid)
                {
                    state.camera         = fit_camera_to_bounds(cloud_data->bounds);
                    state.default_camera = state.camera;
                    state.first_build    = false;
                }
            }
        }

        // ── Realize GPU resources ─────────────────────────────────────────────

        const auto rend_handle = assets.renderables().get_renderable(renderable);
        auto prepared = state.renderable_cache.realize(
            state.ctx.device, assets, rend_handle);
        if (!prepared.valid())
            return false;

        if (!state.pipeline_cache.realize(
                state.ctx.device, assets, prepared.program, shaders))
            return false;

        prepared.render_program = render_program_handle;

        state.splat_handle =
            state.resolver.register_splat_cloud(
                prepared.gpu_resource,
                prepared.program,
                prepared.render_program);

        state.scene_ready = true;
        return true;
    }


    // ── Regenerate ────────────────────────────────────────────────────────────
    //
    // Replaces the asset library with a fresh instance (so we start from an
    // open registration state), clears GPU caches, then rebuilds the scene.

    bool regenerate_scene(ScalarFieldSplatState& state)
    {
        state.renderable_cache.clear();
        state.pipeline_cache.clear();
        state.render_program_cache.clear();

        state.ctx.assets = std::make_unique<wz::engine::assets::EngineAssetLibrary>(
            state.ctx.device, state.ctx.logger, "resources");

        return build_scene(state);
    }


    // ── Per-frame render ──────────────────────────────────────────────────────

    void render_splats(ScalarFieldSplatState& state)
    {
        if (!state.scene_ready)
            return;

        const int w = static_cast<int>(
            wz::gpu::dx12::internal::get_width(state.ctx.device));
        const int h = static_cast<int>(
            wz::gpu::dx12::internal::get_height(state.ctx.device));

        wz::render::DrawCommand cmd{};
        cmd.stage         = wz::render::PipelineStage::Splat;
        cmd.kind          = wz::render::DrawCommandKind::GaussianSplats;
        cmd.splats_buffer = state.splat_handle;
        cmd.world         = wz::math::mat4_identity();

        wz::render::RenderFrameView frame{};
        frame.splats               = std::span<const wz::render::DrawCommand>(&cmd, 1);
        frame.view.view_projection = make_view_proj(state.camera, w, h);

        wz::gpu::dx12::submit_render_frame(
            state.ctx.device, frame, state.resolver,
            state.pipeline_cache, state.render_program_cache);
    }


    // ── ImGui panel ───────────────────────────────────────────────────────────

    void draw_panel(
        ScalarFieldSplatState&          state,
        const wz::engine::FrameContext& fctx)
    {
        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("Scalar Field Splat Toolhost");

        ImGui::Text("Procedural scalar field \xe2\x86\x92 Gaussian splat cloud");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Frame"))
        {
            ImGui::Text("Frame: %llu",
                static_cast<unsigned long long>(fctx.frame.index));
            const double dt  = fctx.frame.delta_seconds();
            const double fps = dt > 0.0 ? 1.0 / dt : 0.0;
            ImGui::Text("dt: %.2f ms  FPS: %.1f", dt * 1000.0, fps);
        }

        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Yaw",   &state.camera.yaw,   -3.14159f, 3.14159f);
            ImGui::SliderFloat("Pitch", &state.camera.pitch, -1.50f,    1.50f);
            ImGui::SliderFloat("Roll",  &state.camera.roll,  -3.14159f, 3.14159f);

            const float dist_step = (std::max)(0.001f, state.camera.distance * 0.01f);
            ImGui::DragFloat("Distance", &state.camera.distance, dist_step, 0.001f, 0.0f);
            ImGui::DragFloat3("Target",  &state.camera.target_x, dist_step);

            if (ImGui::Button("Reset Camera"))
                state.camera = state.default_camera;
        }

        if (ImGui::CollapsingHeader("Scalar Field", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* current_label = kGenerators[state.generator_index].label;
            if (ImGui::BeginCombo("Generator", current_label))
            {
                for (int i = 0; i < kGeneratorCount; ++i)
                {
                    const bool selected = (i == state.generator_index);
                    if (ImGui::Selectable(kGenerators[i].label, selected))
                        state.generator_index = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::SliderInt("Width",  &state.field_width,  4, 256);
            ImGui::SliderInt("Height", &state.field_height, 4, 256);

            if (kGenerators[state.generator_index].has_frequency)
                ImGui::SliderFloat("Frequency", &state.frequency, 0.5f, 16.0f);
            ImGui::SliderFloat("Amplitude", &state.amplitude, 0.1f, 4.0f);
        }

        if (ImGui::CollapsingHeader("Splat Generation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Height scale", &state.height_scale, 0.1f, 20.0f);
            ImGui::SliderFloat("Step X",       &state.step_x,       0.01f, 2.0f);
            ImGui::SliderFloat("Step Z",       &state.step_z,       0.01f, 2.0f);
            ImGui::SliderFloat("Splat scale",  &state.splat_scale,  0.01f, 1.0f);
            ImGui::SliderFloat("Opacity",      &state.opacity,      0.01f, 0.9999f);
            ImGui::Checkbox("Normalize values", &state.normalize_values);
            ImGui::Checkbox("Use threshold",    &state.use_threshold);
            if (state.use_threshold)
                ImGui::SliderFloat("Emit threshold", &state.emit_threshold, 0.0f, 1.0f);
        }

        ImGui::Separator();

        if (ImGui::Button("Regenerate", ImVec2(-1.0f, 0.0f)))
        {
            if (!regenerate_scene(state))
                state.console.push_string("[error] regenerate failed — see log");
        }

        if (state.scene_ready)
        {
            ImGui::Text("Splats: %llu",
                static_cast<unsigned long long>(state.last_splat_count));
            const float world_w =
                static_cast<float>(state.field_width  - 1) * state.step_x;
            const float world_d =
                static_cast<float>(state.field_height - 1) * state.step_z;
            ImGui::Text("Grid: %.2f x %.2f world units", world_w, world_d);
        }
        else
        {
            ImGui::TextDisabled("(not yet generated)");
        }

        ImGui::End();
    }

} // namespace


// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    ScalarFieldSplatState state{};

    if (!wz::engine::init(state.ctx, {
            .window        = { .title = "Scalar Field Splat Toolhost",
                               .width = 1280, .height = 720 },
            .resource_root = "resources",
        }))
    {
        return 1;
    }

    wz::logging::set_log_sink(
        state.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            static_cast<ScalarFieldSplatState*>(user)->console.push(record);
        },
        &state);

    if (!build_scene(state))
    {
        wz::engine::shutdown(state.ctx);
        return 1;
    }

    if (!state.imgui.init(state.ctx.window, state.ctx.device))
    {
        state.imgui.shutdown(state.ctx.window);
        wz::engine::shutdown(state.ctx);
        return 1;
    }

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

        draw_panel(state, fctx);
        wz::toolhost::draw_console_panel(state.console);

        ImGui::Render();
        state.imgui.render(state.ctx.device);

        wz::gpu::end_frame(state.ctx.device);
        wz::gpu::present(state.ctx.device);
    });

    state.imgui.shutdown(state.ctx.window);
    state.renderable_cache.clear();

    wz::logging::set_log_sink(state.ctx.logger, nullptr, nullptr);
    wz::logging::wait_until_idle(state.ctx.logger);

    wz::engine::shutdown(state.ctx);

    return 0;
}
