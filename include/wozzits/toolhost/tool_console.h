#pragma once

// wozzits/toolhost/tool_console.h

#include <logging/logger.h>
#include <logging/logging.h>

#include <mutex>
#include <string>
#include <vector>

namespace wz::toolhost
{
    struct ConsoleLine
    {
        wz::LogLevel level       = wz::LogLevel::Info;
        uint64_t     sequence    = 0;
        uint64_t     event_ticks = 0;
        std::string  text;
    };

    class ToolConsole
    {
    public:
        void push_string(std::string text, wz::LogLevel level = wz::LogLevel::Info);
        void push(const wz::logging::LogRecordView& record);
        void clear();

        bool auto_scroll      = true;
        bool scroll_to_bottom = false;

        std::mutex               mutex;
        std::vector<ConsoleLine> lines;
    };

    void draw_console_panel(ToolConsole& console);

} // namespace wz::toolhost
