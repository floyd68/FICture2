# VirtualPath System

## Overview

The VirtualPath system provides a unified abstraction for accessing files both on the filesystem and inside compressed archives (ZIP, 7-Zip, RAR). It allows treating archives as virtual directories, enabling seamless navigation and file access.

## Core Concepts

### VirtualPath

A `VirtualPath` represents a file location that can be either:
1. **Regular file**: `D:\textures\image.dds`
2. **File inside archive**: `D:\textures\pack.zip\folder\image.dds`

```cpp
struct VirtualPath
{
    std::filesystem::path hostPath;      // Physical path (file or archive)
    std::wstring archiveInnerPath;       // Path inside archive (empty if regular file)
};
```

### VirtualFileSystem

`VirtualFileSystem` provides static methods for file operations that work transparently with both regular files and archived files.

## Usage Examples

### Creating VirtualPaths

```cpp
// Regular file
VirtualPath regularFile = VirtualPath::FromFilesystem(L"D:\\textures\\image.dds");

// File in archive
VirtualPath archivedFile = VirtualPath::FromArchive(
    L"D:\\textures\\pack.zip",
    L"folder/image.dds"
);

// Parse from display path
auto parsed = VirtualPath::Parse(L"D:\\pack.zip\\folder\\image.dds");
if (parsed)
{
    VirtualPath vpath = *parsed;
}
```

### Checking Path Type

```cpp
VirtualPath vpath = /* ... */;

if (vpath.IsInArchive())
{
    // File is inside an archive
    std::wcout << L"Archive: " << vpath.hostPath << L"\n";
    std::wcout << L"Inner path: " << vpath.archiveInnerPath << L"\n";
}

if (vpath.IsArchiveFile())
{
    // Path points to an archive file itself
}

// Get display path
std::wstring displayPath = vpath.GetDisplayPath();
// Returns: "D:\pack.zip\folder\image.dds"
```

### Listing Directory Contents

```cpp
// List regular directory
VirtualPath dir = VirtualPath::FromFilesystem(L"D:\\textures");
auto entries = VirtualFileSystem::ListDirectory(dir);

// List archive root
VirtualPath archive = VirtualPath::FromFilesystem(L"D:\\textures\\pack.zip");
auto archiveEntries = VirtualFileSystem::ListDirectory(archive);

// List subdirectory in archive
VirtualPath subdir = VirtualPath::FromArchive(L"D:\\pack.zip", L"folder");
auto subdirEntries = VirtualFileSystem::ListDirectory(subdir);

// Process entries
for (const auto& entry : entries)
{
    if (entry.isDirectory)
    {
        std::wcout << L"[DIR] " << entry.path.GetFilename() << L"\n";
    }
    else
    {
        std::wcout << L"[FILE] " << entry.path.GetFilename() 
                   << L" (" << entry.size << L" bytes)\n";
    }
}
```

### Reading Files

```cpp
VirtualPath imagePath = VirtualPath::Parse(L"D:\\pack.zip\\texture.dds").value();

// Read file (works for both regular files and archived files)
std::vector<uint8_t> data = VirtualFileSystem::ReadFile(imagePath);

if (!data.empty())
{
    // Process image data
    // Can pass to ImageCore for decoding
}
```

### Filtering Images

```cpp
auto entries = VirtualFileSystem::ListDirectory(somePath);

// Filter to only images and directories
auto imageEntries = VirtualFileSystem::FilterImageEntries(entries);

// Check if specific file is an image
if (VirtualFileSystem::IsImageFile(vpath))
{
    // It's an image file
}

// Get all supported image extensions
auto extensions = VirtualFileSystem::GetImageExtensions();
// Returns: .dds, .png, .jpg, .jpeg, .bmp, .tga, .tif, .tiff, .gif, .webp, .hdr, .exr, .psd
```

### Recursive Image Search

```cpp
VirtualPath rootPath = VirtualPath::FromFilesystem(L"D:\\textures");

// Get all images recursively
std::vector<VirtualPath> allImages = VirtualFileSystem::GetAllImages(rootPath, true);

for (const auto& imagePath : allImages)
{
    std::wcout << imagePath.GetDisplayPath() << L"\n";
}
```

### Navigation

```cpp
VirtualPath current = VirtualPath::Parse(L"D:\\pack.zip\\folder\\subfolder\\image.dds").value();

// Get parent
VirtualPath parent = current.GetParent();
// Returns: D:\pack.zip\folder\subfolder

VirtualPath grandparent = parent.GetParent();
// Returns: D:\pack.zip\folder

VirtualPath archiveRoot = grandparent.GetParent();
// Returns: D:\pack.zip (the archive itself)

VirtualPath filesystem = archiveRoot.GetParent();
// Returns: D:\ (parent directory of archive)
```

## Architecture

### Path Parsing

The `VirtualPath::Parse()` method intelligently detects archive boundaries:

```
Input:  "D:\textures\pack.zip\folder\image.dds"
         └─────────┬─────────┘ └──────┬──────┘
              hostPath          archiveInnerPath
```

