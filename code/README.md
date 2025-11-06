# ForFunEditor (DX11 + ImGui Docking, integrated editor UI)
Two modules only:
- `imgui_docking` (static lib, Win32 + DX11 backends)
- `forfun` (your app; editor UI compiled in; Viewport renders to offscreen RT)
Third-party layout:
third_party/imgui (docking branch with backends)
Build:
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun
./build/forfun.exe
