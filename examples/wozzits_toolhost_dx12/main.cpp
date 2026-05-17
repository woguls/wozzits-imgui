#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <engine/engine.h>
#include <engine/game_app.h>
#include <engine/game_app_benchmark.h>

#include <logging/logging.h>
#include <logging/logger.h>

#include <window/window2.h>

#include <gpu/gpu.h>

#include <wozzits/toolhost/tool_console.h>
#include <wozzits/toolhost/imgui_dx12_context.h>
#include <wozzits/toolhost/benchmark_recorder.h>

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
#include <wozzits/script/script_host.h>
#endif

#include <imgui.h>

#include <cassert>
#include <cstdint>

namespace
{
    struct PlatformFrameStats
    {
        double update_ms = 0.0;
    };

    struct ToolhostState
    {
        wz::app::GameApp app{};

        wz::toolhost::ImGuiDx12ToolhostContext imgui{};
        wz::toolhost::ToolConsole              console{};
        wz::toolhost::BenchmarkRecorder        recorder{};

        PlatformFrameStats platform{};

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
        wz::script::ScriptHost* scripts = nullptr;

        struct ScriptTextPanel
        {
            std::string title;
            std::string text;
        };

        struct ScriptStatsPanel
        {
            std::string title;
            std::vector<std::pair<std::string, std::string>> rows;
        };

        struct ScriptButtonPanel
        {
            std::string title;
            std::vector<std::pair<std::string, std::string>> buttons;
        };

        std::vector<ScriptTextPanel>   script_text_panels;
        std::vector<ScriptStatsPanel>  script_stats_panels;
        std::vector<ScriptButtonPanel> script_button_panels;
#endif
    };


#ifdef WOZZITS_ENABLE_V8_SCRIPTING

    template<typename T>
    void upsert_script_panel(std::vector<T>& panels, T panel)
    {
        for (T& p : panels)
        {
            if (p.title == panel.title)
            {
                p = std::move(panel);
                return;
            }
        }

        panels.push_back(std::move(panel));
    }

    std::string copy_panel_str(const char* text)
    {
        return text ? std::string(text) : std::string{};
    }

    void drain_script_panels(ToolhostState& state)
    {
        {
            const std::size_t count =
                wz::script::pending_text_panel_count(state.scripts);

            for (std::size_t i = 0; i < count; ++i)
            {
                const char* t =
                    wz::script::pending_text_panel_title(
                        state.scripts,
                        i,
                        nullptr);

                if (t == nullptr || t[0] == '\0')
                    continue;

                const char* x =
                    wz::script::pending_text_panel_text(
                        state.scripts,
                        i,
                        nullptr);

                ToolhostState::ScriptTextPanel panel;
                panel.title = copy_panel_str(t);
                panel.text = copy_panel_str(x);
                upsert_script_panel(
                    state.script_text_panels,
                    std::move(panel));
            }

            wz::script::clear_pending_text_panels(state.scripts);
        }

        {
            const std::size_t count =
                wz::script::pending_stats_panel_count(state.scripts);

            for (std::size_t i = 0; i < count; ++i)
            {
                const char* t =
                    wz::script::pending_stats_panel_title(
                        state.scripts,
                        i,
                        nullptr);

                if (t == nullptr || t[0] == '\0')
                    continue;

                ToolhostState::ScriptStatsPanel panel;
                panel.title = copy_panel_str(t);

                const std::size_t row_count =
                    wz::script::pending_stats_panel_row_count(
                        state.scripts,
                        i);

                for (std::size_t r = 0; r < row_count; ++r)
                {
                    const char* l =
                        wz::script::pending_stats_panel_row_label(
                            state.scripts,
                            i,
                            r,
                            nullptr);

                    const char* v =
                        wz::script::pending_stats_panel_row_value(
                            state.scripts,
                            i,
                            r,
                            nullptr);

                    panel.rows.emplace_back(
                        copy_panel_str(l),
                        copy_panel_str(v));
                }

                upsert_script_panel(
                    state.script_stats_panels,
                    std::move(panel));
            }

            wz::script::clear_pending_stats_panels(state.scripts);
        }

        {
            const std::size_t count =
                wz::script::pending_button_panel_count(state.scripts);

            for (std::size_t i = 0; i < count; ++i)
            {
                const char* t =
                    wz::script::pending_button_panel_title(
                        state.scripts,
                        i,
                        nullptr);

                if (t == nullptr || t[0] == '\0')
                    continue;

                ToolhostState::ScriptButtonPanel panel;
                panel.title = copy_panel_str(t);

                const std::size_t btn_count =
                    wz::script::pending_button_panel_button_count(
                        state.scripts,
                        i);

                for (std::size_t b = 0; b < btn_count; ++b)
                {
                    const char* l =
                        wz::script::pending_button_panel_button_label(
                            state.scripts,
                            i,
                            b,
                            nullptr);

                    const char* a =
                        wz::script::pending_button_panel_button_action(
                            state.scripts,
                            i,
                            b,
                            nullptr);

                    panel.buttons.emplace_back(
                        copy_panel_str(l),
                        copy_panel_str(a));
                }

                upsert_script_panel(
                    state.script_button_panels,
                    std::move(panel));
            }

            wz::script::clear_pending_button_panels(state.scripts);
        }

        {
            const std::size_t count =
                wz::script::log_count(state.scripts);

            for (std::size_t i = 0; i < count; ++i)
            {
                const char* m =
                    wz::script::log_message(
                        state.scripts,
                        i,
                        nullptr);

                if (m != nullptr && m[0] != '\0')
                    state.console.push_string(std::string("[script] ") + m);
            }

            wz::script::clear_logs(state.scripts);
        }
    }

