#include "Panels.h"
#include "imgui.h"
#include <string>
#include <iostream>
#include <fstream>
#include <windows.h>
#include <commdlg.h>
#include <nlohmann/json.hpp>
#include <ktx.h>
#include "Engine/Rendering/Skybox.h"
#include "Engine/Rendering/IBLGenerator.h"
#include "Core/KTXExporter.h"

using json = nlohmann::json;

// File-scope state
static bool s_showWindow = false;
static std::string s_hdrFilePath;
static std::string s_outputDir;
static std::string s_assetName;
static bool s_isExporting = false;
static float s_exportProgress = 0.0f;
static std::string s_exportStatus;

// Helper: Open file dialog
static std::string OpenFileDialog(const char* filter) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrInitialDir = "E:\\forfun\\assets";

    if (GetOpenFileNameA(&ofn)) {
        return std::string(filename);
    }
    return "";
}


// Helper: Extract filename without extension
static std::string GetFileNameWithoutExt(const std::string& path) {
    size_t lastSlash = path.find_last_of("/\\");
    size_t lastDot = path.find_last_of('.');
    std::string filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    if (lastDot != std::string::npos && lastDot > lastSlash) {
        filename = filename.substr(0, lastDot - (lastSlash + 1));
    }
    return filename;
}

// Helper: Get relative path
static std::string GetRelativePath(const std::string& from, const std::string& to) {
    // Simple implementation: assume both are absolute paths
    // For now, just return the filename if they're in the same directory
    size_t lastSlashFrom = from.find_last_of("/\\");
    size_t lastSlashTo = to.find_last_of("/\\");

    std::string fromDir = from.substr(0, lastSlashFrom);
    std::string toDir = to.substr(0, lastSlashTo);

    if (fromDir == toDir) {
        return to.substr(lastSlashTo + 1);
    }
    return to;  // Return absolute path if not in same dir
}

void Panels::ShowHDRExportWindow(bool show) {
    s_showWindow = show;
}

