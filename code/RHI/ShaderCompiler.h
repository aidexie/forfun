#pragma once
#include "RHICommon.h"
#include <string>
#include <vector>
#include <memory>

// ============================================
// RHI Shader Compiler Interface
// ============================================
// Abstracts shader compilation from D3DCompiler
// Implementation in RHI/DX11/DX11ShaderCompiler.cpp
// DXC implementation in RHI/DX12/DX12ShaderCompiler.cpp

namespace RHI {

// Shader include handler interface
class IShaderIncludeHandler {
public:
    virtual ~IShaderIncludeHandler() = default;
    virtual bool Open(const char* filename, std::vector<char>& outData) = 0;
};

// Default include handler that searches relative to shader directory
class CDefaultShaderIncludeHandler : public IShaderIncludeHandler {
public:
    explicit CDefaultShaderIncludeHandler(const std::string& baseDir);
    bool Open(const char* filename, std::vector<char>& outData) override;
private:
    std::string m_baseDir;
};

// Compiled shader bytecode
struct SCompiledShader {
    std::vector<uint8_t> bytecode;
    std::string errorMessage;
    bool success = false;
};

// Shader compiler functions (D3DCompiler - SM 5.0 and below)
SCompiledShader CompileShaderFromSource(
    const std::string& source,
    const char* entryPoint,
    const char* target,  // "vs_5_0", "ps_5_0", "cs_5_0", etc.
    IShaderIncludeHandler* includeHandler = nullptr,
    bool debug = false
);

SCompiledShader CompileShaderFromFile(
    const std::string& filepath,
    const char* entryPoint,
    const char* target,
    IShaderIncludeHandler* includeHandler = nullptr,
    bool debug = false
);

// ============================================
// DXCompiler (DXC) - SM 6.0+ and DXR
// ============================================

// Compile DXR shader library (DXIL format, contains multiple entry points)
// Target should be "lib_6_3" or higher for ray tracing
SCompiledShader CompileDXRLibraryFromFile(
    const std::string& filepath,
    IShaderIncludeHandler* includeHandler = nullptr,
    bool debug = false
);

SCompiledShader CompileDXRLibraryFromSource(
    const std::string& source,
    const std::string& sourceName,  // For error reporting
    IShaderIncludeHandler* includeHandler = nullptr,
    bool debug = false
);

// Check if DXCompiler is available
bool IsDXCompilerAvailable();

} // namespace RHI