    static constexpr const char* k_startup_script =
        "wz.log('wozzits toolhost script host online');"
        "wz.tool.textPanel('Script Console', 'V8 scripting host is running.');"
        "wz.tool.statsPanel('Script Info', ["
        "  ['status', 'running'],"
        "  ['host',   'wozzits-toolhost']"
        "]);"
        "wz.tool.buttonPanel('Script Tools', ["
        "  ['Reload scripts',  'scripts.reload'],"
        "  ['Clear logs',      'scripts.clear_logs']"
        "]);"
        "'startup ok';";

    void dispatch_script_action(
        ToolhostState& state,
        const std::string& action)
    {
        if (action == "scripts.reload")
        {
            wz::script::clear_logs(state.scripts);
            state.console.push_string("[script] reloading scripts");
            wz::script::run_source(
                state.scripts,
                "startup.js",
                k_startup_script);
            drain_script_panels(state);
        }
        else if (action == "scripts.clear_logs")
        {
            wz::script::clear_logs(state.scripts);
            state.console.push_string("[script] logs cleared");
        }
        else
        {
            state.console.push_string("[script] unknown action: " + action);
        }
    }

    void draw_script_panels(ToolhostState& state)
    {
        float script_panel_x = 560.0f;
        float script_panel_y = 24.0f;

        for (const auto& panel : state.script_text_panels)
        {
            ImGui::SetNextWindowPos(
                ImVec2(script_panel_x, script_panel_y),
                ImGuiCond_FirstUseEver);

            ImGui::SetNextWindowSize(
                ImVec2(300.0f, 120.0f),
                ImGuiCond_FirstUseEver);

            ImGui::Begin(panel.title.c_str());
            script_panel_y += 140.0f;

            ImGui::TextUnformatted(
                panel.text.c_str(),
                panel.text.c_str() + panel.text.size());

            ImGui::End();
        }

        for (const auto& panel : state.script_stats_panels)
        {
            ImGui::SetNextWindowPos(
                ImVec2(script_panel_x, script_panel_y),
                ImGuiCond_FirstUseEver);

            ImGui::SetNextWindowSize(
                ImVec2(300.0f, 200.0f),
                ImGuiCond_FirstUseEver);

            ImGui::Begin(panel.title.c_str());
            script_panel_y += 220.0f;

            if (ImGui::BeginTable(
                panel.title.c_str(),
                2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Field");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();

                for (const auto& [label, value] : panel.rows)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(value.c_str());
                }

                ImGui::EndTable();
            }

            ImGui::End();
        }

        std::string pending_action;

        for (const auto& panel : state.script_button_panels)
        {
            ImGui::SetNextWindowPos(
                ImVec2(script_panel_x, script_panel_y),
                ImGuiCond_FirstUseEver);

            ImGui::SetNextWindowSize(
                ImVec2(240.0f, 160.0f),
                ImGuiCond_FirstUseEver);

            script_panel_y += 180.0f;

            if (ImGui::Begin(panel.title.c_str()))
            {
                for (const auto& [label, action] : panel.buttons)
                {
                    if (label.empty())
                        continue;

                    if (ImGui::Button(label.c_str()))
                        pending_action = action;
                }
            }

            ImGui::End();
        }

        if (!pending_action.empty())
            dispatch_script_action(state, pending_action);
    }

#endif // WOZZITS_ENABLE_V8_SCRIPTING


    // ── Scene-specific benchmark panel ────────────────────────────────────────

    static const wz::toolhost::BenchmarkExportConfig k_export_config = {
        .name_prefix = "bench/recording",
        .frame_column = "frame",
        .metric_columns = {
            "dt_ms",
            "fps",
            "platform_update_ms",
            "total_job_ms",
            "render_prep_job_ms",
            "slowest_job_ms",
            "opaque_commands",
            "splat_commands",
            "transparent_commands",
            "particle_commands",
            "total_commands",
            "bytes_owned",
            "bytes_allocated",
            "reallocations",
            "scene_nodes",
            "dirty_nodes",
            "debug_renderables",
            "debug_object_ready",
            "compiled_scene_valid",
        },
    };

