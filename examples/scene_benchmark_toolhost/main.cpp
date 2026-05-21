// examples/scene_benchmark_toolhost/main.cpp
//
// Scene-owned splat benchmark: multiple instances of Tree.ply placed in
// a grid via the scene builder.  Each PLY instance is one scene node;
// the renderer handles culling, compilation, and draw-command generation.
// BenchmarkApp owns the window, device, scene graph, and fly camera.
// Press ESC to toggle navigation; BKSPC to quit.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <engine/benchmark_app.h>
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

#include <gpu/gpu.h>
#include <gpu/dx12/dx12.h>

#include <math/mat4.h>

#include <render/frame/render_frame.h>

#include <scene/compile/legacy_classification.h>

#include <time/w_time.h>
#include <logging/logging.h>

#include <wozzits/toolhost/tool_console.h>
#include <wozzits/toolhost/imgui_dx12_context.h>
#include <wozzits/toolhost/benchmark_recorder.h>

#include <imgui.h>

namespace
{
    // ─── Timing helper ────────────────────────────────────────────────────────

    double ticks_to_ms(uint64_t ticks)
    {
        const double tps =
            static_cast<double>(wz::time::TimeSource::ticks_per_second());
        return static_cast<double>(ticks) * 1000.0 / tps;
    }

    const char* prep_path_name(wz::bench::RenderPrepPath p)
    {
        switch (p)
        {
        case wz::bench::RenderPrepPath::FullCompile:      return "FullCompile";
        case wz::bench::RenderPrepPath::ViewOnly:         return "ViewOnly";
        case wz::bench::RenderPrepPath::TransformOnly:    return "TransformOnly";
        case wz::bench::RenderPrepPath::TransformAndView: return "TransformAndView";
        default:                                          return "Unknown";
        }
    }

    double job_ms(
        const std::vector<wz::jobs::JobTimingRecord>& timings,
        const char* wanted_name)
    {
        uint64_t ticks = 0;

        for (const auto& rec : timings)
        {
            const char* name = rec.name ? rec.name : "";
            if (std::strcmp(name, wanted_name) == 0)
                ticks += rec.duration_ticks();
        }

        return ticks_to_ms(ticks);
    }

    // ─── Benchmark recorder config ────────────────────────────────────────────

    static const wz::toolhost::BenchmarkExportConfig k_export_config = {
        .name_prefix  = "bench/scene_benchmark_recording",
        .frame_column = "frame",
        .metric_columns = {
            "dt_ms",
            "fps",
            "frame_cpu_ms",
            "bench_update_ms",
            "begin_clear_ms",
            "submit_opaque_ms",
            "submit_splat_ms",
            "imgui_ms",
            "end_present_ms",

            "total_job_ms",
            "render_prep_job_ms",
            "build_view_ms",
            "compile_scene_ms",
            "build_render_ir_ms",
            "build_render_frame_ms",
            "slowest_job_ms",
            "opaque_commands",
            "splat_commands",
            "total_commands",
            "visible_opaque",
            "culled_opaque",
            "visible_splats",
            "culled_splats",
            "scene_nodes",
            "dirty_nodes",
        },
    };


    // ─── Per-frame phase timings ─────────────────────────────────────────────

    struct FramePhaseTimings
    {
        double frame_cpu_ms = 0.0;

        double bench_update_ms = 0.0;
        double begin_clear_ms = 0.0;
        double submit_opaque_ms = 0.0;
        double submit_splat_ms = 0.0;
        double imgui_ms = 0.0;
        double end_present_ms = 0.0;
    };

    // ─── Application state ────────────────────────────────────────────────────

    struct BenchSceneToolhostState
    {
        wz::bench::BenchmarkApp app{};

        wz::toolhost::ImGuiDx12ToolhostContext  imgui{};
        wz::toolhost::ToolConsole               console{};
        wz::toolhost::BenchmarkRecorder         recorder{};

