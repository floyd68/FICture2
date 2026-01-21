#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <span>

// Archive file reader interface for ZIP, 7-Zip, RAR, and BA2 formats
// Uses libarchive for ZIP/7z/RAR, plus custom BA2 parsing

struct ArchiveEntry
{
    std::wstring name;
    uint64_t size;
    uint64_t compressedSize;
    bool isDirectory;
    time_t modTime;
};

class IArchiveReader
{
public:
    virtual ~IArchiveReader() = default;

    // List all entries in the archive
    virtual std::vector<ArchiveEntry> ListEntries() = 0;

    // Extract a file to memory
    virtual std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) = 0;

    // Check if a file exists in the archive
    virtual bool HasEntry(const std::wstring& entryPath) = 0;

    // Get archive format name (e.g., "ZIP", "7-Zip", "RAR")
    virtual std::wstring GetFormatName() const = 0;
};

class ArchiveReaderFactory
{
public:
    // Check if a file is a supported archive format
    static bool IsArchiveFile(const std::filesystem::path& path);

    // Open an archive file
    // Returns nullptr if the file is not a supported archive or cannot be opened
    static std::unique_ptr<IArchiveReader> Open(const std::filesystem::path& path);

    // Check if a filename has an archive extension
    static bool HasArchiveExtension(const std::wstring& filename);

    // Get list of supported extensions (lowercase, with dot)
    static std::vector<std::wstring> GetSupportedExtensions();
};
