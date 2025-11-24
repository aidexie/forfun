#pragma once
#include "TestCase.h"
#include <string>
#include <map>
#include <vector>

// Global test registry (singleton)
class CTestRegistry {
public:
    static CTestRegistry& Instance() {
        static CTestRegistry instance;
        return instance;
    }

    // Register a test
    void Register(const std::string& name, ITestCase* test) {
        m_tests[name] = test;
    }

    // Get a test by name
    ITestCase* Get(const std::string& name) {
        auto it = m_tests.find(name);
        return (it != m_tests.end()) ? it->second : nullptr;
    }

    // Get all test names
    std::vector<std::string> GetAllTestNames() const {
        std::vector<std::string> names;
        for (const auto& pair : m_tests) {
            names.push_back(pair.first);
        }
        return names;
    }

private:
    CTestRegistry() = default;
    std::map<std::string, ITestCase*> m_tests;
};

// Macro for automatic test registration
#define REGISTER_TEST(TestClass) \
    static TestClass g_##TestClass##_instance; \
    static struct TestClass##_Registrar { \
        TestClass##_Registrar() { \
            CTestRegistry::Instance().Register(#TestClass, &g_##TestClass##_instance); \
        } \
    } g_##TestClass##_registrar;
