#include "Panels.h"
#include "imgui.h"
#include "Engine/Rendering/IBLGenerator.h"

void Panels::DrawIrradianceDebug(IBLGenerator* iblGen) {
    if (!iblGen) return;

    ImGui::Begin("Irradiance Map Debug");

    ImGui::Text("Cubemap Face Layout (HDR R16G16B16A16_FLOAT)");
    ImGui::Separator();

    // Face names for clarity
    const char* faceNames[6] = {
        "+X (Right)",
        "-X (Left)",
        "+Y (Top)",
        "-Y (Bottom)",
        "+Z (Front)",
        "-Z (Back)"
    };

    // Display size for each face
    float displaySize = 128.0f;
    ImGui::SliderFloat("Display Size", &displaySize, 64.0f, 512.0f);
    ImGui::Separator();

    // Cross layout for cubemap (standard unwrap):
    //        [+Y]
    // [-X] [-Z] [+X] [+Z]
    //        [-Y]

    ImGui::BeginChild("CubemapLayout", ImVec2(0, 0), true);

    // Row 1: +Y (Top)
    ImGui::Dummy(ImVec2(displaySize, 0));  // Space before
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("+Y (Top)");
    ID3D11ShaderResourceView* topSRV = iblGen->GetIrradianceFaceSRV(2);
    if (topSRV) {
        ImGui::Image((void*)topSRV, ImVec2(displaySize, displaySize));
    } else {
        ImGui::Text("Not Generated");
    }
    ImGui::EndGroup();

    // Row 2: -X, -Z, +X, +Z (horizontal strip)
    ImGui::BeginGroup();

    // -X (Left)
    ImGui::BeginGroup();
    ImGui::Text("-X (Left)");
    ID3D11ShaderResourceView* leftSRV = iblGen->GetIrradianceFaceSRV(1);
    if (leftSRV) {
        ImGui::Image((void*)leftSRV, ImVec2(displaySize, displaySize));
    } else {
        ImGui::Text("Not Generated");
    }
    ImGui::EndGroup();
    ImGui::SameLine();

    // -Z (Back)
    ImGui::BeginGroup();
    ImGui::Text("+Z (Back)");
    ID3D11ShaderResourceView* backSRV = iblGen->GetIrradianceFaceSRV(4);
    if (backSRV) {
        ImGui::Image((void*)backSRV, ImVec2(displaySize, displaySize));
    } else {
        ImGui::Text("Not Generated");
    }
    ImGui::EndGroup();
    ImGui::SameLine();

    // +X (Right)
    ImGui::BeginGroup();
    ImGui::Text("+X (Right)");
    ID3D11ShaderResourceView* rightSRV = iblGen->GetIrradianceFaceSRV(0);
    if (rightSRV) {
        ImGui::Image((void*)rightSRV, ImVec2(displaySize, displaySize));
    } else {
        ImGui::Text("Not Generated");
    }
    ImGui::EndGroup();
    ImGui::SameLine();

    // +Z (Front)
    ImGui::BeginGroup();
    ImGui::Text("-Z (Front)");
    ID3D11ShaderResourceView* frontSRV = iblGen->GetIrradianceFaceSRV(5);
    if (frontSRV) {
        ImGui::Image((void*)frontSRV, ImVec2(displaySize, displaySize));
    } else {
        ImGui::Text("Not Generated");
    }
    ImGui::EndGroup();

    ImGui::EndGroup();

    // Row 3: -Y (Bottom)
    ImGui::Dummy(ImVec2(displaySize, 0));  // Space before
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("-Y (Bottom)");
    ID3D11ShaderResourceView* bottomSRV = iblGen->GetIrradianceFaceSRV(3);
    if (bottomSRV) {
        ImGui::Image((void*)bottomSRV, ImVec2(displaySize, displaySize));
    } else {
        ImGui::Text("Not Generated");
    }
    ImGui::EndGroup();

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextWrapped("Note: This displays HDR values tonemapped by ImGui. "
                      "For accurate inspection, check individual pixel values with a color picker tool.");

    ImGui::End();
}
