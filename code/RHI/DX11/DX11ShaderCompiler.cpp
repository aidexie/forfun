#include "../ShaderCompiler.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

namespace RHI {

// ============================================
// Default Include Handler
// ============================================

CDefaultShaderIncludeHandler::CDefaultShaderIncludeHandler(const std::string& baseDir)
    : m_baseDir(baseDir)
{
    // Ensure trailing slash
    if (!m_baseDir.empty() && m_baseDir.back() != '/' && m_baseDir.back() != '\\') {
        m_baseDir += '/';
    }
}

bool CDefaultShaderIncludeHandler::Open(const char* filename, std::vector<char>& outData)
{
    std::string fullPath = m_baseDir + filename;
    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    outData.resize(size);
    file.read(outData.data(), size);
    return true;
}

// ============================================
// D3D Include Handler Wrapper
// ============================================

class CD3DIncludeWrapper : public ID3DInclude {
public:
    explicit CD3DIncludeWrapper(IShaderIncludeHandler* handler) : m_handler(handler) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName,
                          LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override
    {
        if (!m_handler) return E_FAIL;

        std::vector<char> data;
        if (!m_handler->Open(pFileName, data)) {
            return E_FAIL;
        }

        // Allocate and copy data
        char* buffer = new char[data.size()];
        memcpy(buffer, data.data(), data.size());

        *ppData = buffer;
        *pBytes = static_cast<UINT>(data.size());
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID pData) override
    {
        delete[] static_cast<const char*>(pData);
        return S_OK;
    }

private:
    IShaderIncludeHandler* m_handler;
};

// ============================================
// Shader Compilation
// ============================================

SCompiledShader CompileShaderFromSource(
    const std::string& source,
    const char* entryPoint,
    const char* target,
    IShaderIncludeHandler* includeHandler,
    bool debug)
{
    SCompiledShader result;

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    if (debug) {
        compileFlags |= D3DCOMPILE_DEBUG;
    }

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    CD3DIncludeWrapper includeWrapper(includeHandler);
    ID3DInclude* pInclude = includeHandler ? &includeWrapper : nullptr;

    HRESULT hr = D3DCompile(
        source.c_str(),
        source.size(),
        nullptr,
        nullptr,
        pInclude,
        entryPoint,
        target,
        compileFlags,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            result.errorMessage = static_cast<const char*>(errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        if (shaderBlob) {
            shaderBlob->Release();
        }
        return result;
    }

    // Copy bytecode
    result.bytecode.resize(shaderBlob->GetBufferSize());
    memcpy(result.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
    result.success = true;

    shaderBlob->Release();
    if (errorBlob) {
        errorBlob->Release();
    }

    return result;
}

SCompiledShader CompileShaderFromFile(
    const std::string& filepath,
    const char* entryPoint,
    const char* target,
    IShaderIncludeHandler* includeHandler,
    bool debug)
{
    std::ifstream file(filepath);
    if (!file.is_open()) {
        SCompiledShader result;
        result.errorMessage = "Failed to open shader file: " + filepath;
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return CompileShaderFromSource(buffer.str(), entryPoint, target, includeHandler, debug);
}

} // namespace RHI
