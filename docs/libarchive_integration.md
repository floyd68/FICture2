# libarchive Integration Guide

## Overview

FICture2 uses libarchive to support reading image files from compressed archives (ZIP, 7-Zip, RAR).
BA2 archives are handled by a custom reader.

## Build Configuration

### Submodule Setup

libarchive is included as a Git submodule:
```bash
git submodule add https://github.com/libarchive/libarchive.git external/libarchive
```

### Minimal Build

Build script: `external/libarchive/build-minimal.ps1`

**CMake Configuration:**
```cmake
-G "Visual Studio 18 2026"       # VS 2026 (v145 toolset)
-A x64                           # 64-bit platform
-DBUILD_SHARED_LIBS=OFF          # Static library
-DENABLE_ZLIB=ON                 # ZIP deflate (REQUIRED)
-DENABLE_LZMA=ON                 # 7z LZMA/XZ (REQUIRED)
-DENABLE_BZip2=OFF               # Disabled (not needed)
-DENABLE_ZSTD=OFF                # Disabled (not needed)
-DENABLE_LZ4=OFF                 # Disabled (not needed)
-DENABLE_LZO=OFF                 # Disabled (not needed)
-DENABLE_OPENSSL=OFF             # No encryption
-DENABLE_TAR=OFF                 # No TAR format
-DENABLE_CPIO=OFF                # No CPIO format
-DENABLE_CAT=OFF                 # No cat utility
-DENABLE_TEST=OFF                # No tests
-DENABLE_INSTALL=OFF             # No install
-DENABLE_ACL=OFF                 # No ACL support
-DENABLE_XATTR=OFF               # No extended attributes
```

**Note**: FICture2 uses PlatformToolset `v145` (Visual Studio 2026). The CMake generator "Visual Studio 18 2026" matches this toolset.

**Build Output:**
- Debug: `external/libarchive/build-minimal/libarchive/Debug/archive.lib`
- Release: `external/libarchive/build-minimal/libarchive/Release/archive.lib`
- Size: ~2.53 MB (static library)

## Format Support

### Supported Archive Formats

| Format | Read | Write | Compression Filters |
|--------|------|-------|---------------------|
| **ZIP** | ✅ | ❌ | gzip (deflate) |
| **7-Zip** | ✅ | ❌ | LZMA, LZMA2, XZ |
| **RAR** | ✅ | ❌ | Built-in decoder |
| **BA2** | ✅ (zlib) | ❌ | zlib/deflate (custom reader) |

### Compression Filters

Only essential filters are enabled to minimize executable size:

```cpp
archive_read_support_filter_none(a);    // Uncompressed
archive_read_support_filter_gzip(a);    // ZIP deflate/gzip
archive_read_support_filter_lzma(a);    // 7z LZMA
archive_read_support_filter_xz(a);      // 7z XZ
```

**Excluded filters** (to reduce size):
- ❌ `bzip2` - BZip2 compression (not used in ZIP/7z/RAR typically)
- ❌ `zstd` - Zstandard compression
- ❌ `lz4` - LZ4 compression
- ❌ `lzo` - LZO compression
- ❌ `uu` - UUencode
- ❌ `rpm` - RPM package
- ❌ `compress` - Unix compress
- ❌ `grzip` - grzip
- ❌ `lrzip` - lrzip
- ❌ `lzop` - lzop

## Code Integration

### Preprocessor Definitions

Required for static linking:
```xml
<PreprocessorDefinitions>
  LIBARCHIVE_STATIC;
  %(PreprocessorDefinitions)
</PreprocessorDefinitions>
```

### Include Paths

```xml
<AdditionalIncludeDirectories>
  $(SolutionDir)external\libarchive\libarchive;
  $(SolutionDir)external\libarchive\build-minimal;
  %(AdditionalIncludeDirectories)
</AdditionalIncludeDirectories>
```

### Library Paths

**Debug:**
```xml
<AdditionalLibraryDirectories>
  $(SolutionDir)external\libarchive\build-minimal\libarchive\Debug;
  %(AdditionalLibraryDirectories)
</AdditionalLibraryDirectories>
```

**Release:**
```xml
<AdditionalLibraryDirectories>
  $(SolutionDir)external\libarchive\build-minimal\libarchive\Release;
  %(AdditionalLibraryDirectories)
</AdditionalLibraryDirectories>
```

### Link Dependencies

```xml
<AdditionalDependencies>
  archive.lib;
  %(AdditionalDependencies)
</AdditionalDependencies>
```

## API Usage

### ArchiveReader Interface

```cpp
#include "ArchiveReader.h"

// Check if file is an archive
if (ArchiveReaderFactory::IsArchiveFile(path))
{
    // Open archive
    auto reader = ArchiveReaderFactory::Open(path);
    if (reader)
    {
        // List entries
        auto entries = reader->ListEntries();
        
        // Extract file to memory
        auto data = reader->ExtractToMemory(L"image.dds");
        
        // Check format
        std::wstring format = reader->GetFormatName();
    }
}
```

### Supported Extensions

```cpp
std::vector<std::wstring> exts = 
    ArchiveReaderFactory::GetSupportedExtensions();
// Returns: { L".zip", L".7z", L".rar", L".ba2" }
```

## Size Impact

### Executable Size Comparison

| Configuration | Size | Delta |
|--------------|------|-------|
| **Original FICture2.exe** | 497 KB | - |
| **With libarchive (filter_all)** | ~3.0 MB | +2.5 MB |
| **With minimal filters** | ~583 KB | +86 KB |

### Size Optimization Results

By using specific filters instead of `archive_read_support_filter_all()`:
- **Saved**: ~2.4 MB
- **Final overhead**: Only ~86 KB for ZIP/7z/RAR support

## Performance Considerations

### Memory Usage

- Archives are scanned once on open
- File index is cached in memory
- Extraction is done on-demand
- No temporary files created

### Threading

- ArchiveReader is not thread-safe
- Create separate instances per thread
- ImageCore's decode scheduler handles threading

## Future Enhancements

### Planned Features

1. **BA2 Format Support**
   - Bethesda Archive 2 (Fallout 4, Starfield)
   - Custom parser with GNRL/DX10 support
   - Zlib-compressed entries are supported

2. **VirtualPath System**
   - Treat archives as virtual directories
   - Seamless navigation in ImageBrowser
   - Breadcrumb navigation

3. **Thumbnail Caching**
   - Cache thumbnails for archive contents
   - Invalidate on archive modification
   - Persistent cache across sessions

4. **Write Support**
   - Create ZIP archives
   - Export selected images
   - Batch operations

## Troubleshooting

### Linker Errors: `__imp_archive_*`

**Problem**: DLL import symbols when using static library

**Solution**: Add `LIBARCHIVE_STATIC` preprocessor definition

### Missing `config.h`

**Problem**: Cannot find `config.h` during compilation

**Solution**: Add build directory to include paths:
```
$(SolutionDir)external\libarchive\build-minimal
```

### Unsupported Archive Format

**Problem**: Archive opens but files cannot be extracted

**Solution**: Check if required compression filter is enabled in `ArchiveReader.cpp`

## References

- [libarchive Official Site](https://www.libarchive.org/)
- [libarchive GitHub](https://github.com/libarchive/libarchive)
- [libarchive Wiki](https://github.com/libarchive/libarchive/wiki)
- [Format Documentation](https://github.com/libarchive/libarchive/wiki/LibarchiveFormats)

## License

libarchive is licensed under the BSD 2-Clause License.

See: `external/libarchive/LICENSE`
