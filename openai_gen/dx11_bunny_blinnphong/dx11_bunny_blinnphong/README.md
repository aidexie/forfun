
# DX11 Bunny — Blinn‑Phong + Texture + Normal Map（打包工程）

## 资源放置
- `assets/bunny.obj`
- `assets/bunny_albedo.png`（sRGB）
- `assets/bunny_normal.png`（线性；若需要翻转Y，在像素着色器内取消注释 `nTS.y = -nTS.y;`）

## 构建
```
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Debug
.\Debug\DX11_Bunny_BlinnPhong.exe
```

## 控制
- 右键：自由视角
- WASD：移动
- R：重置相机（-X 看向原点）