    void draw_benchmark_panel(
        ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        const wz::app::GameAppBenchmarkSnapshot snap =
            wz::app::benchmark_snapshot(state.app, fctx);

        // ── Accumulate recording row ──────────────────────────────────────────
        if (state.recorder.recording())
        {
            char buf[32];

            auto ds = [&](double v) -> std::string {
                std::snprintf(buf, sizeof(buf), "%.6g", v);
                return buf;
                };

            const uint32_t frame_idx =
                static_cast<uint32_t>(state.recorder.frame_count());

            state.recorder.push_row({
                std::to_string(frame_idx),
                ds(snap.dt_seconds * 1000.0),
                ds(snap.fps),
                ds(state.platform.update_ms),
                ds(snap.total_job_ms),
                ds(snap.render_prep_job_ms),
                ds(snap.slowest_job_ms),
                std::to_string(snap.opaque_commands),
                std::to_string(snap.splat_commands),
                std::to_string(snap.transparent_commands),
                std::to_string(snap.particle_commands),
                std::to_string(snap.total_commands),
                std::to_string(snap.bytes_owned),
                std::to_string(snap.bytes_allocated_this_frame),
                std::to_string(snap.reallocations_this_frame),
                std::to_string(snap.scene_nodes),
                std::to_string(snap.dirty_nodes),
                std::to_string(snap.debug_renderables),
                std::to_string(snap.debug_object_ready ? 1u : 0u),
                std::to_string(snap.compiled_scene_valid ? 1u : 0u),
                });
        }

        ImGui::SetNextWindowPos(
            ImVec2(24.0f, 24.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowSize(
            ImVec2(520.0f, 420.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::Begin("Wozzits Benchmark");

        ImGui::Text("Scene compiler / culler benchmark");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Frame: %llu",
                static_cast<unsigned long long>(snap.frame_index));

            ImGui::Text(
                "dt: %.4f ms",
                snap.dt_seconds * 1000.0);

            ImGui::Text(
                "FPS: %.1f",
                snap.fps);

            ImGui::Text(
                "Platform/update: %.4f ms",
                state.platform.update_ms);
        }

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Debug object ready: %s",
                snap.debug_object_ready ? "yes" : "no");

            ImGui::Text(
                "Compiled scene valid: %s",
                snap.compiled_scene_valid ? "yes" : "no");

            ImGui::Text(
                "Scene nodes: %llu",
                static_cast<unsigned long long>(snap.scene_nodes));

            ImGui::Text(
                "Dirty transform nodes: %llu",
                static_cast<unsigned long long>(snap.dirty_nodes));
        }

        if (ImGui::CollapsingHeader("Jobs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Total jobs: %u", snap.job_count);
            ImGui::Text("Total job time: %.3f ms", snap.total_job_ms);
            ImGui::Text("Render prep time: %.3f ms", snap.render_prep_job_ms);

            ImGui::Text(
                "Slowest: %s %.3f ms",
                snap.slowest_job_name ? snap.slowest_job_name : "<none>",
                snap.slowest_job_ms);

            ImGui::Separator();

            if (ImGui::BeginTable(
                "job timings",
                2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("ms");
                ImGui::TableHeadersRow();

                for (uint32_t i = 0; i < snap.job_count; ++i)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(
                        snap.jobs[i].name
                        ? snap.jobs[i].name
                        : "<unnamed>");

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", snap.jobs[i].ms);
                }

                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader("Render prep", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Path: %s",
                snap.render_prep_path ? snap.render_prep_path : "Unknown");
        }

        if (ImGui::CollapsingHeader("Render commands", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Opaque: %llu",
                static_cast<unsigned long long>(snap.opaque_commands));

            ImGui::Text(
                "Splats: %llu",
                static_cast<unsigned long long>(snap.splat_commands));

            ImGui::Text(
                "Transparent: %llu",
                static_cast<unsigned long long>(snap.transparent_commands));

            ImGui::Text(
                "Particles: %llu",
                static_cast<unsigned long long>(snap.particle_commands));

            ImGui::Separator();

            ImGui::Text(
                "Total: %llu",
                static_cast<unsigned long long>(snap.total_commands));
        }

        if (ImGui::CollapsingHeader("Frame allocations", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Owned bytes: %llu",
                static_cast<unsigned long long>(snap.bytes_owned));

            ImGui::Text(
                "Allocated this frame: %llu",
                static_cast<unsigned long long>(
                    snap.bytes_allocated_this_frame));

            ImGui::Text(
                "Reallocations this frame: %u",
                snap.reallocations_this_frame);
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

    void draw_toolhost_panel(
        const ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        const wz::app::GameApp& app = state.app;

        ImGui::SetNextWindowPos(
            ImVec2(24.0f, 24.0f),
            ImGuiCond_Once);

        ImGui::SetNextWindowSize(
            ImVec2(420.0f, 220.0f),
            ImGuiCond_Once);

        ImGui::Begin("Wozzits Toolhost");

        ImGui::Text("Running GameApp with ImGui overlay");
        ImGui::Separator();

        ImGui::Text(
            "Frame: %llu",
            static_cast<unsigned long long>(fctx.frame.index));

        ImGui::Text(
            "Camera: x=%.3f y=%.3f",
            app.camera.x,
            app.camera.y);

        ImGui::Text(
            "Debug objects ready: %s",
            app.debug_object.ready ? "yes" : "no");

        ImGui::Text(
            "Compiled scene valid: %s",
            app.debug_object.compiled_scene_valid ? "yes" : "no");

        ImGui::End();
    }


    // ── Engine update callback ────────────────────────────────────────────────

    void toolhost_update(
        wz::engine::Context& fctx_owner,
        wz::engine::FrameContext& fctx,
        void* user_data)
    {
        auto* state = static_cast<ToolhostState*>(user_data);
        assert(state != nullptr);

        state->platform = {};

        const auto platform_begin =
            std::chrono::steady_clock::now();

        wz::app::update(fctx_owner, fctx, state->app);

        const auto platform_end =
            std::chrono::steady_clock::now();

        state->platform.update_ms =
            std::chrono::duration<double, std::milli>(
                platform_end - platform_begin).count();

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
        if (state->scripts != nullptr)
            drain_script_panels(*state);
#endif

        if (!fctx_owner.running)
            return;

        wz::gpu::begin_frame(state->app.ctx.device);

        wz::gpu::clear(
            state->app.ctx.device,
            0.0f,
            0.15f,
            0.35f,
            1.0f);

        wz::app::render_contents(state->app, fctx);

        state->imgui.begin_frame(state->app.ctx.device, fctx);

        draw_benchmark_panel(*state, fctx);
        draw_toolhost_panel(*state, fctx);
        wz::toolhost::draw_console_panel(state->console);

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
        if (state->scripts != nullptr)
            draw_script_panels(*state);
#endif

        ImGui::Render();

        state->imgui.render(state->app.ctx.device);

        wz::gpu::end_frame(state->app.ctx.device);
        wz::gpu::present(state->app.ctx.device);
    }

} // namespace


// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    ToolhostState state{};

    // Configure the recorder columns once at startup.
    state.recorder.configure({
        "frame",
        "dt_ms",
        "fps",
        "platform_update_ms",
        "total_job_ms",
        "render_prep_job_ms",
        "slowest_job_ms",
        "opaque_commands",
        "splat_commands",
        "transparent_commands",
        "particle_commands",
        "total_commands",
        "bytes_owned",
        "bytes_allocated",
        "reallocations",
        "scene_nodes",
        "dirty_nodes",
        "debug_renderables",
        "debug_object_ready",
        "compiled_scene_valid",
        });

    if (!wz::app::init(state.app))
        return 1;

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            static_cast<ToolhostState*>(user)->console.push(record);
        },
        &state);

