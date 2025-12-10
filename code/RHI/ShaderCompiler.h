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

// Shader compiler functions
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

} // namespace RHI