void Panels::DrawHDRExportWindow() {
    if (!s_showWindow) return;

    // Set window size on first appearance
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);

    // Begin window with close button
    if (!ImGui::Begin("HDR Export", &s_showWindow, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Export HDR to ForFun Asset (.ffasset)");
    ImGui::Separator();

    // Input: HDR file path
    ImGui::Text("Source HDR File:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-100.0f);
    char hdrBuf[512];
    snprintf(hdrBuf, sizeof(hdrBuf), "%s", s_hdrFilePath.c_str());
    ImGui::InputText("##hdr_path", hdrBuf, sizeof(hdrBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse...##hdr")) {
        std::string path = OpenFileDialog("HDR Files (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            s_hdrFilePath = path;
            // Auto-generate asset name from filename
            s_assetName = GetFileNameWithoutExt(path);
            // Set default output directory to same as HDR file
            size_t lastSlash = path.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                s_outputDir = path.substr(0, lastSlash);
            }
        }
    }

    ImGui::Spacing();

    // Input: Output directory (manual input)
    ImGui::Text("Output Directory:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-1.0f);
    char outBuf[512];
    snprintf(outBuf, sizeof(outBuf), "%s", s_outputDir.c_str());
    if (ImGui::InputText("##out_dir", outBuf, sizeof(outBuf))) {
        s_outputDir = outBuf;
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();

    // Input: Asset name
    ImGui::Text("Asset Name:");
    ImGui::SameLine();
    ImGui::PushItemWidth(-1.0f);
    char nameBuf[256];
    snprintf(nameBuf, sizeof(nameBuf), "%s", s_assetName.c_str());
    if (ImGui::InputText("##asset_name", nameBuf, sizeof(nameBuf))) {
        s_assetName = nameBuf;
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Export button
    ImGui::BeginDisabled(s_isExporting || s_hdrFilePath.empty() || s_outputDir.empty() || s_assetName.empty());
    if (ImGui::Button("Export", ImVec2(120, 0))) {
        s_isExporting = true;
        s_exportProgress = 0.0f;
        s_exportStatus = "Starting export...";
        std::cout << "Exporting HDR: " << s_hdrFilePath << std::endl;

        bool success = true;

        // 1. Generate environment cubemap from HDR
        s_exportProgress = 0.1f;
        s_exportStatus = "Generating environment cubemap...";

        CSkybox tempSkybox;
        if (!tempSkybox.Initialize(s_hdrFilePath, 512)) {
            s_exportStatus = "ERROR: Failed to load HDR file";
            s_isExporting = false;
            success = false;
        }

        // 2. Generate IBL maps
        CIBLGenerator tempIBL;
        ID3D11ShaderResourceView* irradianceSRV = nullptr;
        ID3D11ShaderResourceView* prefilterSRV = nullptr;

        if (success) {
            s_exportProgress = 0.3f;
            s_exportStatus = "Initializing IBL generator...";

            if (!tempIBL.Initialize()) {
                s_exportStatus = "ERROR: Failed to initialize IBL generator";
                s_isExporting = false;
                success = false;
            }
        }

        if (success) {
            s_exportProgress = 0.4f;
            s_exportStatus = "Generating irradiance map...";

            irradianceSRV = tempIBL.GenerateIrradianceMap(tempSkybox.GetEnvironmentMap(), 32);
            if (!irradianceSRV) {
                s_exportStatus = "ERROR: Failed to generate irradiance map";
                success = false;
            }
        }

        if (success) {
            s_exportProgress = 0.5f;
            s_exportStatus = "Generating pre-filtered map (this may take a while)...";

            prefilterSRV = tempIBL.GeneratePreFilteredMap(tempSkybox.GetEnvironmentMap(), 128, 7);
            if (!prefilterSRV) {
                s_exportStatus = "ERROR: Failed to generate pre-filtered map";
                success = false;
            }
        }

        // 3. Export to KTX2 files
        std::string envPath, irrPath, prefilterPath;
        if (success) {
            s_exportProgress = 0.7f;
            s_exportStatus = "Exporting environment cubemap...";

            envPath = s_outputDir + "\\" + s_assetName + "_env.ktx2";
            if (!CKTXExporter::ExportCubemapToKTX2(tempSkybox.GetEnvironmentTexture(), envPath, 0)) {
                s_exportStatus = "ERROR: Failed to export environment cubemap";
                success = false;
            }
        }

        if (success) {
            s_exportProgress = 0.8f;
            s_exportStatus = "Exporting irradiance map...";

            irrPath = s_outputDir + "\\" + s_assetName + "_irr.ktx2";
            if (!CKTXExporter::ExportCubemapToKTX2(tempIBL.GetIrradianceTexture(), irrPath, 1)) {
                s_exportStatus = "ERROR: Failed to export irradiance map";
                success = false;
            }
        }

        if (success) {
            s_exportProgress = 0.9f;
            s_exportStatus = "Exporting pre-filtered map...";

            prefilterPath = s_outputDir + "\\" + s_assetName + "_prefilter.ktx2";
            if (!CKTXExporter::ExportCubemapToKTX2(tempIBL.GetPreFilteredTexture(), prefilterPath, 7)) {
                s_exportStatus = "ERROR: Failed to export pre-filtered map";
                success = false;
            }
        }

        // 4. Generate .ffasset JSON file
        if (success) {
            s_exportProgress = 0.95f;
            s_exportStatus = "Generating .ffasset file...";

            std::string ffassetPath = s_outputDir + "\\" + s_assetName + ".ffasset";

            json ffasset;
            ffasset["type"] = "skybox";
            ffasset["version"] = "1.0";
            ffasset["source"] = GetRelativePath(ffassetPath, s_hdrFilePath);
            ffasset["data"] = {
                {"env", s_assetName + "_env.ktx2"},
                {"irr", s_assetName + "_irr.ktx2"},
                {"prefilter", s_assetName + "_prefilter.ktx2"}
            };

            std::ofstream outFile(ffassetPath);
            if (outFile.is_open()) {
                outFile << ffasset.dump(2);  // Pretty print with 2-space indent
                outFile.close();
                std::cout << "Generated .ffasset: " << ffassetPath << std::endl;
            } else {
                s_exportStatus = "ERROR: Failed to write .ffasset file";
                success = false;
            }
        }

        // Cleanup
        tempIBL.Shutdown();
        tempSkybox.Shutdown();

        // Finish
        s_exportProgress = 1.0f;
        if (success) {
            s_exportStatus = "Export completed successfully!";
        }
        s_isExporting = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        s_showWindow = false;
    }

    // Progress display
    if (s_isExporting) {
        ImGui::Spacing();
        ImGui::ProgressBar(s_exportProgress, ImVec2(-1.0f, 0.0f));
        ImGui::Text("%s", s_exportStatus.c_str());
    }

    // Status message
    if (!s_exportStatus.empty() && !s_isExporting) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s", s_exportStatus.c_str());
    }

    ImGui::End();
}
