# Third party libraries

Please keep categories (`##` level) listed alphabetically and matching their
respective folder names. Use two empty lines to separate categories for
readability.


## imgui

- Upstream: https://github.com/ocornut/imgui
- Version: 1.90
- License: MIT

Files used from upstream source:

- imported `*.h`, `*.cpp` and `LICENSE` from root and backends directory from release
- added includes precompiled headers
- modified `imgui_impl_opengl3.cpp` to support RK3588 (search `#ifdef __RK3588__`)


## imguizmo

- Upstream: https://github.com/CedricGuillemet/ImGuizmo
- Version: 1.83
- License: MIT

Files used from upstream source:

- imported `*.h`, `*.cpp` and `LICENSE` from release


## implot

- Upstream: https://github.com/epezent/implot
- Version: 0.16
- License: MIT

Files used from upstream source:

- imported `*.h`, `*.cpp` and `LICENSE` from release
- modified include to match project structure
