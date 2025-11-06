# JSON Library Setup

The scene serialization feature requires the **nlohmann/json** library (single header file).

## Installation Steps

1. Download the latest `json.hpp` from:
   https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

2. Create directory:
   ```
   E:\forfun\thirdparty\nlohmann\
   ```

3. Place `json.hpp` in:
   ```
   E:\forfun\thirdparty\nlohmann\json.hpp
   ```

4. Rebuild the project:
   ```bash
   cmake --build build --target forfun
   ```

## Verification

After setup, you should be able to:
- Click "File > Save Scene" to save current scene as `.scene` file
- Click "File > Load Scene" to load a saved scene
- Scene files are human-readable JSON format

## Alternative: Direct Download (PowerShell)

```powershell
New-Item -ItemType Directory -Force -Path "E:\forfun\thirdparty\nlohmann"
Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" -OutFile "E:\forfun\thirdparty\nlohmann\json.hpp"
```

## License

nlohmann/json is licensed under MIT License.
