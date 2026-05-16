#pragma once

// wozzits/toolhost/benchmark_recorder.h

#include <gpu/gpu.h>
#include <logging/logger.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wz::toolhost
{
    // ── BenchmarkRecorder ─────────────────────────────────────────────────────
    //
    // Records per-frame rows into an in-memory table.  The scene-specific
    // toolhost configures the column names and pushes one row per frame while
    // recording is active.  Call make_table() to retrieve the accumulated data
    // for export.

    class BenchmarkRecorder
    {
    public:
        // Must be called before start().  Calling configure() while recording
        // implicitly cancels the current recording.
        void configure(std::vector<std::string> columns);

        void start();
        void stop();
        void cancel();

        bool        recording()   const noexcept { return recording_; }
        std::size_t frame_count() const noexcept { return rows_.size(); }

        // Push one row of serialized cell values.  Silently ignored if not
        // currently recording or if the cell count does not match columns_.
        void push_row(std::vector<std::string> cells);

        // Return the accumulated data as a DataTableData-compatible structure.
        // The returned value is a snapshot; the recorder retains its own copy.
        struct Table
        {
            std::vector<std::string>              columns;
            std::vector<std::vector<std::string>> rows;
        };

        Table make_table() const;

        int         bucket_frames = 60;
        std::string last_status;

    private:
        bool                                  recording_ = false;
        std::vector<std::string>              columns_;
        std::vector<std::vector<std::string>> rows_;
    };


    // ── BenchmarkExportConfig ─────────────────────────────────────────────────

    struct BenchmarkExportConfig
    {
        std::string              name_prefix    = "bench/recording";
        std::string              frame_column   = "frame";
        std::vector<std::string> metric_columns;
    };


    // ── draw_recorder_controls ────────────────────────────────────────────────
    //
    // Draws the Record / Stop & Export / Cancel UI inside the calling window.
    // Returns ExportRequested when the user clicks "Stop & Export".

    enum class RecorderAction { None, ExportRequested, Cancelled };

    RecorderAction draw_recorder_controls(BenchmarkRecorder& recorder);


    // ── export_recording ──────────────────────────────────────────────────────
    //
    // Exports the recorder's accumulated table through the diagnostic asset
    // pipeline (DataTable → DiagnosticTimeframeSummary → DataTable → CSVExport)
    // and writes the result to a timestamped file in the working directory.
    // Updates recorder.last_status on completion or failure.

    void export_recording(
        BenchmarkRecorder&          recorder,
        wz::gpu::Device&            device,
        wz::Logger&                 logger,
        const BenchmarkExportConfig& config);

} // namespace wz::toolhost
