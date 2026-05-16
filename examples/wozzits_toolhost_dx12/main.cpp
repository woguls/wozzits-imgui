#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <mutex>
#include <string>
#include <vector>

#include <engine/engine.h>
#include <engine/game_app.h>

#include <logging/logging.h>
#include <logging/logger.h>

#include <window/window2.h>

#include <gpu/gpu.h>
#include <gpu/dx12/dx12_internal.h>

#include <wozzits/imgui/imgui_context.h>

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
#include <wozzits/script/script_host.h>
#endif

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <engine/game_app_benchmark.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam);

#include <d3d12.h>

#include <cassert>
#include <cstdint>
#include <ctime>
#include <iostream>

namespace
{
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

    struct ConsoleLine
    {
        wz::LogLevel level = wz::LogLevel::Info;
        uint64_t sequence = 0;
        uint64_t event_ticks = 0;
        std::string text;
    };

    struct FrameRecord
    {
        uint32_t frame                       = 0;
        double   dt_ms                       = 0.0;
        double   fps                         = 0.0;
        double   total_job_ms                = 0.0;
        double   render_prep_job_ms          = 0.0;
        double   slowest_job_ms              = 0.0;
        uint64_t opaque_commands             = 0;
        uint64_t splat_commands              = 0;
        uint64_t transparent_commands        = 0;
        uint64_t particle_commands           = 0;
        uint64_t total_commands              = 0;
        uint64_t bytes_owned                 = 0;
        uint64_t bytes_allocated_this_frame  = 0;
        uint32_t reallocations_this_frame    = 0;
        uint64_t scene_nodes                 = 0;
        uint64_t dirty_nodes                 = 0;
        uint64_t debug_renderables           = 0;
        uint32_t debug_object_ready          = 0;
        uint32_t compiled_scene_valid        = 0;
    };

    struct DiagRecorder
    {
        bool     recording     = false;
        int      bucket_frames = 60;
        std::vector<FrameRecord> frames;
        std::string last_status;
    };

    struct ToolConsole
    {
        std::mutex mutex;
        std::vector<ConsoleLine> lines;
        bool auto_scroll = true;
        bool scroll_to_bottom = false;

        void push_string(std::string text, wz::LogLevel level = wz::LogLevel::Info)
        {
            std::lock_guard lock(mutex);
            lines.push_back(ConsoleLine{
                .level = level,
                .text  = std::move(text),
            });
            scroll_to_bottom = true;
        }

        void push(const wz::logging::LogRecordView& record)
        {
            if (record.text == nullptr)
                return;

            std::lock_guard lock(mutex);

            lines.push_back(ConsoleLine{
                .level = record.level,
                .sequence = record.sequence,
                .event_ticks = record.event_ticks,
                .text = std::string(
                    record.text,
                    record.text + record.text_size),
                });

            constexpr std::size_t kMaxLines = 4000;

            if (lines.size() > kMaxLines)
            {
                lines.erase(
                    lines.begin(),
                    lines.begin()
                    + static_cast<std::ptrdiff_t>(
                        lines.size() - kMaxLines));
            }

            scroll_to_bottom = true;
        }
    };

    struct ToolhostState
    {
        wz::app::GameApp app{};

        ID3D12DescriptorHeap* imgui_srv_heap = nullptr;
        ImGuiDx12SrvAllocator imgui_srv_allocator{};

        ToolConsole  console{};
        DiagRecorder diag_recorder{};

        bool imgui_ready = false;

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
                const char* t = wz::script::pending_text_panel_title(state.scripts, i, nullptr);
                if (t == nullptr || t[0] == '\0') continue;
                const char* x = wz::script::pending_text_panel_text(state.scripts, i, nullptr);

