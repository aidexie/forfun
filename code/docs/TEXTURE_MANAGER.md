# Texture Manager

The Texture Manager is a singleton system responsible for loading, caching, and managing texture resources with support for both synchronous and asynchronous loading.

**Source Files**:
- `Core/TextureManager.h` / `Core/TextureManager.cpp`
- `Core/TextureHandle.h`

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                      CTextureManager                            │
│  (Singleton - manages all texture loading and caching)          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌─────────────────┐   │
│  │ Sync Cache   │    │ Handle Cache │    │ Pending Queue   │   │
│  │ m_textures   │    │ m_handles    │    │ m_pendingLoads  │   │
│  └──────────────┘    └──────────────┘    └─────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              Default Textures                            │   │
│  │  m_defaultWhite | m_defaultNormal | m_defaultBlack       │   │
│  │                   m_placeholder (checkerboard)           │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      CTextureHandle                             │
│  (Async-friendly wrapper - returns placeholder until ready)     │
├─────────────────────────────────────────────────────────────────┤
│  State: Pending → Loading → Uploading → Ready / Failed          │
│                                                                 │
│  GetTexture() → Returns placeholder OR real texture             │
│  IsReady()    → True when fully loaded                          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Core Components

### CTextureManager (Singleton)

Central manager for all texture operations.

**Key Responsibilities**:
- Load textures from disk (PNG, JPG, TGA, BMP, KTX2)
- Cache textures to avoid duplicate loading
- Manage color space (sRGB vs Linear)
- Provide default/fallback textures
- Process async loads with frame-budget control

**Access**:
```cpp
CTextureManager& mgr = CTextureManager::Instance();
```

### CTextureHandle

Async-friendly texture wrapper that provides transparent access to textures that may still be loading.

**States**:
| State | Description |
|-------|-------------|
| `Pending` | Queued for loading, not yet started |
| `Loading` | Currently loading from disk |
| `Uploading` | GPU upload in progress |
| `Ready` | Fully loaded and usable |
| `Failed` | Load failed, using fallback texture |

---

## API Reference

### Synchronous Loading (Blocking)

```cpp
RHI::TextureSharedPtr Load(const std::string& path, bool srgb);
```

**Behavior**:
- Blocks until texture is fully loaded
- Returns cached texture if already loaded
- Returns default texture if load fails

**Parameters**:
- `path`: Relative path from assets directory (e.g., `"textures/wood_albedo.png"`)
- `srgb`: `true` for sRGB (albedo, emissive), `false` for linear (normal, metallic, roughness, AO)

**Example**:
```cpp
// Load albedo texture (sRGB)
auto albedo = CTextureManager::Instance().Load("textures/wood_albedo.png", true);

// Load normal map (linear)
auto normal = CTextureManager::Instance().Load("textures/wood_normal.png", false);
```

### Asynchronous Loading (Non-Blocking) - Preferred

```cpp
TextureHandlePtr LoadAsync(const std::string& path, bool srgb);
```

**Behavior**:
- Returns immediately with a TextureHandle
- Handle returns placeholder texture until real texture is ready
- Requires calling `Tick()` to process pending loads

**Example**:
```cpp
// Queue texture for async loading
TextureHandlePtr handle = CTextureManager::Instance().LoadAsync("textures/large_texture.png", true);

// Use immediately - returns placeholder
RHI::ITexture* tex = handle->GetTexture();

// Later, check if ready
if (handle->IsReady()) {
    // Now returns real texture
    tex = handle->GetTexture();
}
```

### Processing Async Loads

```cpp
uint32_t Tick(uint32_t maxLoadsPerFrame = 2);
```

**Call at frame start** to process pending texture loads.

**Parameters**:
- `maxLoadsPerFrame`: Maximum textures to load this frame (0 = unlimited)

**Returns**: Number of textures actually loaded

**Example**:
```cpp
// In main loop
void OnFrameStart() {
    // Load up to 2 textures per frame to maintain framerate
    CTextureManager::Instance().Tick(2);
}
```

### Flush All Pending Loads

```cpp
void FlushPendingLoads();
```

Blocks and loads all pending textures. Useful for:
- Loading screens
- Scene initialization
- Test cases

### Default Textures

```cpp
RHI::TextureSharedPtr GetDefaultWhite();   // 1x1 white (sRGB)
RHI::TextureSharedPtr GetDefaultNormal();  // 1x1 flat normal RGB(128,128,255)
RHI::TextureSharedPtr GetDefaultBlack();   // 1x1 black (linear)
RHI::TextureSharedPtr GetPlaceholder();    // 8x8 magenta/black checkerboard
```

### Utility Methods