    if (!state.imgui.init(state.app.ctx.window, state.app.ctx.device))
    {
        state.imgui.shutdown(state.app.ctx.window);
        wz::app::shutdown(state.app);
        return 1;
    }

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
    if (!wz::script::init_v8_platform())
    {
        state.imgui.shutdown(state.app.ctx.window);
        wz::app::shutdown(state.app);
        return 1;
    }

    state.scripts = wz::script::create_v8_script_host();

    if (state.scripts == nullptr || !wz::script::initialize(state.scripts))
    {
        wz::script::destroy_v8_script_host(state.scripts);
        wz::script::shutdown_v8_platform();
        state.imgui.shutdown(state.app.ctx.window);
        wz::app::shutdown(state.app);
        return 1;
    }

    {
        wz::script::run_source(
            state.scripts,
            "startup.js",
            k_startup_script);

        drain_script_panels(state);
    }
#endif

    wz::engine::run(toolhost_update, &state);

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
    wz::script::destroy_v8_script_host(state.scripts);
    state.scripts = nullptr;
    wz::script::shutdown_v8_platform();
#endif

    state.imgui.shutdown(state.app.ctx.window);

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        nullptr,
        nullptr);

    wz::logging::wait_until_idle(state.app.ctx.logger);

    wz::app::shutdown(state.app);

    return 0;
}