#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "World.h"
#include "GameObject.h"

void Panels::DrawHierarchy(CScene& scene) {
    ImGui::Begin("Hierarchy");

    // Keyboard shortcuts (only when Hierarchy window is focused)
    if (ImGui::IsWindowFocused()) {
        auto* selectedObject = scene.GetSelectedObject();

        // Ctrl+C: Copy
        if (ImGui::IsKeyPressed(ImGuiKey_C) && ImGui::GetIO().KeyCtrl) {
            if (selectedObject) {
                scene.CopyGameObject(selectedObject);
            }
        }

        // Ctrl+V: Paste
        if (ImGui::IsKeyPressed(ImGuiKey_V) && ImGui::GetIO().KeyCtrl) {
            auto* newObj = scene.PasteGameObject();
            if (newObj) {
                // Select the newly pasted object
                for (size_t i = 0; i < scene.GetWorld().Count(); ++i) {
                    if (scene.GetWorld().Get(i) == newObj) {
                        scene.SetSelected((int)i);
                        break;
                    }
                }
            }
        }

        // Ctrl+D: Duplicate
        if (ImGui::IsKeyPressed(ImGuiKey_D) && ImGui::GetIO().KeyCtrl) {
            if (selectedObject) {
                auto* newObj = scene.DuplicateGameObject(selectedObject);
                if (newObj) {
                    // Select the newly duplicated object
                    for (size_t i = 0; i < scene.GetWorld().Count(); ++i) {
                        if (scene.GetWorld().Get(i) == newObj) {
                            scene.SetSelected((int)i);
                            break;
                        }
                    }
                }
            }
        }

        // Delete key: Delete selected object
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (selectedObject) {
                int selectedIndex = scene.GetSelected();
                scene.GetWorld().Destroy((size_t)selectedIndex);
                scene.SetSelected(-1);  // Deselect
            }
        }
    }

    // Draw GameObject list
    for (int i = 0; i < (int)scene.GetWorld().Count(); ++i) {
        auto* go = scene.GetWorld().Get((std::size_t)i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
            | ((scene.GetSelected() == i) ? ImGuiTreeNodeFlags_Selected : 0);

        if (ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", go->GetName().c_str())) {
            if (ImGui::IsItemClicked()) {
                scene.SetSelected(i);
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                    scene.CopyGameObject(go);
                }

                // Paste option (always show, enabled only if clipboard has content)
                const char* clipboardText = ImGui::GetClipboardText();
                bool hasClipboard = (clipboardText && strlen(clipboardText) > 0);
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, hasClipboard)) {
                    auto* newObj = scene.PasteGameObject();
                    if (newObj) {
                        for (size_t j = 0; j < scene.GetWorld().Count(); ++j) {
                            if (scene.GetWorld().Get(j) == newObj) {
                                scene.SetSelected((int)j);
                                break;
                            }
                        }
                    }
                }

                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                    auto* newObj = scene.DuplicateGameObject(go);
                    if (newObj) {
                        for (size_t j = 0; j < scene.GetWorld().Count(); ++j) {
                            if (scene.GetWorld().Get(j) == newObj) {
                                scene.SetSelected((int)j);
                                break;
                            }
                        }
                    }
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Delete", "Del")) {
                    scene.GetWorld().Destroy((size_t)i);
                    scene.SetSelected(-1);
                }

                ImGui::EndPopup();
            }

            ImGui::TreePop();
        } else {
            if (ImGui::IsItemClicked()) {
                scene.SetSelected(i);
            }

            // Right-click context menu (collapsed node)
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                    scene.CopyGameObject(go);
                }

                const char* clipboardText = ImGui::GetClipboardText();
                bool hasClipboard = (clipboardText && strlen(clipboardText) > 0);
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, hasClipboard)) {
                    auto* newObj = scene.PasteGameObject();
                    if (newObj) {
                        for (size_t j = 0; j < scene.GetWorld().Count(); ++j) {
                            if (scene.GetWorld().Get(j) == newObj) {
                                scene.SetSelected((int)j);
                                break;
                            }
                        }
                    }
                }

                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                    auto* newObj = scene.DuplicateGameObject(go);
                    if (newObj) {
                        for (size_t j = 0; j < scene.GetWorld().Count(); ++j) {
                            if (scene.GetWorld().Get(j) == newObj) {
                                scene.SetSelected((int)j);
                                break;
                            }
                        }
                    }
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Delete", "Del")) {
                    scene.GetWorld().Destroy((size_t)i);
                    scene.SetSelected(-1);
                }

                ImGui::EndPopup();
            }
        }
    }

    if (ImGui::Button("Create GameObject")) {
        scene.GetWorld().Create("GameObject");
    }

    ImGui::End();
}
