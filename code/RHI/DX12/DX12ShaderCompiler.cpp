#include "../ShaderCompiler.h"
#include "../../Core/FFLog.h"
#include "../../Core/PathManager.h"

#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <fstream>
#include <sstream>
#include <codecvt>
#include <locale>

using Microsoft::WRL::ComPtr;

// ============================================
// DXCompiler Runtime Loading
// ============================================

namespace {

// DXCompiler DLL handle
HMODULE g_dxcModule = nullptr;
bool g_dxcInitialized = false;
bool g_dxcAvailable = false;

// DXCompiler function pointers
typedef HRESULT(WINAPI* DxcCreateInstanceProc)(REFCLSID rclsid, REFIID riid, LPVOID* ppv);
DxcCreateInstanceProc g_DxcCreateInstance = nullptr;

// Convert UTF-8 to wide string
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], size);
    return result;
}

// Convert wide string to UTF-8
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size, nullptr, nullptr);
    return result;
}

// Initialize DXCompiler
bool InitializeDXCompiler() {
    if (g_dxcInitialized) {
        return g_dxcAvailable;
    }
    g_dxcInitialized = true;

    // Try to load dxcompiler.dll from various locations
    const char* searchPaths[] = {
        "dxcompiler.dll",                           // Current directory / system
        "bin/dxcompiler.dll",                       // bin folder
        "../bin/dxcompiler.dll",                    // Parent bin folder
    };

    for (const char* path : searchPaths) {
        g_dxcModule = LoadLibraryA(path);
        if (g_dxcModule) {
            CFFLog::Info("[DXCompiler] Loaded from: %s", path);
            break;
        }
    }

    if (!g_dxcModule) {
        CFFLog::Warning("[DXCompiler] dxcompiler.dll not found - DXR shader compilation unavailable");
        CFFLog::Warning("[DXCompiler] Download from Windows SDK or https://github.com/microsoft/DirectXShaderCompiler/releases");
        return false;
    }

    // Get DxcCreateInstance function
    g_DxcCreateInstance = (DxcCreateInstanceProc)GetProcAddress(g_dxcModule, "DxcCreateInstance");
    if (!g_DxcCreateInstance) {
        CFFLog::Error("[DXCompiler] Failed to get DxcCreateInstance");
        FreeLibrary(g_dxcModule);
        g_dxcModule = nullptr;
        return false;
    }

    g_dxcAvailable = true;
    CFFLog::Info("[DXCompiler] Initialized successfully");
    return true;
}

// DXC Include Handler
class CDXCIncludeHandler : public IDxcIncludeHandler {
public:
    CDXCIncludeHandler(RHI::IShaderIncludeHandler* handler, IDxcUtils* utils)
        : m_handler(handler), m_utils(utils), m_refCount(1) {}

    HRESULT STDMETHODCALLTYPE LoadSource(
        _In_z_ LPCWSTR pFilename,
        _COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource) override
    {
        if (!m_handler || !ppIncludeSource) {
            return E_FAIL;
        }

        *ppIncludeSource = nullptr;

        std::string filename = WideToUtf8(pFilename);
        std::vector<char> data;

        if (!m_handler->Open(filename.c_str(), data)) {
            return E_FAIL;
        }

        ComPtr<IDxcBlobEncoding> blob;
        HRESULT hr = m_utils->CreateBlob(data.data(), (UINT32)data.size(), CP_UTF8, &blob);
        if (FAILED(hr)) {
            return hr;
        }

        *ppIncludeSource = blob.Detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --m_refCount;
        if (ref == 0) delete this;
        return ref;
    }

private:
    RHI::IShaderIncludeHandler* m_handler;
    IDxcUtils* m_utils;
    std::atomic<ULONG> m_refCount;
};

} // anonymous namespace

// ============================================
// Public API Implementation
// ============================================

namespace RHI {

bool IsDXCompilerAvailable() {
    return InitializeDXCompiler();
}

SCompiledShader CompileDXRLibraryFromSource(
    const std::string& source,
    const std::string& sourceName,
    IShaderIncludeHandler* includeHandler,
    bool debug)
{
    SCompiledShader result;

    if (!InitializeDXCompiler()) {
        result.errorMessage = "DXCompiler not available";
        return result;
    }

    // Create DXC utils
    ComPtr<IDxcUtils> utils;
    HRESULT hr = g_DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) {
        result.errorMessage = "Failed to create IDxcUtils";
        return result;
    }