        wz::engine::rendering::RenderableGpuCache         renderable_cache{};
        wz::engine::rendering::RenderablePipelineCache    pipeline_cache{};
        wz::engine::rendering::RenderProgramPipelineCache render_program_cache{};
        wz::engine::rendering::RenderResourceResolver     resolver{};

        // Asset handles populated during scene_def.pre_commit (before commit());
        // used in build_splat_scene() (the scene_def.build callback).
        wz::engine::assets::GaussianSplatCloudAsset splat_cloud{};
        wz::engine::assets::RenderableAsset         splat_renderable{};
        wz::engine::assets::ShaderPairAsset         splat_shaders{};
        wz::engine::assets::RenderProgramAsset      splat_render_program{};

        wz::scene::SplatHandle splat_handle{ wz::scene::INVALID_SPLAT };

        FramePhaseTimings timings{};
    };

    void record_benchmark_row(
        BenchSceneToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        if (!state.recorder.recording())
            return;

        const auto& profile = state.app.jobs.profile;
        const auto& culling = state.app.frame.render_ir.ir.culling;
        const auto& rf = state.app.frame.render_frame.frame;

        uint64_t total_ticks = 0;
        uint64_t prep_ticks = 0;
        const wz::jobs::JobTimingRecord* slowest = nullptr;

        for (const auto& rec : profile.timings)
        {
            const uint64_t dt = rec.duration_ticks();
            total_ticks += dt;

            const char* name = rec.name ? rec.name : "";
            if (std::strcmp(name, "build_view") == 0
                || std::strcmp(name, "compile_scene") == 0
                || std::strcmp(name, "build_render_ir") == 0
                || std::strcmp(name, "build_render_frame") == 0)
            {
                prep_ticks += dt;
            }

            if (!slowest || dt > slowest->duration_ticks())
                slowest = &rec;
        }

        const uint64_t opaque_cmds = rf.opaque.size();
        const uint64_t splat_cmds = rf.splats.size();
        const uint64_t total_cmds =
            opaque_cmds + splat_cmds + rf.transparent.size() + rf.particles.size();

        const double build_view_ms =
            job_ms(profile.timings, "build_view");
        const double compile_scene_ms =
            job_ms(profile.timings, "compile_scene");
        const double build_render_ir_ms =
            job_ms(profile.timings, "build_render_ir");
        const double build_render_frame_ms =
            job_ms(profile.timings, "build_render_frame");

        const double dt = fctx.frame.delta_seconds();
        const double fps = dt > 0.0 ? 1.0 / dt : 0.0;

        char buf[32];
        auto ds = [&](double v) -> std::string {
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            return buf;
            };

        state.recorder.push_row({
            std::to_string(static_cast<uint32_t>(state.recorder.frame_count())),
            ds(dt * 1000.0),
            ds(fps),

            ds(state.timings.frame_cpu_ms),
            ds(state.timings.bench_update_ms),
            ds(state.timings.begin_clear_ms),
            ds(state.timings.submit_opaque_ms),
            ds(state.timings.submit_splat_ms),
            ds(state.timings.imgui_ms),
            ds(state.timings.end_present_ms),

            ds(ticks_to_ms(total_ticks)),
            ds(ticks_to_ms(prep_ticks)),
            ds(build_view_ms),
            ds(compile_scene_ms),
            ds(build_render_ir_ms),
            ds(build_render_frame_ms),
            ds(slowest ? ticks_to_ms(slowest->duration_ticks()) : 0.0),

            std::to_string(opaque_cmds),
            std::to_string(splat_cmds),
            std::to_string(total_cmds),
            std::to_string(culling.visible_opaque),
            std::to_string(culling.culled_opaque),
            std::to_string(culling.visible_splats),
            std::to_string(culling.culled_splats),
            std::to_string(wz::core::graph::node_count(
                state.app.scene_runtime.scene.polytree)),
            std::to_string(state.app.scene_runtime.dirty_nodes.size()),
            });
    }