Detection algorithm:
1. Search for archive extensions (`.zip`, `.7z`, `.rar`) in path
2. Check if followed by path separator (`\` or `/`)
3. Split path at archive boundary
4. Normalize inner path separators to forward slashes

### Directory Listing

`VirtualFileSystem::ListDirectory()` handles three cases:

1. **Regular directory**: Uses `std::filesystem::directory_iterator`
2. **Archive root**: Lists top-level entries from archive
3. **Archive subdirectory**: Filters archive entries by path prefix

### File Reading

`VirtualFileSystem::ReadFile()` transparently:
- Reads regular files with `std::ifstream`
- Extracts archived files with `ArchiveReader`

## Integration with ImageCore

VirtualPath integrates seamlessly with ImageCore:

```cpp
VirtualPath imagePath = /* ... */;

// Read image data
std::vector<uint8_t> imageData = VirtualFileSystem::ReadFile(imagePath);

// Create ImageCore request
ImageCore::ImageRequest request;
request.source = imagePath.GetDisplayPath();

ImageCore::DecodeInput input;
input.bytes = imageData;
input.header = std::span(imageData.data(), std::min(imageData.size(), size_t(512)));

// Decode with ImageCore
auto result = ImageCore::ImageDecodeDispatcher::Decode(request, input);
```

## Performance Considerations

### Caching

- Archive file lists are **not cached** by VirtualFileSystem
- Each `ListDirectory()` call opens and scans the archive
- Consider caching results at application level for frequently accessed archives

### Memory Usage

- `ReadFile()` loads entire file into memory
- Suitable for images (typically < 100 MB)
- Not suitable for very large files

### Threading

- VirtualPath is thread-safe (immutable)
- VirtualFileSystem methods are thread-safe
- Multiple threads can read different files simultaneously

## Limitations

### Current Limitations

1. **No write support**: VirtualPath is read-only
2. **No archive modification**: Cannot add/remove files from archives
3. **No nested archives**: Cannot open archives inside archives
4. **No symbolic links**: Symlinks are not followed in archives

### Archive Format Limitations

- **ZIP**: Full support
- **7-Zip**: Full support
- **RAR**: Read-only (no write support in libarchive)

## Future Enhancements

### Planned Features

1. **Archive caching**
   - Cache archive file lists in memory
   - Invalidate on file modification
   - Configurable cache size

2. **Nested archives**
   - Support archives inside archives
   - Example: `pack.zip\inner.7z\texture.dds`

3. **Write support**
   - Create new archives
   - Add files to existing archives
   - Modify archive contents

4. **BA2 format**
   - Bethesda Archive 2 support
   - GNRL and DX10 types
   - Texture-specific optimizations

5. **Streaming**
   - Stream large files without loading fully into memory
   - Useful for video files or very large textures

## Error Handling

VirtualPath methods handle errors gracefully:

```cpp
// Parse returns optional
auto vpath = VirtualPath::Parse(invalidPath);
if (!vpath)
{
    // Invalid path format
}

// ListDirectory returns empty vector on error
auto entries = VirtualFileSystem::ListDirectory(nonexistentPath);
// entries.empty() == true

// ReadFile returns empty vector on error
auto data = VirtualFileSystem::ReadFile(invalidPath);
// data.empty() == true

// Exists() returns false if file doesn't exist
if (vpath.Exists())
{
    // File exists
}
```

## Testing

Test code is available in `test_archive.cpp`:

```cpp
void TestVirtualPath()
{
    // Test path parsing
    auto vp = VirtualPath::Parse(L"D:\\pack.zip\\folder\\image.dds");
    
    // Test directory listing
    auto entries = VirtualFileSystem::ListDirectory(somePath);
    
    // Test image filtering
    auto images = VirtualFileSystem::FilterImageEntries(entries);
}
```

## API Reference

### VirtualPath

| Method | Description |
|--------|-------------|
| `IsInArchive()` | Returns true if path is inside an archive |
| `IsArchiveFile()` | Returns true if path points to an archive |
| `GetDisplayPath()` | Returns full path string |
| `GetFilename()` | Returns just the filename |
| `GetExtension()` | Returns file extension |
| `GetParent()` | Returns parent directory path |
| `Exists()` | Checks if file/directory exists |
| `Parse(path)` | Parses display path string |

### VirtualFileSystem

| Method | Description |
|--------|-------------|
| `ListDirectory(path)` | Lists directory contents |
| `ReadFile(path)` | Reads file into memory |
| `IsImageFile(path)` | Checks if file is an image |
| `GetImageExtensions()` | Returns supported image extensions |
| `FilterImageEntries(entries)` | Filters to images and directories |
| `IsDirectory(path)` | Checks if path is a directory |
| `GetAllImages(path, recursive)` | Gets all images in directory tree |

### VirtualFileEntry

| Field | Type | Description |
|-------|------|-------------|
| `path` | `VirtualPath` | File path |
| `isDirectory` | `bool` | True if directory |
| `size` | `uint64_t` | File size in bytes |
| `modTime` | `time_t` | Last modification time |

## See Also

- [libarchive Integration](libarchive_integration.md)
- [ArchiveReader API](../ArchiveReader.h)
- [ImageCore Integration](../../ImageCore/README.md)