    // Create DXC compiler
    ComPtr<IDxcCompiler3> compiler;
    hr = g_DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr)) {
        result.errorMessage = "Failed to create IDxcCompiler3";
        return result;
    }

    // Create source blob
    ComPtr<IDxcBlobEncoding> sourceBlob;
    hr = utils->CreateBlob(source.c_str(), (UINT32)source.size(), CP_UTF8, &sourceBlob);
    if (FAILED(hr)) {
        result.errorMessage = "Failed to create source blob";
        return result;
    }

    // Build arguments
    std::vector<LPCWSTR> arguments;

    // Target profile: lib_6_3 for ray tracing
    arguments.push_back(L"-T");
    arguments.push_back(L"lib_6_3");

    // Entry point not needed for library compilation

    // Include directories - use shader directory
    std::wstring shaderDir = Utf8ToWide(FFPath::GetSourceDir() + "/Shader");
    arguments.push_back(L"-I");
    arguments.push_back(shaderDir.c_str());

    // DXR include path
    std::wstring dxrDir = Utf8ToWide(FFPath::GetSourceDir() + "/Shader/DXR");
    arguments.push_back(L"-I");
    arguments.push_back(dxrDir.c_str());

    // Enable 16-bit types (optional, for performance)
    arguments.push_back(L"-enable-16bit-types");

    // Debug flags
    if (debug) {
        arguments.push_back(L"-Zi");           // Debug info
        arguments.push_back(L"-Qembed_debug"); // Embed debug info
        arguments.push_back(L"-Od");           // Disable optimizations
    } else {
        arguments.push_back(L"-O3");           // Full optimizations
    }

    // HLSL version 2021 (latest features)
    arguments.push_back(L"-HV");
    arguments.push_back(L"2021");

    // Create include handler
    CDXCIncludeHandler* dxcIncludeHandler = nullptr;
    if (includeHandler) {
        dxcIncludeHandler = new CDXCIncludeHandler(includeHandler, utils.Get());
    }

    // Prepare source
    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = CP_UTF8;

    std::wstring wSourceName = Utf8ToWide(sourceName);

    // Compile
    ComPtr<IDxcResult> compileResult;
    hr = compiler->Compile(
        &sourceBuffer,
        arguments.data(),
        (UINT32)arguments.size(),
        dxcIncludeHandler,
        IID_PPV_ARGS(&compileResult)
    );

    // Clean up include handler
    if (dxcIncludeHandler) {
        dxcIncludeHandler->Release();
    }

    // Check compilation status
    HRESULT status;
    compileResult->GetStatus(&status);

    // Get errors/warnings
    ComPtr<IDxcBlobUtf8> errors;
    compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        result.errorMessage = errors->GetStringPointer();
    }

    if (FAILED(hr) || FAILED(status)) {
        CFFLog::Error("[DXCompiler] Compilation failed: %s", result.errorMessage.c_str());
        return result;
    }

    // Get compiled bytecode
    ComPtr<IDxcBlob> shaderBlob;
    compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
    if (!shaderBlob) {
        result.errorMessage = "No output blob from compilation";
        return result;
    }

    // Copy bytecode
    result.bytecode.resize(shaderBlob->GetBufferSize());
    memcpy(result.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
    result.success = true;

    CFFLog::Info("[DXCompiler] Successfully compiled DXR library: %s (%zu bytes)",
                 sourceName.c_str(), result.bytecode.size());

    return result;
}

SCompiledShader CompileDXRLibraryFromFile(
    const std::string& filepath,
    IShaderIncludeHandler* includeHandler,
    bool debug)
{
    SCompiledShader result;

    // Read file
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        result.errorMessage = "Failed to open file: " + filepath;
        CFFLog::Error("[DXCompiler] %s", result.errorMessage.c_str());
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Extract filename for error reporting
    size_t lastSlash = filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

    return CompileDXRLibraryFromSource(source, filename, includeHandler, debug);
}

} // namespace RHI
