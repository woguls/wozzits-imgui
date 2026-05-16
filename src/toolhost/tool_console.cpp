// src/toolhost/tool_console.cpp

#include <wozzits/toolhost/tool_console.h>

#include <imgui.h>

namespace wz::toolhost
{
    namespace
    {
        const char* log_level_name(wz::LogLevel level)
        {
            switch (level)
            {
            case wz::LogLevel::Debug:    return "DEBUG";
            case wz::LogLevel::Info:     return "INFO";
            case wz::LogLevel::Warning:  return "WARN";
            case wz::LogLevel::Error:    return "ERROR";
            case wz::LogLevel::Critical: return "CRITICAL";
            }
            return "UNKNOWN";
        }
    }

    void ToolConsole::push_string(std::string text, wz::LogLevel level)
    {
        std::lock_guard lock(mutex);
        lines.push_back(ConsoleLine{
            .level = level,
            .text  = std::move(text),
        });
        scroll_to_bottom = true;
    }

    void ToolConsole::push(const wz::logging::LogRecordView& record)
    {
        if (record.text == nullptr)
            return;

        std::lock_guard lock(mutex);

        lines.push_back(ConsoleLine{
            .level       = record.level,
            .sequence    = record.sequence,
            .event_ticks = record.event_ticks,
            .text        = std::string(record.text, record.text + record.text_size),
        });

        constexpr std::size_t kMaxLines = 4000;

        if (lines.size() > kMaxLines)
        {
            lines.erase(
                lines.begin(),
                lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - kMaxLines));
        }

        scroll_to_bottom = true;
    }

    void ToolConsole::clear()
    {
        std::lock_guard lock(mutex);
        lines.clear();
        scroll_to_bottom = false;
    }

    void draw_console_panel(ToolConsole& console)
    {
        ImGui::SetNextWindowSize(ImVec2(760.0f, 360.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Wozzits Console");

        if (ImGui::Button("Clear"))
            console.clear();

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &console.auto_scroll);
        ImGui::Separator();

        if (ImGui::BeginChild(
            "console_scroll",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar))
        {
            std::lock_guard lock(console.mutex);

            for (const ConsoleLine& line : console.lines)
            {
                ImGui::Text("[%s]", log_level_name(line.level));
                ImGui::SameLine();
                ImGui::TextUnformatted(line.text.c_str());
            }

            if (console.auto_scroll && console.scroll_to_bottom)
            {
                ImGui::SetScrollHereY(1.0f);
                console.scroll_to_bottom = false;
            }
        }

        ImGui::EndChild();
        ImGui::End();
    }

} // namespace wz::toolhost
