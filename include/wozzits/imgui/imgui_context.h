#pragma once

namespace wz::imgui
{
    bool create_context();
    void destroy_context();

    bool has_context();

    const char* version();
}