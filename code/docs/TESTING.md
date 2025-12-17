# Automated Testing Guide

Complete guide for the automated testing framework.

**Related Documents**:
- `CLAUDE.md` - AI working guidelines (high-level overview)
- `Core/Testing/` - Test framework implementation

---

## Quick Reference

**Run a test**:
```bash
./build/Debug/forfun.exe --test TestRayCast
```

**List all tests**:
```bash
./build/Debug/forfun.exe --list-tests
```

**Debug output location**:
```
E:/forfun/debug/{TestName}/
  ├── runtime.log              # Frame-by-frame execution log
  ├── test.log                 # Test session log with assertions
  └── screenshot_frame20.png   # Visual verification
```

---

## Test Framework Architecture

### Core Components

**Location**: `Core/Testing/`
- `TestCase.h` - `ITestCase` interface, `CTestContext` API
- `TestRegistry.h` - `REGISTER_TEST()` macro
- `Screenshot.h` - `CScreenshot::CaptureTest()`
- `Assertions.h` - `ASSERT_*` macros
- `Tests/` - All test implementations

### Frame Callback Pattern

**Core Concept**: Tests execute within the normal engine loop using frame callbacks to schedule operations.

```cpp
class CTestMyFeature : public ITestCase {
public:
    const char* GetName() const override { return "TestMyFeature"; }

    void Setup(CTestContext& ctx) override {
        ctx.OnFrame(1, [&]() {
            // Frame 1-10: Scene setup
            // Create test scene, load resources
        });

        ctx.OnFrame(20, [&]() {
            // Frame 20: Test + Screenshot + Assertions
            CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);
            ASSERT_EQUAL(ctx, actual, expected, "Description");

            // Log visual expectations for AI analysis
            CFFLog::Info("VISUAL_EXPECTATION: Sky should be blue");
        });

        ctx.OnFrame(30, [&]() {
            // Frame 30: Finalization
            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestMyFeature)
```

**Why Frame Callbacks?**
- Tests run in normal rendering loop (not separate thread)
- Allows GPU resources to initialize properly
- Enables visual verification via screenshots
- Ensures proper timing for async operations

---

## Test Naming Convention

**CRITICAL**: Test names are registered using `GetName()` return value (since 2025-11-25).

**Example**:
```cpp
class CTestMaterialAsset : public ITestCase {
public:
    const char* GetName() const override {
        return "TestMaterialAsset";  // ← This is the registered name
    }
};
REGISTER_TEST(CTestMaterialAsset)
```

**Command line usage**:
```bash
# Run specific test (use GetName() value, NOT class name)
./forfun.exe --test TestMaterialAsset       # ✅ Correct
./forfun.exe --test CTestMaterialAsset      # ❌ Wrong (class name)
```

**Naming Convention**:
- **Class name**: `CTestFeatureName` (with C prefix, following CODING_STYLE.md)
- **GetName() return**: `"TestFeatureName"` (without C prefix, user-friendly)
- **Command line**: Use the `GetName()` value

**Before 2025-11-25**: Tests were registered using class name, requiring `--test CTestName`. This was confusing and has been changed.

---

## Assertion Macros

```cpp
ASSERT(ctx, condition, "Description");
ASSERT_EQUAL(ctx, actual, expected, "Description");
ASSERT_NOT_NULL(ctx, pointer, "Description");
ASSERT_IN_RANGE(ctx, value, min, max, "Description");
ASSERT_VEC3_EQUAL(ctx, actual, expected, epsilon, "Description");
```

**Benefits**:
- Test failures don't crash the application
- All failures are recorded in `ctx.failures`
- Final pass/fail determined at end of test

**Example**:
```cpp
ctx.OnFrame(20, [&]() {
    auto* camera = scene.GetMainCamera();
    ASSERT_NOT_NULL(ctx, camera, "Main camera should exist");

    XMFLOAT3 pos = camera->GetPosition();
    ASSERT_IN_RANGE(ctx, pos.x, -10.0f, 10.0f, "Camera X should be in range");
});
```

---

## Visual Expectations

Use `VISUAL_EXPECTATION` logs for AI analysis:

```cpp
ctx.OnFrame(20, [&]() {
    CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);

    // Describe what the screenshot should show
    CFFLog::Info("VISUAL_EXPECTATION: Sky should be blue with visible clouds");
    CFFLog::Info("VISUAL_EXPECTATION: No black/pink missing texture colors");
    CFFLog::Info("VISUAL_EXPECTATION: Environment lighting visible on test cube");
});
```

**Why Visual Expectations?**
- AI can analyze screenshots using multimodal capabilities
- Provides context for what "correct" rendering looks like
- Helps catch visual regressions that assertions might miss

---

## Development Workflow

### Step 1: Implement Feature

