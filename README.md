# wozzits-imgui

`wozzits-imgui` provides the Dear ImGui integration layer for Wozzits development tools.

It owns ImGui context setup, platform/backend integration, and frame rendering helpers.
It does not own Wozzits-specific tools such as asset inspectors, scene graph panels,
benchmark dashboards, or script bindings. Those belong in higher-level devtool modules.

Initial backend target:

- Win32 platform backend
- DirectX 12 renderer backend

Future users:

- `wozzits-devtools`
- Wozzits debug applications
- optional V8-driven tool panels