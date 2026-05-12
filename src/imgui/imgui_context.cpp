// src/imgui/imgui_context.cpp

#include <wozzits/imgui/imgui_context.h>

#include <imgui.h>

namespace wz::imgui
{
    bool create_context()
    {
        if (ImGui::GetCurrentContext() != nullptr)
            return true;

        ImGui::CreateContext();

        return ImGui::GetCurrentContext() != nullptr;
    }

    void destroy_context()
    {
        if (ImGui::GetCurrentContext() != nullptr)
            ImGui::DestroyContext();
    }

    bool has_context()
    {
        return ImGui::GetCurrentContext() != nullptr;
    }

    const char* version()
    {
        return IMGUI_VERSION;
    }
}