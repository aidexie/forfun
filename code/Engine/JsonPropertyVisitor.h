#pragma once

#include "PropertyVisitor.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ===========================
// JSON Write Visitor
// ===========================
class CJsonWriteVisitor : public CPropertyVisitor {
public:
    CJsonWriteVisitor(json& j) : m_json(j) {}

    void VisitFloat(const char* name, float& value) override {
        m_json[name] = value;
    }

    void VisitInt(const char* name, int& value) override {
        m_json[name] = value;
    }

    void VisitBool(const char* name, bool& value) override {
        m_json[name] = value;
    }

    void VisitString(const char* name, std::string& value) override {
        m_json[name] = value;
    }

    void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) override {
        m_json[name] = { value.x, value.y, value.z };
    }

    void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) override {
        m_json[name] = value;
    }

    // VisitFilePath - just write the string path (ignore filter)
    void VisitFilePath(const char* name, std::string& value, const char* filter) override {
        m_json[name] = value;
    }

private:
    json& m_json;
};

// ===========================
// JSON Read Visitor
// ===========================
class CJsonReadVisitor : public CPropertyVisitor {
public:
    CJsonReadVisitor(const json& j) : m_json(j) {}

    void VisitFloat(const char* name, float& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<float>();
        }
    }

    void VisitInt(const char* name, int& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<int>();
        }
    }

    void VisitBool(const char* name, bool& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<bool>();
        }
    }

    void VisitString(const char* name, std::string& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<std::string>();
        }
    }

    void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) override {
        if (m_json.contains(name) && m_json[name].is_array() && m_json[name].size() == 3) {
            value.x = m_json[name][0].get<float>();
            value.y = m_json[name][1].get<float>();
            value.z = m_json[name][2].get<float>();
        }
    }

    void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<int>();
        }
    }

    // VisitFilePath - just read the string path (ignore filter)
    void VisitFilePath(const char* name, std::string& value, const char* filter) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<std::string>();
        }
    }

private:
    const json& m_json;
};