    // ─── Grid parameters ─────────────────────────────────────────────────────

    constexpr int   k_grid_x       = 10;     // instances along X
    constexpr int   k_grid_z       = 10;     // instances along Z
    constexpr float k_grid_spacing = 5.0f; // distance between instance centres

    // ─── Scene builder: PLY cloud instances on a grid ─────────────────────
    //
    // Realizes GPU resources for the splat cloud, then places k_grid_x × k_grid_z
    // instances of Tree.ply into the scene graph.  Each instance is one scene node
    // with a translation transform; the renderer handles culling, compilation,
    // and draw-command generation.

    bool build_splat_scene(
        wz::bench::SceneRuntime& runtime,
        wz::engine::AppContext&  ctx,
        BenchSceneToolhostState& state)
    {
        using namespace wz::scene;
        using namespace wz::core::graph;
        using namespace wz::math;
        using namespace wz::engine::assets;
        using namespace wz::engine::rendering;

        // ── GPU realization (assets are committed + resolved by now) ─────

        EngineAssetLibrary& assets = *ctx.assets;

        const auto program_handle =
            assets.render_programs().get_render_program(state.splat_render_program);
        if (!program_handle.valid())
            return false;

        if (!state.render_program_cache.realize(
                ctx.device, assets.render_programs().table(), program_handle))
            return false;

        const auto rend_handle =
            assets.renderables().get_renderable(state.splat_renderable);
        auto prepared =
            state.renderable_cache.realize(ctx.device, assets, rend_handle);
        if (!prepared.valid())
            return false;

        prepared.render_program = program_handle;

        state.splat_handle = state.resolver.register_splat_cloud(
            prepared.gpu_resource, prepared.program, prepared.render_program);

        // ── Read cloud data (for camera fitting) ────────────────────────

        const auto cloud_asset_handle =
            assets.gaussian_splats().get_cloud(state.splat_cloud);
        if (!cloud_asset_handle.valid())
            return false;

        const auto* cloud_data =
            assets.gaussian_splats().get_cloud_data(cloud_asset_handle);
        if (!cloud_data || !cloud_data->valid())
            return false;

        // ── Build scene graph: root + grid of cloud instance nodes ──────

        const int total_instances = k_grid_x * k_grid_z;

        // Centre the grid around the origin.
        const float origin_x = -0.5f * (k_grid_x - 1) * k_grid_spacing;
        const float origin_z = -0.5f * (k_grid_z - 1) * k_grid_spacing;

        SceneBuilder b;

        TransformNode root{};
        root.local       = Mat4::identity();
        root.flags       = TransformNodeFlag::None;
        root.motion_type = TransformNode::MotionType::Static;
        const NodeHandle root_h = add_node(b, root);

        std::vector<NodeHandle> instance_handles;
        instance_handles.reserve(total_instances);

        for (int iz = 0; iz < k_grid_z; ++iz)
        {
            for (int ix = 0; ix < k_grid_x; ++ix)
            {
                TransformNode node{};
                node.local       = Mat4::identity();
                node.local.m[12] = origin_x + ix * k_grid_spacing;
                node.local.m[14] = origin_z + iz * k_grid_spacing;
                node.motion_type = TransformNode::MotionType::Static;

                const NodeHandle h = add_node(b, node);
                add_edge(b, root_h, h);
                instance_handles.push_back(h);
            }
        }

        auto result = build(std::move(b));
        if (!result.has_value())
            return false;

        runtime.scene = std::move(*result);

        // ── Descriptors ─────────────────────────────────────────────────

        const uint32_t node_cnt = node_count(runtime.scene.polytree);
        runtime.descriptors.resize(node_cnt);

        // Root: non-renderable
        runtime.descriptors[root_h] = RenderableDescriptor{
            .node_class = classify_legacy_renderable(RenderPipeline::None),
        };

        // Each instance: one cloud renderable
        for (const NodeHandle h : instance_handles)
        {
            runtime.descriptors[h] = RenderableDescriptor{
                .node_class = classify_legacy_renderable(RenderPipeline::Splat),
                .splat_data = SplatDescriptor{
                    .cloud_handle      = state.splat_handle,
                    .cloud_local_index = 0,
                },
                .visible = true,
            };
        }

        propagate_all(runtime.scene.polytree);

        // ── Fit camera to grid centre, pulled back to see the whole grid ─

        if (cloud_data->bounds.valid)
        {
            const auto& bnd = cloud_data->bounds;
            const float cloud_dx = bnd.max[0] - bnd.min[0];
            const float cloud_dy = bnd.max[1] - bnd.min[1];
            const float cloud_dz = bnd.max[2] - bnd.min[2];
            const float cloud_diag =
                std::sqrt(cloud_dx * cloud_dx + cloud_dy * cloud_dy + cloud_dz * cloud_dz);

            const float grid_extent_x = (k_grid_x - 1) * k_grid_spacing;
            const float grid_extent_z = (k_grid_z - 1) * k_grid_spacing;
            const float grid_diag =
                std::sqrt(grid_extent_x * grid_extent_x + grid_extent_z * grid_extent_z);

            state.app.camera.x = 0.0f;
            state.app.camera.y = (bnd.min[1] + bnd.max[1]) * 0.5f;
            state.app.camera.z = -(grid_diag * 0.5f + cloud_diag);
        }

        return true;
    }
    // ─── Splat asset registration (pre_commit hook) ───────────────────────────
    //
    // Called inside wz::bench::init() while the asset registration phase is
    // still open.  Stores asset handles in state for use after commit().