                ToolhostState::ScriptTextPanel panel;
                panel.title = copy_panel_str(t);
                panel.text  = copy_panel_str(x);
                upsert_script_panel(state.script_text_panels, std::move(panel));
            }
            wz::script::clear_pending_text_panels(state.scripts);
        }

        {
            const std::size_t count =
                wz::script::pending_stats_panel_count(state.scripts);

            for (std::size_t i = 0; i < count; ++i)
            {
                const char* t = wz::script::pending_stats_panel_title(state.scripts, i, nullptr);
                if (t == nullptr || t[0] == '\0') continue;

                ToolhostState::ScriptStatsPanel panel;
                panel.title = copy_panel_str(t);

                const std::size_t row_count =
                    wz::script::pending_stats_panel_row_count(state.scripts, i);

                for (std::size_t r = 0; r < row_count; ++r)
                {
                    const char* l = wz::script::pending_stats_panel_row_label(state.scripts, i, r, nullptr);
                    const char* v = wz::script::pending_stats_panel_row_value(state.scripts, i, r, nullptr);
                    panel.rows.emplace_back(copy_panel_str(l), copy_panel_str(v));
                }
                upsert_script_panel(state.script_stats_panels, std::move(panel));
            }
            wz::script::clear_pending_stats_panels(state.scripts);
        }

        {
            const std::size_t count =
                wz::script::pending_button_panel_count(state.scripts);

            for (std::size_t i = 0; i < count; ++i)
            {
                const char* t = wz::script::pending_button_panel_title(state.scripts, i, nullptr);
                if (t == nullptr || t[0] == '\0') continue;

                ToolhostState::ScriptButtonPanel panel;
                panel.title = copy_panel_str(t);

                const std::size_t btn_count =
                    wz::script::pending_button_panel_button_count(state.scripts, i);

                for (std::size_t b = 0; b < btn_count; ++b)
                {
                    const char* l = wz::script::pending_button_panel_button_label(state.scripts, i, b, nullptr);
                    const char* a = wz::script::pending_button_panel_button_action(state.scripts, i, b, nullptr);
                    panel.buttons.emplace_back(copy_panel_str(l), copy_panel_str(a));
                }
                upsert_script_panel(state.script_button_panels, std::move(panel));
            }
            wz::script::clear_pending_button_panels(state.scripts);
        }

        {
            const std::size_t count = wz::script::log_count(state.scripts);
            for (std::size_t i = 0; i < count; ++i)
            {
                const char* m = wz::script::log_message(state.scripts, i, nullptr);
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

    void dispatch_script_action(ToolhostState& state, const std::string& action)
    {
        if (action == "scripts.reload")
        {
            wz::script::clear_logs(state.scripts);
            state.console.push_string("[script] reloading scripts");
            wz::script::run_source(state.scripts, "startup.js", k_startup_script);
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
            ImGui::SetNextWindowPos(ImVec2(script_panel_x, script_panel_y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300.0f, 120.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin(panel.title.c_str());
            script_panel_y += 140.0f;
            ImGui::TextUnformatted(
                panel.text.c_str(),
                panel.text.c_str() + panel.text.size());
            ImGui::End();
        }

        for (const auto& panel : state.script_stats_panels)
        {
            ImGui::SetNextWindowPos(ImVec2(script_panel_x, script_panel_y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin(panel.title.c_str());
            script_panel_y += 220.0f;
            if (ImGui::BeginTable(
                panel.title.c_str(), 2,
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
            ImGui::SetNextWindowPos(ImVec2(script_panel_x, script_panel_y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(240.0f, 160.0f), ImGuiCond_FirstUseEver);
            script_panel_y += 180.0f;
            if (ImGui::Begin(panel.title.c_str()))
            {
                for (const auto& [label, action] : panel.buttons)
                {
                    if (label.empty()) continue;
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

    static ToolhostState* g_toolhost_state = nullptr;

    bool init_imgui_for_app(ToolhostState& state)
    {
        wz::gpu::Device& device =
            state.app.ctx.device;

        void* hwnd =
            wz::window::get_native_handle(state.app.ctx.window);

        if (hwnd == nullptr)
            return false;

        ID3D12Device* dx_device =
            wz::gpu::dx12::internal::get_device(device);

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

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 0.85f;
        }

        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd))
            return false;

        wz::window::set_native_message_hook(
            state.app.ctx.window,
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
            wz::gpu::dx12::internal::get_command_queue(device);

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
                assert(g_toolhost_state != nullptr);
                g_toolhost_state->imgui_srv_allocator.alloc(
                    out_cpu,
                    out_gpu);
            };

        init_info.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*,
                D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                D3D12_GPU_DESCRIPTOR_HANDLE gpu)
            {
                assert(g_toolhost_state != nullptr);
                g_toolhost_state->imgui_srv_allocator.free(
                    cpu,
                    gpu);
            };

        if (!ImGui_ImplDX12_Init(&init_info))
            return false;

        state.imgui_ready = true;
        return true;
    }

    void shutdown_imgui(ToolhostState& state)
    {
        if (state.imgui_ready)
        {
            ImGui_ImplDX12_Shutdown();
            wz::window::set_native_message_hook(
                state.app.ctx.window,
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

    void begin_imgui_frame(
        ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        wz::gpu::Device& device =
            state.app.ctx.device;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGuiIO& io = ImGui::GetIO();

        const UINT width =
            wz::gpu::dx12::internal::get_width(device);

        const UINT height =
            wz::gpu::dx12::internal::get_height(device);

        io.DisplaySize = ImVec2(
            static_cast<float>(width),
            static_cast<float>(height));

        const double dt =
            fctx.frame.delta_seconds();

        io.DeltaTime =
            dt > 0.0 ? static_cast<float>(dt) : (1.0f / 60.0f);

        ImGui::NewFrame();
    }

    const char* log_level_name(wz::LogLevel level)
    {
        switch (level)
        {
        case wz::LogLevel::Debug:
            return "DEBUG";

        case wz::LogLevel::Info:
            return "INFO";

        case wz::LogLevel::Warning:
            return "WARN";

        case wz::LogLevel::Error:
            return "ERROR";

        case wz::LogLevel::Critical:
            return "CRITICAL";
        }

        return "UNKNOWN";
    }

    void draw_console_panel(ToolhostState& state)
    {
        ImGui::SetNextWindowSize(
            ImVec2(760.0f, 360.0f),
            ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::Begin("Wozzits Console");

        if (ImGui::Button("Clear"))
        {
            std::lock_guard lock(state.console.mutex);
            state.console.lines.clear();
            state.console.scroll_to_bottom = false;
        }

        ImGui::SameLine();

        ImGui::Checkbox(
            "Auto-scroll",
            &state.console.auto_scroll);

        ImGui::Separator();

        if (ImGui::BeginChild(
            "console_scroll",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar))
        {
            std::lock_guard lock(state.console.mutex);

            for (const ConsoleLine& line : state.console.lines)
            {
                ImGui::Text("[%s]", log_level_name(line.level));
                ImGui::SameLine();
                ImGui::TextUnformatted(line.text.c_str());
            }

            if (state.console.auto_scroll && state.console.scroll_to_bottom)
            {
                ImGui::SetScrollHereY(1.0f);
                state.console.scroll_to_bottom = false;
            }
        }

        ImGui::EndChild();
        ImGui::End();
    }

    void export_benchmark_recording(ToolhostState& state)
    {
        DiagRecorder& rec = state.diag_recorder;

        if (rec.frames.empty()) {
            rec.last_status = "No frames recorded.";
            return;
        }

        if (!state.app.ctx.assets) {
            rec.last_status = "Asset library not available.";
            return;
        }

        wz::engine::assets::EngineAssetLibrary assets{
            state.app.ctx.device,
            state.app.ctx.logger,
            ".",
        };

        // ── Build DataTableData ───────────────────────────────────────────────────
        using wz::engine::assets::DataTableData;
        using wz::engine::assets::DataTableColumn;
        using wz::engine::assets::DataTableRow;

        DataTableData table;
        for (const char* col : {
            "frame",
            "dt_ms", "fps",
            "total_job_ms", "render_prep_job_ms", "slowest_job_ms",
            "opaque_commands", "splat_commands", "transparent_commands",
            "particle_commands", "total_commands",
            "bytes_owned", "bytes_allocated", "reallocations",
            "scene_nodes", "dirty_nodes", "debug_renderables",
            "debug_object_ready", "compiled_scene_valid",
        }) {
            table.columns.push_back({ .name = col });
        }

        char buf[32];
        auto ds = [&](double v) -> std::string {
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            return buf;
        };

        for (const FrameRecord& f : rec.frames) {
            table.rows.push_back({ .cells = {
                std::to_string(f.frame),
                ds(f.dt_ms), ds(f.fps),
                ds(f.total_job_ms), ds(f.render_prep_job_ms), ds(f.slowest_job_ms),
                std::to_string(f.opaque_commands),
                std::to_string(f.splat_commands),
                std::to_string(f.transparent_commands),
                std::to_string(f.particle_commands),
                std::to_string(f.total_commands),
                std::to_string(f.bytes_owned),
                std::to_string(f.bytes_allocated_this_frame),
                std::to_string(f.reallocations_this_frame),
                std::to_string(f.scene_nodes),
                std::to_string(f.dirty_nodes),
                std::to_string(f.debug_renderables),
                std::to_string(f.debug_object_ready),
                std::to_string(f.compiled_scene_valid),
            }});
        }

        // ── Register assets ───────────────────────────────────────────────────────
        const uint32_t last = rec.frames.back().frame;

        const auto source = assets.data_tables().create_inline_table({
            .name  = "bench/recording/raw",
            .table = std::move(table),
        });

        if (!source.valid()) {
            rec.last_status = "Failed to create source table.";
            return;
        }

        const std::vector<std::string> metrics = {
            "dt_ms", "fps",
            "total_job_ms", "render_prep_job_ms", "slowest_job_ms",
            "opaque_commands", "splat_commands", "transparent_commands",
            "particle_commands", "total_commands",
            "bytes_owned", "bytes_allocated", "reallocations",
            "scene_nodes", "dirty_nodes", "debug_renderables",
            "debug_object_ready", "compiled_scene_valid",
        };

        const auto summary = assets.diagnostic_timeframe_summaries().create_timeframe_summary({
            .name              = "bench/recording/summary",
            .source            = source,
            .frame_column      = "frame",
            .metric_columns    = metrics,
            .frame_start       = 0,
            .frame_end         = last,
            .frames_per_bucket = static_cast<uint32_t>(
                rec.bucket_frames > 0 ? rec.bucket_frames : 0),
        });

        if (!summary.valid()) {
            rec.last_status = "Failed to create summary.";
            return;
        }

        const auto view = assets.diagnostic_timeframe_summaries().create_data_table_view(
            "bench/recording/view", summary);

        if (!view.valid()) {
            rec.last_status = "Failed to create table view.";
            return;
        }

        const auto csv_asset = assets.csv_export().create_csv_export({
            .name   = "bench/recording/csv",
            .source = view,
        });

        if (!csv_asset.valid()) {
            rec.last_status = "Failed to create CSV export.";
            return;
        }

        // ── Compile ───────────────────────────────────────────────────────────────
        if (!assets.commit()) {
            rec.last_status = "Asset commit failed.";
            return;
        }

        const auto report = assets.resolve_all();
        if (!report.ok()) {
            rec.last_status = "Resolve failed ("
                + std::to_string(report.failures.size()) + " errors).";
            return;
        }

        const auto csv_handle = assets.csv_export().get_export(csv_asset);
        if (!csv_handle.valid()) {
            rec.last_status = "CSV export handle invalid after resolve.";
            return;
        }

        // ── Write file ────────────────────────────────────────────────────────────
        std::time_t now = std::time(nullptr);
        std::tm     tm_now{};
        ::localtime_s(&tm_now, &now);
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_now);

        const std::string path =
            std::string("benchmark_") + timestamp + ".csv";

        const wz::fs::FileError err =
            assets.csv_export().write_export_to_file(csv_handle, path);

        if (err != wz::fs::FileError::None) {
            rec.last_status = "Write failed: " + path;
        } else {
            rec.last_status = "Exported: " + path;
            state.console.push_string("[bench] " + rec.last_status);
        }
    }

    void draw_benchmark_panel(
        ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        const wz::app::GameAppBenchmarkSnapshot snap =
            wz::app::benchmark_snapshot(
                state.app,
                fctx);

        if (state.diag_recorder.recording) {
            const uint32_t idx =
                static_cast<uint32_t>(state.diag_recorder.frames.size());
            state.diag_recorder.frames.push_back(FrameRecord{
                .frame                      = idx,
                .dt_ms                      = snap.dt_seconds * 1000.0,
                .fps                        = snap.fps,
                .total_job_ms               = snap.total_job_ms,
                .render_prep_job_ms         = snap.render_prep_job_ms,
                .slowest_job_ms             = snap.slowest_job_ms,
                .opaque_commands            = snap.opaque_commands,
                .splat_commands             = snap.splat_commands,
                .transparent_commands       = snap.transparent_commands,
                .particle_commands          = snap.particle_commands,
                .total_commands             = snap.total_commands,
                .bytes_owned                = snap.bytes_owned,
                .bytes_allocated_this_frame = snap.bytes_allocated_this_frame,
                .reallocations_this_frame   = snap.reallocations_this_frame,
                .scene_nodes                = snap.scene_nodes,
                .dirty_nodes                = snap.dirty_nodes,
                .debug_renderables          = snap.debug_renderables,
                .debug_object_ready         = snap.debug_object_ready  ? 1u : 0u,
                .compiled_scene_valid       = snap.compiled_scene_valid ? 1u : 0u,
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

            if (ImGui::BeginTable("job timings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("ms");
                ImGui::TableHeadersRow();

                for (uint32_t i = 0; i < snap.job_count; ++i)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(
                        snap.jobs[i].name ? snap.jobs[i].name : "<unnamed>");

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
            DiagRecorder& rec = state.diag_recorder;

            ImGui::SetNextItemWidth(140.0f);
            ImGui::InputInt("Frames per bucket", &rec.bucket_frames);
            if (rec.bucket_frames < 0)
                rec.bucket_frames = 0;

            if (!rec.recording) {
                if (ImGui::Button("Record")) {
                    rec.recording = true;
                    rec.frames.clear();
                    rec.last_status.clear();
                }
            } else {
                ImGui::TextUnformatted("Recording");
                ImGui::SameLine();
                ImGui::Text("(%zu frames)", rec.frames.size());

                if (ImGui::Button("Stop & Export")) {
                    rec.recording = false;
                    export_benchmark_recording(state);
                }

                ImGui::SameLine();

                if (ImGui::Button("Cancel")) {
                    rec.recording = false;
                    rec.frames.clear();
                    rec.last_status = "Cancelled.";
                }
            }

            if (!rec.last_status.empty())
                ImGui::TextUnformatted(rec.last_status.c_str());
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

    void render_imgui(
        ToolhostState& state,
        const wz::engine::FrameContext& fctx)
    {
        wz::gpu::Device& device =
            state.app.ctx.device;

        begin_imgui_frame(state, fctx);

        draw_benchmark_panel(state, fctx);
        draw_console_panel(state);

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
        if (state.scripts != nullptr)
            draw_script_panels(state);
#endif

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
            wz::gpu::dx12::internal::get_command_list(device);

        assert(cmd != nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            wz::gpu::dx12::internal::get_current_rtv(device);

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

        ImGuiIO& io = ImGui::GetIO();

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    void toolhost_update(
        wz::engine::Context& ctx,
        wz::engine::FrameContext& fctx,
        void* user_data)
    {
        auto* state =
            static_cast<ToolhostState*>(user_data);

        assert(state != nullptr);

        wz::app::update(
            ctx,
            fctx,
            state->app);

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
        if (state->scripts != nullptr)
            drain_script_panels(*state);
#endif

        if (!ctx.running)
            return;

        wz::gpu::begin_frame(state->app.ctx.device);
        wz::gpu::clear(
            state->app.ctx.device,
            0.0f,
            0.15f,
            0.35f,
            1.0f);

        wz::app::render_contents(
            state->app,
            fctx);

        render_imgui(
            *state,
            fctx);





        wz::gpu::end_frame(state->app.ctx.device);
        wz::gpu::present(state->app.ctx.device);
    }
}

int main()
{
    ToolhostState state{};
    g_toolhost_state = &state;

    if (!wz::app::init(state.app))
    {
        g_toolhost_state = nullptr;
        return 1;
    }

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        [](const wz::logging::LogRecordView& record, void* user)
        {
            auto* state = static_cast<ToolhostState*>(user);

            if (!state)
                return;

            state->console.push(record);
        },
        &state);

    if (!init_imgui_for_app(state))
    {
        shutdown_imgui(state);
        wz::app::shutdown(state.app);
        g_toolhost_state = nullptr;
        return 1;
    }

#ifdef WOZZITS_ENABLE_V8_SCRIPTING
    if (!wz::script::init_v8_platform())
    {
        std::cerr << "failed to initialize V8 platform\n";
        shutdown_imgui(state);
        wz::app::shutdown(state.app);
        g_toolhost_state = nullptr;
        return 1;
    }

    state.scripts = wz::script::create_v8_script_host();

    if (state.scripts == nullptr || !wz::script::initialize(state.scripts))
    {
        std::cerr << "failed to initialize script host\n";
        wz::script::destroy_v8_script_host(state.scripts);
        wz::script::shutdown_v8_platform();
        shutdown_imgui(state);
        wz::app::shutdown(state.app);
        g_toolhost_state = nullptr;
        return 1;
    }

    {
        wz::script::run_source(state.scripts, "startup.js", k_startup_script);
        drain_script_panels(state);
    }
#endif

    wz::engine::run(
        toolhost_update,
        &state);


#ifdef WOZZITS_ENABLE_V8_SCRIPTING
    wz::script::destroy_v8_script_host(state.scripts);
    state.scripts = nullptr;
    wz::script::shutdown_v8_platform();
#endif

    shutdown_imgui(state);

    wz::logging::set_log_sink(
        state.app.ctx.logger,
        nullptr,
        nullptr);

    wz::logging::wait_until_idle(state.app.ctx.logger);

    wz::app::shutdown(state.app);

    g_toolhost_state = nullptr;

    return 0;
}