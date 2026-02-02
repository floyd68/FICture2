# Static Runtime Migration (/MT)

## Overview

As of v1.1, FICture2 uses **static runtime linking** (`/MT`) instead of dynamic linking (`/MD`).

## Benefits

### 1. Eliminates VC Redistributable Dependency
- No need to install `vcruntime140.dll` or `msvcp140.dll`
- Installer becomes simpler
- Users don't need VC Redistributable installed

### 2. Reduces Network Activity
Windows validates digital signatures of DLLs when they load:
- **Before (with `/MD`)**: Windows checks CRL/OCSP for VC Runtime DLLs → network requests
- **After (with `/MT`)**: No external DLLs to validate → fewer network requests

### 3. Self-Contained Executable
- Runtime is statically linked into the executable
- Better compatibility across Windows systems
- No DLL version conflicts

## Trade-offs

### Increased Executable Size
- **Before**: ~500 KB exe + ~1 MB VC Runtime DLLs (shared with other apps)
- **After**: ~600 KB exe (includes runtime, no external DLLs needed)
- **Net impact**: +100 KB per exe, but no shared DLLs required

### Multiple Instances
If user runs multiple `/MT` applications, each has its own runtime copy in memory.
However:
- Modern Windows handles this efficiently
- Memory overhead is minimal (~1 MB per process)
- FICture2 is designed for single-instance use anyway

## Implementation

### CMakeLists.txt Change

```cmake
# Use static runtime library (/MT)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
```

This setting:
- Applies to FICture2 and all submodules (DirectXTex, zlib, libarchive, etc.)
- Uses `/MT` for Release builds
- Uses `/MTd` for Debug builds

## Building After Migration

### Clean Build Required

After upgrading to v1.1+, you **must** clean your build directory:

```batch
# Delete old CMake cache
rmdir /s /q build_winget
rmdir /s /q build_nexus
rmdir /s /q build_store

# Rebuild
build_winget.bat
```

### Verification

Check that no external runtime DLLs are required:

```batch
# Method 1: Dependency Walker
dumpbin /dependents build_winget\bin\Release\FICture2.exe

# Method 2: Check for runtime DLLs in output
dir build_winget\bin\Release\*.dll

# Should only see ThumbnailProvider.dll, no vcruntime*.dll or msvcp*.dll
```

## Inno Setup

No changes needed in Inno Setup scripts - VC Redistributable installation sections can be removed (if they existed).

## Compatibility

### Minimum Windows Version
- Windows 10 1809+ (same as before)
- No additional requirements

### Visual Studio Version
- Visual Studio 2019 16.8+ (for `/MT` with C++20)
- Visual Studio 2022 recommended

## Network Activity Impact

### Test Results (Expected)

**Before (`/MD` with VC Runtime DLLs):**
- 4-6 network connections during startup
- DNS queries to Microsoft CRL servers
- Certificate validation for `vcruntime140.dll`, `msvcp140.dll`

**After (`/MT` without external DLLs):**
- 2-3 network connections during startup
- Only WIC/DirectX component validation
- **~50% reduction** in network activity

Use `test_network.bat` to verify.

## Rollback (if needed)

To revert to dynamic runtime:

```cmake
# Remove or comment out this section in CMakeLists.txt:
# if(MSVC)
#     set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# endif()
```

Then clean and rebuild.

## FAQ

**Q: Why not use `/MD` like most Windows apps?**  
A: FICture2 prioritizes minimal dependencies and reduced network activity. The small size increase is acceptable for these benefits.

**Q: Does this affect DirectX or WIC?**  
A: No, WIC and DirectX are system components and remain unchanged.

**Q: Can I still use VC Redistributable if I want?**  
A: With `/MT`, it's not needed or used. The runtime is self-contained.

**Q: What about updates to VC Runtime?**  
A: With `/MT`, you control updates by rebuilding with newer compilers. With `/MD`, Windows Update controls it.

---

*Last updated: 2026-01-28*