    bool register_splat_assets(
        wz::engine::assets::EngineAssetLibrary& assets,
        BenchSceneToolhostState&                state)
    {
        using namespace wz::engine::assets;
        using namespace wz::engine::rendering;

        const wz::asset::AssetKey ply_file =
            assets.files().register_file_node(
                "splats/Tree.ply",
                kRawFileSchema,
                kAssetTypeRawFile);
        if (ply_file == wz::asset::AssetKey{})
            return false;

        state.splat_cloud = assets.gaussian_splats().create_from_ply({
            .name        = "splat/tree",
            .source_file = ply_file,
        });
        if (!state.splat_cloud.valid())
            return false;

        state.splat_renderable = assets.renderables().create_gaussian_splat_debug({
            .name        = "splat/tree_renderable",
            .splat_cloud = state.splat_cloud,
        });
        if (!state.splat_renderable.valid())
            return false;

        ShaderPairDesc shader_desc{};
        if (!get_builtin_shader_pair_desc(
                BuiltinRenderProgram::GaussianSplatPullDebug, shader_desc))
            return false;

        state.splat_shaders = assets.shaders().create_shader_pair(shader_desc);
        if (!state.splat_shaders.valid())
            return false;

        state.splat_render_program = assets.render_programs().create_builtin({
            .name          = "program/gaussian_splat_pull_debug",
            .program       = BuiltinRenderProgram::GaussianSplatPullDebug,
            .vertex_shader = state.splat_shaders.vertex_shader,
            .pixel_shader  = state.splat_shaders.pixel_shader,
        });
        if (!state.splat_render_program.valid())
            return false;

        return true;
    }

    // ─── Benchmark panel ──────────────────────────────────────────────────────