When user requests "implement XXX feature":
1. Implement core functionality
2. **Proactively write automated test** (don't wait for user to ask)
3. Test naming: `CTestFeatureName`

### Step 2: Write Test

**Minimum test structure**:
```cpp
- Frame 1-10: Scene setup (create objects, load resources)
- Frame 20: Test execution + Screenshot + Assertions + Visual expectations
- Frame 30: Finalization (check failures, set testPassed, call Finish())
```

**Good test checklist**:
- ✅ Creates minimal test scene
- ✅ Uses appropriate geometry (sphere for lighting, cube for simple tests)
- ✅ Sets up materials explicitly (see "Material Setup" below)
- ✅ Takes screenshot at Frame 20
- ✅ Logs VISUAL_EXPECTATION descriptions
- ✅ Uses assertions to verify logic
- ✅ Finalizes at Frame 30

### Step 3: Run Test

```bash
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestXXX
```

### Step 4: AI Auto-Analysis

**After test runs, AI must automatically**:

1. **Read test log**: `E:/forfun/debug/{TestName}/test.log`
   - Check for "✓ ALL ASSERTIONS PASSED" or "✗ TEST FAILED"
   - Extract VISUAL_EXPECTATION descriptions
   - Record any failure messages

2. **Read screenshot**: `E:/forfun/debug/{TestName}/screenshot_frame*.png`
   - Use multimodal AI to view the image
   - Compare with VISUAL_EXPECTATION descriptions
   - Check for obvious errors (black screen, missing textures, wrong colors)

3. **Read runtime log** (if needed): `E:/forfun/debug/{TestName}/runtime.log`
   - Look for ERROR messages
   - Check resource loading issues

### Step 5: Generate Test Report

**Standard format**:
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
测试分析报告：{TestName}
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

✅ 断言状态：所有断言通过 (0 failures)
   或
✗ 断言状态：3 个断言失败
   - [TestName:Frame10] Object count: expected 1, got 2

✅ 视觉验证：截图符合预期
   - ✓ Sky shows blue color with clouds
   或
✗ 视觉验证：发现问题
   - ✗ Screenshot shows black screen

📊 日志摘要：
   - Frame 1: Scene setup complete
   - Frame 20: Screenshot captured

📸 截图：
   - screenshot_frame20.png (950x803)
   - 显示：[AI description of what it sees]

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ 总结：功能实现正确，测试通过
   或
✗ 总结：测试失败，需要修复以下问题：
   1. [Specific issue]
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### Step 6: Failure Handling

If test fails:
1. Analyze failure from logs and screenshot
2. Fix code
3. Rebuild
4. Re-run test (back to Step 3)
5. Repeat until passing

---

## Testing Best Practices

### Geometry Selection

**⚠️ CRITICAL LESSON (2025-11-28)**: Choose appropriate geometry for the test

**Use Sphere for**:
- ✅ Lighting tests (point lights, spot lights)
- ✅ PBR material tests
- ✅ Any test requiring uniform surface normals from all angles

**Reason**: Cube only has 6 faces with outward normals. If light comes from above, only the top face receives lighting. This causes false failures where lights work correctly but appear dark in screenshots.

**Example - TestSpotLight failure**:
```cpp
// ❌ BAD - Cube with lights from above
auto* cube = scene.GetWorld().Create("TestCube");
auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
meshRenderer->path = "mesh/cube.obj";  // Only top face lit!

// Spot light above pointing down → Only top face visible
// From camera angle, sides appear black → FALSE FAILURE

// ✅ GOOD - Sphere with lights from above
auto* sphere = scene.GetWorld().Create("TestSphere");
auto* meshRenderer = sphere->AddComponent<SMeshRenderer>();
meshRenderer->path = "mesh/sphere.obj";  // All angles receive light!
```

**Use Cube for**:
- ✅ Transform/rotation tests
- ✅ Raycast/collision tests
- ✅ Simple geometry tests
- ✅ Tests where lighting doesn't matter

### Material Setup

**⚠️ CRITICAL**: Always set material explicitly in tests

**Problem**: Default material may have low albedo or missing properties, causing dark/invisible objects.

**Solution**: Use `.ffasset` material files

```cpp
// ✅ CORRECT - Explicit material setup
auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
meshRenderer->path = "mesh/sphere.obj";
meshRenderer->materialPath = "materials/default_white.ffasset";  // Bright albedo
```

**Available materials** (E:/forfun/assets/materials/):
- `default_white.ffasset` - White albedo (1.0, 1.0, 1.0), non-metallic
- `default_metal.ffasset` - Metallic material

**Why .ffasset?**
- Material component data is stored in asset files, not in code
- Cannot set albedo/metallic/roughness directly via C++ API
- Must use pre-made material assets

### Light Intensity Guidelines

**Point Lights**:
- Close range (< 5m): 50-100 intensity
- Medium range (5-10m): 100-300 intensity
- Far range (> 10m): 300-1000 intensity

**Spot Lights** (2025-11-28):
- **Use higher intensity than point lights** (10x recommended)
- Close range: 500-1000 intensity
- Medium range: 1000-3000 intensity
- Reason: Cone attenuation reduces effective brightness

**Directional Lights**:
- Standard: 0.01-0.1 intensity (represents sun)

**Example**:
```cpp
// Spot light 4 meters above cube
auto* light = spotLight->AddComponent<SSpotLight>();
light->color = XMFLOAT3(1.0f, 0.0f, 0.0f);  // Red
light->intensity = 500.0f;  // High enough to be visible
light->range = 8.0f;
light->innerConeAngle = 20.0f;
light->outerConeAngle = 35.0f;
```

### Camera Setup

**Default test camera** (from `CMainPass`):
- Position: `(-6, 0.8, 0)` world space
- Looking: `+X` direction (right)
- Up: `+Y`
- Height: `0.8` units (typical eye level)

**Positioning test objects**:
```cpp
// Object should be in +X direction from camera, at same height
XMFLOAT3 cubePos = {5.0f, 0.8f, 0.0f};  // 5 units in front, same height

// Multiple objects: spread along X and Z
XMFLOAT3 positions[] = {
    {2.0f, 0.8f, -2.0f},  // Left (negative Z)
    {5.0f, 0.8f, 0.0f},   // Center
    {8.0f, 0.8f, 2.0f}    // Right (positive Z)
};
```

---

## Common Issues & Solutions

### Issue: Shader Compilation Errors Not Showing

**Problem**: Error log shows empty `{}` placeholders
```
[ERROR] Shader compilation error: {}
```

**Cause**: `CFFLog` uses printf-style formatting (`%s`, `%d`), not `{}` placeholders

**Solution**: Use correct format specifiers
```cpp
// ❌ Wrong
CFFLog::Error("Shader error: {}", errorMessage);

// ✅ Correct
CFFLog::Error("Shader error: %s", errorMessage);
```

### Issue: HLSL Reserved Keywords

**Problem**: Shader compilation fails with "unexpected token"

**Common reserved words**: `point`, `line`, `triangle`, `linear`, `sample`

**Solution**: Rename variables
```hlsl
// ❌ Wrong
bool PointInCone(float3 point, ...) {  // 'point' is reserved!

// ✅ Correct
bool PointInCone(float3 p, ...) {  // Use 'p' instead
```

### Issue: Objects Appear Black/Dark

**Checklist**:
1. ✅ Did you set material explicitly? (`materialPath = "materials/default_white.ffasset"`)
2. ✅ Is light intensity high enough? (Spot lights need 500+)
3. ✅ Are you using sphere instead of cube for lighting tests?
4. ✅ Is light within range of objects?
5. ✅ Are surface normals facing the light?

### Issue: Test Not Found

**Problem**: `./forfun.exe --test CTestMyFeature` returns "Test not found"

**Solution**: Use `GetName()` value, not class name
```bash
# ❌ Wrong
./forfun.exe --test CTestMyFeature

# ✅ Correct
./forfun.exe --test TestMyFeature
```

---

## Test Lifecycle

```
1. User requests feature
   ↓
2. AI implements feature
   ↓
3. AI proactively writes test (CTestFeatureName)
   ↓
4. AI builds project
   ↓
5. AI runs test (timeout 15s)
   ↓
6. AI automatically reads:
   - test.log (assertions)
   - screenshot (visual verification)
   - runtime.log (errors)
   ↓
7. AI generates test report
   ↓
8. If PASS: Done! ✅
   If FAIL: Fix → Rebuild → Goto step 5 ↻
```

---

## Advanced Topics

### Custom Assertions

Add to `Core/Testing/Assertions.h`:

```cpp
#define ASSERT_CUSTOM(ctx, condition, message) \
    do { \
        if (!(condition)) { \
            std::string failMsg = "[" + ctx.testName + ":Frame" + \
                std::to_string(ctx.frameCount) + "] " + message; \
            ctx.failures.push_back(failMsg); \
            CFFLog::Error("✗ ASSERTION FAILED: %s", failMsg.c_str()); \
        } \
    } while(0)
```

### Multi-Screenshot Tests

```cpp
ctx.OnFrame(10, [&]() {
    CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 10);
    CFFLog::Info("VISUAL_EXPECTATION: Initial state - objects not loaded");
});

ctx.OnFrame(20, [&]() {
    CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);
    CFFLog::Info("VISUAL_EXPECTATION: After load - objects visible");
});
```

### Performance Testing

```cpp
ctx.OnFrame(20, [&]() {
    auto start = std::chrono::high_resolution_clock::now();

    // Run expensive operation
    scene.PerformComplexOperation();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    ASSERT(ctx, duration.count() < 16, "Operation should complete within 16ms (60 FPS)");
});
```

---

**Last Updated**: 2025-12-17
