# DX11 Rotating 3D Cube (CMake + WinMain)

Upgrades the minimal triangle into a rotating **3D cube** using Direct3D 11, with a depth buffer and an MVP constant buffer.

## Build (CLI)
```bat
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Debug
.\Debug\DX11RotatingCube.exe
```

## Key Points
- Left-handed matrices (`XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`).
- Upload **transpose(W*V*P)** and use `mul(float4(pos,1), mvp)` in VS.
- Recreates RTV/DSV on window resize.