```cpp
bool IsLoaded(const std::string& path) const;   // Check if texture is cached
uint32_t GetPendingCount() const;               // Number of pending loads
bool HasPendingLoads() const;                   // Any pending loads?
void Clear();                                    // Clear all cached textures
void Shutdown();                                 // Release all resources
```

---

## Usage Patterns

### Pattern 1: Immediate Use with Async Loading

```cpp
// Load async - returns immediately
TextureHandlePtr handle = CTextureManager::Instance().LoadAsync("tex.png", true);

// Can use immediately in rendering - placeholder shown until ready
material->SetAlbedo(handle->GetTexture());

// In render loop, Tick() processes loads gradually
CTextureManager::Instance().Tick(2);
```

### Pattern 2: Wait for Specific Texture

```cpp
TextureHandlePtr handle = CTextureManager::Instance().LoadAsync("important.png", true);

// If you need to wait for this specific texture
while (!handle->IsReady()) {
    CTextureManager::Instance().Tick(1);
}
```

### Pattern 3: Loading Screen

```cpp
void LoadScene() {
    // Queue all textures
    for (const auto& path : texturePaths) {
        LoadAsync(path, true);
    }

    // Show loading screen and flush all
    ShowLoadingScreen();
    CTextureManager::Instance().FlushPendingLoads();
    HideLoadingScreen();
}
```

### Pattern 4: Material System Integration

```cpp
class CMaterial {
    TextureHandlePtr m_albedoHandle;
    TextureHandlePtr m_normalHandle;

public:
    void LoadTextures(const std::string& albedoPath, const std::string& normalPath) {
        m_albedoHandle = CTextureManager::Instance().LoadAsync(albedoPath, true);
        m_normalHandle = CTextureManager::Instance().LoadAsync(normalPath, false);
    }

    // Always returns valid texture (placeholder or real)
    RHI::ITexture* GetAlbedo() const { return m_albedoHandle->GetTexture(); }
    RHI::ITexture* GetNormal() const { return m_normalHandle->GetTexture(); }

    bool IsFullyLoaded() const {
        return m_albedoHandle->IsReady() && m_normalHandle->IsReady();
    }
};
```

---

## Color Space Guidelines

| Texture Type | sRGB Parameter | Format |
|--------------|----------------|--------|
| Albedo / Diffuse | `true` | R8G8B8A8_UNORM_SRGB |
| Emissive | `true` | R8G8B8A8_UNORM_SRGB |
| Normal Map | `false` | R8G8B8A8_UNORM |
| Metallic | `false` | R8G8B8A8_UNORM |
| Roughness | `false` | R8G8B8A8_UNORM |
| AO | `false` | R8G8B8A8_UNORM |
| Height Map | `false` | R8G8B8A8_UNORM |

---

## Supported Formats

| Extension | Loader | Notes |
|-----------|--------|-------|
| `.png` | WIC | Common, lossless |
| `.jpg` / `.jpeg` | WIC | Lossy, no alpha |
| `.tga` | WIC | Legacy format |
| `.bmp` | WIC | Uncompressed |
| `.ktx2` | KTX Loader | GPU-compressed (BC7, ASTC, etc.) |
| `.ktx` | KTX Loader | Legacy KTX format |

**Note**: KTX2 files have format embedded - `srgb` parameter is ignored.

---

## Caching System

### Cache Key Format
```
{path}|srgb    // e.g., "textures/wood.png|srgb"
{path}|linear  // e.g., "textures/wood.png|linear"
```

Same texture loaded with different sRGB settings creates separate cache entries.

### Two-Level Cache

1. **Sync Cache** (`m_textures`): Stores `RHI::TextureSharedPtr`
2. **Handle Cache** (`m_handles`): Stores `TextureHandlePtr`

When async load completes, texture is added to both caches for compatibility.

---

## Performance Considerations

### Tick Budget
- **Default**: 2 textures per frame
- **1080p texture**: ~5-15ms load time (disk I/O + GPU upload + mip gen)
- Adjust `maxLoadsPerFrame` based on target framerate

### Best Practices
1. Use `LoadAsync()` for all non-critical textures
2. Call `Tick()` early in frame (before rendering)
3. Use `FlushPendingLoads()` only during loading screens
4. Keep placeholder texture visible time minimal for UX

---

## Ownership Model

Uses `shared_ptr` for texture ownership (UE4/UE5 style):
- Multiple systems can safely reference same texture
- Textures automatically released when all references dropped
- No manual reference counting needed

```cpp
using TextureHandlePtr = std::shared_ptr<CTextureHandle>;
using RHI::TextureSharedPtr = std::shared_ptr<RHI::ITexture>;
```

---

## Shutdown

**Must call before RHI shutdown**:
```cpp
CTextureManager::Instance().Shutdown();
```

This releases all texture resources and clears caches.
