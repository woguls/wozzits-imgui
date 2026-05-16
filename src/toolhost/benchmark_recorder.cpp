// src/toolhost/benchmark_recorder.cpp

#include <wozzits/toolhost/benchmark_recorder.h>

#include <engine/assets/engine_asset_library.h>
#include <engine/assets/data_table/data_table.h>

#include <imgui.h>

#include <cassert>
#include <cstdio>
#include <ctime>
#include <string>

namespace wz::toolhost
{
    // ── BenchmarkRecorder ─────────────────────────────────────────────────────

    void BenchmarkRecorder::configure(std::vector<std::string> columns)
    {
        recording_ = false;
        rows_.clear();
        last_status.clear();
        columns_ = std::move(columns);
    }

    void BenchmarkRecorder::start()
    {
        recording_ = true;
        rows_.clear();
        last_status.clear();
    }

    void BenchmarkRecorder::stop()
    {
        recording_ = false;
    }

    void BenchmarkRecorder::cancel()
    {
        recording_ = false;
        rows_.clear();
        last_status = "Cancelled.";
    }

    void BenchmarkRecorder::push_row(std::vector<std::string> cells)
    {
        if (!recording_)
            return;

        if (!columns_.empty() && cells.size() != columns_.size())
            return;

        rows_.push_back(std::move(cells));
    }

    BenchmarkRecorder::Table BenchmarkRecorder::make_table() const
    {
        return Table{ columns_, rows_ };
    }


    // ── draw_recorder_controls ────────────────────────────────────────────────

    RecorderAction draw_recorder_controls(BenchmarkRecorder& recorder)
    {
        RecorderAction action = RecorderAction::None;

        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputInt("Frames per bucket", &recorder.bucket_frames);
        if (recorder.bucket_frames < 0)
            recorder.bucket_frames = 0;

        if (!recorder.recording())
        {
            if (ImGui::Button("Record"))
                recorder.start();
        }
        else
        {
            ImGui::TextUnformatted("Recording");
            ImGui::SameLine();
            ImGui::Text("(%zu frames)", recorder.frame_count());

            if (ImGui::Button("Stop & Export"))
            {
                recorder.stop();
                action = RecorderAction::ExportRequested;
            }

            ImGui::SameLine();

            if (ImGui::Button("Cancel"))
            {
                recorder.cancel();
                action = RecorderAction::Cancelled;
            }
        }

        if (!recorder.last_status.empty())
            ImGui::TextUnformatted(recorder.last_status.c_str());

        return action;
    }


    // ── export_recording ──────────────────────────────────────────────────────

    void export_recording(
        BenchmarkRecorder&           recorder,
        wz::gpu::Device&             device,
        wz::Logger&                  logger,
        const BenchmarkExportConfig& config)
    {
        using wz::engine::assets::DataTableData;
        using wz::engine::assets::DataTableColumn;
        using wz::engine::assets::DataTableRow;

        const BenchmarkRecorder::Table snapshot = recorder.make_table();

        if (snapshot.rows.empty())
        {
            recorder.last_status = "No frames recorded.";
            return;
        }

        // ── Build DataTableData ───────────────────────────────────────────────
        DataTableData table;
        table.columns.reserve(snapshot.columns.size());
        for (const std::string& col : snapshot.columns)
            table.columns.push_back(DataTableColumn{ .name = col });

        table.rows.reserve(snapshot.rows.size());
        for (const std::vector<std::string>& cells : snapshot.rows)
            table.rows.push_back(DataTableRow{ .cells = cells });

        // ── Build asset pipeline ──────────────────────────────────────────────
        wz::engine::assets::EngineAssetLibrary assets{ device, logger, "." };

        const uint32_t last_frame =
            static_cast<uint32_t>(snapshot.rows.size()) > 0
                ? static_cast<uint32_t>(snapshot.rows.size() - 1)
                : 0u;

        const auto source = assets.data_tables().create_inline_table({
            .name  = config.name_prefix + "/raw",
            .table = std::move(table),
        });

        if (!source.valid())
        {
            recorder.last_status = "Failed to create source table.";
            return;
        }

        const auto summary = assets.diagnostic_timeframe_summaries().create_timeframe_summary({
            .name              = config.name_prefix + "/summary",
            .source            = source,
            .frame_column      = config.frame_column,
            .metric_columns    = config.metric_columns,
            .frame_start       = 0,
            .frame_end         = last_frame,
            .frames_per_bucket = static_cast<uint32_t>(
                recorder.bucket_frames > 0 ? recorder.bucket_frames : 0),
        });

        if (!summary.valid())
        {
            recorder.last_status = "Failed to create summary.";
            return;
        }

        const auto view = assets.diagnostic_timeframe_summaries().create_data_table_view(
            config.name_prefix + "/view", summary);

        if (!view.valid())
        {
            recorder.last_status = "Failed to create table view.";
            return;
        }

        const auto csv_asset = assets.csv_export().create_csv_export({
            .name   = config.name_prefix + "/csv",
            .source = view,
        });

        if (!csv_asset.valid())
        {
            recorder.last_status = "Failed to create CSV export.";
            return;
        }

        if (!assets.commit())
        {
            recorder.last_status = "Asset commit failed.";
            return;
        }

        const auto report = assets.resolve_all();
        if (!report.ok())
        {
            recorder.last_status = "Resolve failed ("
                + std::to_string(report.failures.size()) + " errors).";
            return;
        }

        const auto csv_handle = assets.csv_export().get_export(csv_asset);
        if (!csv_handle.valid())
        {
            recorder.last_status = "CSV export handle invalid after resolve.";
            return;
        }

        // ── Write timestamped file ────────────────────────────────────────────
        std::time_t now = std::time(nullptr);
        std::tm     tm_now{};
        ::localtime_s(&tm_now, &now);

        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_now);

        const std::string path = std::string("benchmark_") + timestamp + ".csv";

        const wz::fs::FileError err =
            assets.csv_export().write_export_to_file(csv_handle, path);

        if (err != wz::fs::FileError::None)
            recorder.last_status = "Write failed: " + path;
        else
            recorder.last_status = "Exported: " + path;
    }

} // namespace wz::toolhost
