#include "Panels.h"
#include "imgui.h"
#include "Core/MaterialAsset.h"
#include "Core/MaterialManager.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"  // FFPath namespace
#include "PropertyVisitor.h"
#include <string>
#include <windows.h>
#include <commdlg.h>

// Material Editor state (singleton-like static variables)
static bool s_MaterialEditorOpen = false;
static std::string s_EditingMaterialPath;
static CMaterialAsset* s_EditingMaterial = nullptr;

// Icon placeholders (same as InspectorPanel)
#define ICON_FA_FILE "~"
#define ICON_FA_FOLDER_OPEN "..."
#define ICON_FA_EDIT "E"

// Declare ImGui Property Visitor (duplicate from InspectorPanel for now)
// TODO: Move to shared header
class ImGuiPropertyVisitor : public CPropertyVisitor {
public:
    void VisitFloat(const char* name, float& value) override {
        ImGui::DragFloat(name, &value, 0.1f);
    }

    void VisitFloatSlider(const char* name, float& value, float min, float max) override {
        ImGui::SliderFloat(name, &value, min, max);
    }

    void VisitInt(const char* name, int& value) override {
        ImGui::DragInt(name, &value);
    }

    void VisitBool(const char* name, bool& value) override {
        ImGui::Checkbox(name, &value);
    }

    void VisitString(const char* name, std::string& value) override {
        char buf[260];
        snprintf(buf, sizeof(buf), "%s", value.c_str());
        if (ImGui::InputText(name, buf, sizeof(buf))) {
            value = buf;
        }
    }

    void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) override {
        float arr[3] = { value.x, value.y, value.z };
        if (ImGui::DragFloat3(name, arr, 0.1f)) {
            value = { arr[0], arr[1], arr[2] };
        }
    }

    void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) override {
        if (ImGui::BeginCombo(name, options[value])) {
            for (int i = 0; i < (int)options.size(); ++i) {
                bool selected = (i == value);
                if (ImGui::Selectable(options[i], selected)) {
                    value = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    void VisitFilePath(const char* name, std::string& value, const char* filter) override {
        ImGui::PushID(name);

        ImGui::Text("%s", name);
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_FA_FILE);
        ImGui::SameLine();

        float buttonWidth = 50.0f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushItemWidth(-buttonWidth);

        char displayBuf[260];
        snprintf(displayBuf, sizeof(displayBuf), "%s", value.empty() ? "(None)" : value.c_str());
        ImGui::InputText("##path", displayBuf, sizeof(displayBuf), ImGuiInputTextFlags_ReadOnly);

        ImGui::PopItemWidth();
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "##browse")) {
            char filename[MAX_PATH] = "";
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = filter ? filter : "All Files\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

            // Use FFPath for initial directory
            std::string assetsDir = FFPath::GetAssetsDir();
            ofn.lpstrInitialDir = assetsDir.c_str();

            if (GetOpenFileNameA(&ofn)) {
                // Normalize path to relative format
                value = FFPath::Normalize(filename);
            }
        }

        ImGui::PopID();
    }
};

void Panels::OpenMaterialEditor(const std::string& materialPath) {
    s_EditingMaterialPath = materialPath;
    s_EditingMaterial = CMaterialManager::Instance().Load(materialPath);
    s_MaterialEditorOpen = true;

    if (!s_EditingMaterial) {
        CFFLog::Error(("Failed to load material for editing: " + materialPath).c_str());
        s_MaterialEditorOpen = false;
    }
}

void Panels::DrawMaterialEditor() {
    if (!s_MaterialEditorOpen || !s_EditingMaterial) {
        return;
    }

    // Create Material Editor window
    ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Material Editor", &s_MaterialEditorOpen, ImGuiWindowFlags_None)) {
        // Title
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Editing: %s", s_EditingMaterialPath.c_str());
        ImGui::Separator();

        // Use reflection to display all properties
        ImGuiPropertyVisitor visitor;
        s_EditingMaterial->VisitProperties(visitor);

        ImGui::Separator();

        // Buttons: Save and Close
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            std::string fullPath = FFPath::GetAbsolutePath(s_EditingMaterialPath);
            if (s_EditingMaterial->SaveToFile(fullPath)) {
                CFFLog::Info(("Material saved: " + s_EditingMaterialPath).c_str());
            } else {
                CFFLog::Error(("Failed to save material: " + s_EditingMaterialPath).c_str());
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Close", ImVec2(120, 0))) {
            s_MaterialEditorOpen = false;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(Changes are applied immediately)");
    }
    ImGui::End();

    // If window closed via X button, reset state
    if (!s_MaterialEditorOpen) {
        s_EditingMaterial = nullptr;
        s_EditingMaterialPath.clear();
    }
}
