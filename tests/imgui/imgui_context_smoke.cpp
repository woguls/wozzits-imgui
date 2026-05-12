#include <wozzits/imgui/imgui_context.h>

#include <cassert>
#include <cstring>
#include <iostream>

int main()
{
    assert(!wz::imgui::has_context());

    const bool created = wz::imgui::create_context();
    assert(created);
    assert(wz::imgui::has_context());

    const char* version = wz::imgui::version();
    assert(version != nullptr);
    assert(std::strlen(version) > 0);

    std::cout << "Dear ImGui version: " << version << "\n";

    wz::imgui::destroy_context();
    assert(!wz::imgui::has_context());

    return 0;
}