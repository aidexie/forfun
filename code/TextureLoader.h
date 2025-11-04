
#pragma once
#include <wrl/client.h>
#include <d3d11.h>
#include <string>

bool LoadTextureWIC(ID3D11Device* device, const std::wstring& path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV, bool srgb=false);
