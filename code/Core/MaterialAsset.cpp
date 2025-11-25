#include "MaterialAsset.h"
#include "FFLog.h"
#include "Engine/JsonPropertyVisitor.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;
using namespace DirectX;

CMaterialAsset::CMaterialAsset(const std::string& name)
    : name(name) {}

bool CMaterialAsset::LoadFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error(("Failed to open material file: " + filepath).c_str());
        return false;
    }

    try {
        json j;
        file >> j;
        return FromJson(j.dump());
    }
    catch (const std::exception& e) {
        CFFLog::Error(("Failed to parse material JSON: " + std::string(e.what())).c_str());
        return false;
    }
}

bool CMaterialAsset::SaveToFile(const std::string& filepath) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error(("Failed to create material file: " + filepath).c_str());
        return false;
    }

    try {
        std::string jsonStr = ToJson();
        json j = json::parse(jsonStr);
        file << j.dump(4);  // Pretty print with 4-space indent
        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error(("Failed to write material JSON: " + std::string(e.what())).c_str());
        return false;
    }
}

std::string CMaterialAsset::ToJson() const {
    json j;
    j["type"] = "material";
    j["version"] = "1.0";

    // Use reflection system for serialization
    // VisitProperties now uses exact variable names, so JSON keys match member names
    CMaterialAsset* mutableThis = const_cast<CMaterialAsset*>(this);
    CJsonWriteVisitor writer(j);
    mutableThis->VisitProperties(writer);

    return j.dump();
}

bool CMaterialAsset::FromJson(const std::string& jsonStr) {
    try {
        json j = json::parse(jsonStr);

        // Verify type
        if (j.value("type", "") != "material") {
            CFFLog::Error("Invalid material file: missing 'type' field");
            return false;
        }

        // Use reflection system for deserialization
        // VisitProperties now uses exact variable names, so JSON keys match member names
        CJsonReadVisitor reader(j);
        VisitProperties(reader);

        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error(("Failed to parse material JSON: " + std::string(e.what())).c_str());
        return false;
    }
}