    void draw_benchmark_panel(
        BenchSceneToolhostState&        state,
        const wz::engine::FrameContext& fctx)
    {
        const auto& profile = state.app.jobs.profile;
        const auto& culling = state.app.frame.render_ir.ir.culling;
        const auto& rf      = state.app.frame.render_frame.frame;

        uint64_t total_ticks = 0;
        uint64_t prep_ticks  = 0;
        const wz::jobs::JobTimingRecord* slowest = nullptr;

        for (const auto& rec : profile.timings)
        {
            const uint64_t dt = rec.duration_ticks();
            total_ticks += dt;

            const char* name = rec.name ? rec.name : "";
            if (   std::strcmp(name, "build_view")         == 0
                || std::strcmp(name, "compile_scene")      == 0
                || std::strcmp(name, "build_render_ir")    == 0
                || std::strcmp(name, "build_render_frame") == 0)
            {
                prep_ticks += dt;
            }

            if (!slowest || dt > slowest->duration_ticks())
                slowest = &rec;
        }

        const uint64_t opaque_cmds = rf.opaque.size();
        const uint64_t splat_cmds  = rf.splats.size();
        const uint64_t total_cmds  =
            opaque_cmds + splat_cmds + rf.transparent.size() + rf.particles.size();
        const double build_view_ms =
            job_ms(profile.timings, "build_view");
        const double compile_scene_ms =
            job_ms(profile.timings, "compile_scene");
        const double build_render_ir_ms =
            job_ms(profile.timings, "build_render_ir");
        const double build_render_frame_ms =
            job_ms(profile.timings, "build_render_frame");

        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(520.0f, 500.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Scene Benchmark");

        ImGui::Text("Scene-owned gaussian splat cloud (compile -> IR -> frame -> submit)");
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
            ImGui::Text("Frame CPU: %.4f ms", state.timings.frame_cpu_ms);
            ImGui::Text("Benchmark update: %.4f ms", state.timings.bench_update_ms);
            ImGui::Text("Begin + clear: %.4f ms", state.timings.begin_clear_ms);
            ImGui::Text("Submit opaque: %.4f ms", state.timings.submit_opaque_ms);
            ImGui::Text("Submit splat: %.4f ms", state.timings.submit_splat_ms);
            ImGui::Text("ImGui: %.4f ms", state.timings.imgui_ms);
            ImGui::Text("End + present: %.4f ms", state.timings.end_present_ms);
        }

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Compiled scene valid: %s",
                state.app.scene_runtime.compiled_scene_valid ? "yes" : "no");
            ImGui::Text("Scene nodes: %u",
                wz::core::graph::node_count(
                    state.app.scene_runtime.scene.polytree));
            ImGui::Text("Dirty transform nodes: %zu",
                state.app.scene_runtime.dirty_nodes.size());
            ImGui::Text("Render prep path: %s",
                prep_path_name(state.app.frame.render_prep_path));
        }

        if (ImGui::CollapsingHeader("Jobs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Total job time: %.3f ms", ticks_to_ms(total_ticks));
            ImGui::Text("Render prep time: %.3f ms", ticks_to_ms(prep_ticks));
            ImGui::Text("Slowest: %s  %.3f ms",
                slowest && slowest->name ? slowest->name : "<none>",
                slowest ? ticks_to_ms(slowest->duration_ticks()) : 0.0);
            ImGui::Separator();

            if (ImGui::BeginTable(
                "job_timings", 2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("ms");
                ImGui::TableHeadersRow();

                for (const auto& rec : profile.timings)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(rec.name ? rec.name : "<unnamed>");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", ticks_to_ms(rec.duration_ticks()));
                }

                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader("Render commands", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Opaque cmds:  %llu",
                static_cast<unsigned long long>(opaque_cmds));
            ImGui::Text("Splat cmds:   %llu",
                static_cast<unsigned long long>(splat_cmds));
            ImGui::Text("Total cmds:   %llu",
                static_cast<unsigned long long>(total_cmds));
        }

        if (ImGui::CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Visible opaque: %u", culling.visible_opaque);
            ImGui::Text("Culled opaque:  %u", culling.culled_opaque);
        }

        if (ImGui::CollapsingHeader("Record / Export"))
        {
            const wz::toolhost::RecorderAction action =
                wz::toolhost::draw_recorder_controls(state.recorder);

            if (action == wz::toolhost::RecorderAction::ExportRequested)
            {
                wz::toolhost::export_recording(
                    state.recorder,
                    state.app.ctx.device,
                    state.app.ctx.logger,
                    k_export_config);

                state.console.push_string(
                    "[bench] " + state.recorder.last_status);
            }
        }

        ImGui::End();
    }

    // ─── Navigation panel ─────────────────────────────────────────────────────

    void draw_navigation_panel(BenchSceneToolhostState& state)
    {
        ImGui::SetNextWindowPos(ImVec2(560.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340.0f, 280.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Navigation");

        ImGui::Text("Status: %s",
            state.app.navigation_active
                ? "ACTIVE — ESC to release"
                : "inactive — ESC to activate");
        ImGui::Separator();

        const auto& cam = state.app.camera;
        ImGui::Text("Position");
        ImGui::Text("  x: %.3f  y: %.3f  z: %.3f",
            cam.x, cam.y, cam.z);
        const auto& q = cam.orientation;
        ImGui::Text("  orientation: %.3f %.3f %.3f %.3f",
            q.x, q.y, q.z, q.w);

        ImGui::Separator();

        ImGui::SliderFloat("Move speed",
            &state.app.camera.move_speed,
            0.5f, 50.0f);
        ImGui::SliderFloat("Boost multiplier",
            &state.app.camera.boost_multiplier,
            1.0f, 10.0f);

        ImGui::Separator();

        ImGui::TextWrapped(
            "ESC: toggle navigation | BKSPC: quit\n"
            "WASD: move | E/Q: up/down | Shift: boost");

        ImGui::End();
    }

    // ─── Scene panel ──────────────────────────────────────────────────────────

    void draw_scene_panel(BenchSceneToolhostState& state)
    {
        ImGui::SetNextWindowPos(ImVec2(560.0f, 320.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340.0f, 220.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Scene");

        ImGui::Text("Scene: %dx%d grid of splats/Tree.ply (%d instances)",
            k_grid_x, k_grid_z, k_grid_x * k_grid_z);
        ImGui::Text("Splat handle: %s",
            state.splat_handle != wz::scene::INVALID_SPLAT
            ? "registered"
            : "not loaded");
        ImGui::Text("Scene nodes: %u",
            wz::core::graph::node_count(
                state.app.scene_runtime.scene.polytree));

        ImGui::Separator();

        const auto& culling = state.app.frame.render_ir.ir.culling;
        ImGui::Text("Visible splats:  %u", culling.visible_splats);
        ImGui::Text("Culled splats:   %u", culling.culled_splats);

        ImGui::End();
    }

} // namespace


// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    BenchSceneToolhostState state{};

    state.recorder.configure({
        "frame",
        "dt_ms",
        "fps",
        "frame_cpu_ms",
        "bench_update_ms",
        "begin_clear_ms",
        "submit_opaque_ms",
        "submit_splat_ms",
        "imgui_ms",
        "end_present_ms",

        "total_job_ms",
        "render_prep_job_ms",
        "build_view_ms",
        "compile_scene_ms",
        "build_render_ir_ms",
        "build_render_frame_ms",
        "slowest_job_ms",
        "opaque_commands",
        "splat_commands",
        "total_commands",
        "visible_opaque",
        "culled_opaque",
        "visible_splats",
        "culled_splats",
        "scene_nodes",
        "dirty_nodes",
    });

    // Build the scene definition.  pre_commit runs inside wz::bench::init()
    // while the asset registration phase is still open, so all assets — both
    // the debug shaders and the gaussian splat — share a single commit().
    wz::bench::SceneDefinition scene_def{};
    scene_def.name = "splat-only-benchmark";


    scene_def.pre_commit = [&state](wz::engine::assets::EngineAssetLibrary& assets) -> bool {
        return register_splat_assets(assets, state);
    };

    scene_def.build = [&state](wz::bench::SceneRuntime& runtime,
                              wz::engine::AppContext& ctx) -> bool {
        return build_splat_scene(runtime, ctx, state);
    };

    if (!wz::bench::init(state.app, std::move(scene_def)))
    {
        wz::bench::shutdown(state.app);
        return 1;
    }

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            static_cast<BenchSceneToolhostState*>(user)->console.push(record);
        },
        &state);

    // GPU realization now happens inside build_splat_scene() — the
    // scene_def.build callback — so no separate init_splat_gpu() call.

    if (!state.imgui.init(state.app.ctx.window, state.app.ctx.device))
    {
        state.imgui.shutdown(state.app.ctx.window);
        wz::logging::set_log_sink(state.app.ctx.logger, nullptr, nullptr);
        wz::bench::shutdown(state.app);
        return 1;
    }

    wz::engine::run([&state](wz::engine::Context& ctx, wz::engine::FrameContext& fctx)
    {
            state.timings = {};

            using Clock = std::chrono::steady_clock;

            auto elapsed_ms = [](Clock::time_point a, Clock::time_point b) -> double {
                return std::chrono::duration<double, std::milli>(b - a).count();
                };

            const auto frame_t0 = Clock::now();

            {
                const auto t0 = Clock::now();
                wz::bench::update(ctx, fctx, state.app);
                const auto t1 = Clock::now();

                state.timings.bench_update_ms = elapsed_ms(t0, t1);
            }

            if (!ctx.running)
                return;

            {
                const auto t0 = Clock::now();

                wz::gpu::begin_frame(state.app.ctx.device);
                wz::gpu::clear(state.app.ctx.device, 0.05f, 0.05f, 0.1f, 1.0f);

                const auto t1 = Clock::now();
                state.timings.begin_clear_ms = elapsed_ms(t0, t1);
            }

            {
                const auto t0 = Clock::now();

                // Track B: submit opaque scene (2-arg path).
                wz::bench::render_contents(state.app, fctx);

                const auto t1 = Clock::now();
                state.timings.submit_opaque_ms = elapsed_ms(t0, t1);
            }

            {
                const auto t0 = Clock::now();

                // Submit scene-owned splats through the 5-arg path.
                // No patching — the renderer produces the commands, we submit them.
                const auto& rf = state.app.frame.render_frame.frame;
                if (!rf.splats.empty())
                {
                    wz::render::RenderFrameView splat_frame{};
                    splat_frame.splats = rf.splats;
                    splat_frame.view   = rf.view;

                    wz::gpu::dx12::submit_render_frame(
                        state.app.ctx.device, splat_frame,
                        state.resolver, state.pipeline_cache,
                        state.render_program_cache);
                }

                const auto t1 = Clock::now();
                state.timings.submit_splat_ms = elapsed_ms(t0, t1);
            }

            {
                const auto t0 = Clock::now();

                state.imgui.begin_frame(state.app.ctx.device, fctx);

                draw_benchmark_panel(state, fctx);
                draw_navigation_panel(state);
                draw_scene_panel(state);
                wz::toolhost::draw_console_panel(state.console);

                ImGui::Render();
                state.imgui.render(state.app.ctx.device);

                const auto t1 = Clock::now();
                state.timings.imgui_ms = elapsed_ms(t0, t1);
            }

            {
                const auto t0 = Clock::now();

                wz::gpu::end_frame(state.app.ctx.device);
                wz::gpu::present(state.app.ctx.device);

                const auto t1 = Clock::now();
                state.timings.end_present_ms = elapsed_ms(t0, t1);
            }

            const auto frame_t1 = Clock::now();
            state.timings.frame_cpu_ms = elapsed_ms(frame_t0, frame_t1);
            record_benchmark_row(state, fctx);
    });

    state.imgui.shutdown(state.app.ctx.window);
    state.renderable_cache.clear();

    wz::logging::set_log_sink(state.app.ctx.logger, nullptr, nullptr);
    wz::logging::wait_until_idle(state.app.ctx.logger);

    wz::bench::shutdown(state.app);

    return 0;
}
