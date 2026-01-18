#include "ArchiveReader.h"

#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_map>
#include <Windows.h>

namespace
{
    std::wstring Utf8ToWide(const char* utf8Str)
    {
        if (!utf8Str || !*utf8Str)
            return {};

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str, -1, nullptr, 0);
        if (wideLen <= 0)
            return {};

        std::wstring result(wideLen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Str, -1, result.data(), wideLen);
        return result;
    }

    std::string WideToUtf8(const std::wstring& wideStr)
    {
        if (wideStr.empty())
            return {};

        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0)
            return {};

        std::string result(utf8Len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, result.data(), utf8Len, nullptr, nullptr);
        return result;
    }

    std::wstring ToLower(const std::wstring& str)
    {
        std::wstring result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return result;
    }
}

class LibArchiveReader : public IArchiveReader
{
    std::filesystem::path m_path;
    std::vector<ArchiveEntry> m_entries;
    std::unordered_map<std::wstring, size_t> m_entryMap;
    std::wstring m_formatName;

public:
    LibArchiveReader(const std::filesystem::path& path)
        : m_path(path)
    {
        OutputDebugStringW((L"[LibArchiveReader] Opening: " + path.wstring() + L"\n").c_str());
        ScanArchive();
        OutputDebugStringW((L"[LibArchiveReader] Scanned, found " + std::to_wstring(m_entries.size()) + L" entries\n").c_str());
    }

    std::vector<ArchiveEntry> ListEntries() override
    {
        return m_entries;
    }

    std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) override
    {
        OutputDebugStringW((L"[LibArchiveReader] ExtractToMemory: " + entryPath + L"\n").c_str());

        auto it = m_entryMap.find(ToLower(entryPath));
        if (it == m_entryMap.end())
        {
            OutputDebugStringW(L"[LibArchiveReader] Entry not found in map\n");
            return {};
        }

        const ArchiveEntry& entry = m_entries[it->second];
        if (entry.isDirectory)
        {
            return {};
        }

        // Open archive for extraction
        struct archive* a = archive_read_new();
        if (!a)
        {
            return {};
        }

        // Enable only the formats we need
        archive_read_support_format_zip(a);
        archive_read_support_format_7zip(a);
        archive_read_support_format_rar(a);

        // Enable only essential compression filters for ZIP/7z/RAR
        archive_read_support_filter_none(a);    // Uncompressed
        archive_read_support_filter_gzip(a);    // ZIP deflate/gzip
        archive_read_support_filter_lzma(a);    // 7z LZMA
        archive_read_support_filter_xz(a);      // 7z XZ

        std::string pathUtf8 = WideToUtf8(m_path.wstring());
        int r = archive_read_open_filename(a, pathUtf8.c_str(), 10240);
        if (r != ARCHIVE_OK)
        {
            archive_read_free(a);
            return {};
        }

        std::vector<uint8_t> result;
        struct archive_entry* archiveEntry;
        // Use the original entry name (with correct case) from m_entries
        std::string targetUtf8 = WideToUtf8(entry.name);

        while (archive_read_next_header(a, &archiveEntry) == ARCHIVE_OK)
        {
            const char* name = archive_entry_pathname(archiveEntry);
            if (!name)
                continue;

            if (std::string(name) == targetUtf8)
            {
                // Found the entry, extract it
                la_int64_t size = archive_entry_size(archiveEntry);
                
                if (size > 0 && size < 1024 * 1024 * 1024) // Max 1GB
                {
                    result.resize(static_cast<size_t>(size));
                    la_ssize_t bytesRead = archive_read_data(a, result.data(), result.size());
                    
                    if (bytesRead != size)
                    {
                        result.clear();
                    }
                }
                break;
            }
        }

        archive_read_free(a);
        return result;
    }

    bool HasEntry(const std::wstring& entryPath) override
    {
        return m_entryMap.find(ToLower(entryPath)) != m_entryMap.end();
    }

    std::wstring GetFormatName() const override
    {
        return m_formatName;
    }

private:
    void ScanArchive()
    {
        struct archive* a = archive_read_new();
        if (!a)
        {
            return;
        }

        // Enable only the formats we need
        archive_read_support_format_zip(a);
        archive_read_support_format_7zip(a);
        archive_read_support_format_rar(a);

        // Enable only essential compression filters for ZIP/7z/RAR
        archive_read_support_filter_none(a);    // Uncompressed
        archive_read_support_filter_gzip(a);    // ZIP deflate/gzip
        archive_read_support_filter_lzma(a);    // 7z LZMA
        archive_read_support_filter_xz(a);      // 7z XZ

        std::string pathUtf8 = WideToUtf8(m_path.wstring());
        int r = archive_read_open_filename(a, pathUtf8.c_str(), 10240);
        if (r != ARCHIVE_OK)
        {
            const char* errMsg = archive_error_string(a);
            std::wstring errWide = errMsg ? Utf8ToWide(errMsg) : L"Unknown error";
            OutputDebugStringW((L"[LibArchiveReader] ERROR: Failed to open " + m_path.wstring() + L": " + errWide + L"\n").c_str());
            archive_read_free(a);
            return;
        }

        // Get format name
        const char* formatName = archive_format_name(a);
        if (formatName)
        {
            m_formatName = Utf8ToWide(formatName);
        }

        struct archive_entry* entry;
        while (true)
        {
            int r = archive_read_next_header(a, &entry);
            if (r == ARCHIVE_EOF)
            {
                break;
            }
            if (r != ARCHIVE_OK)
            {
                const char* errMsg = archive_error_string(a);
                std::wstring errWide = errMsg ? Utf8ToWide(errMsg) : L"Unknown error";
                OutputDebugStringW((L"[LibArchiveReader] ERROR reading header: " + errWide + L"\n").c_str());
                break;
            }

            const char* name = archive_entry_pathname(entry);
            if (!name)
            {
                continue;
            }

            ArchiveEntry archEntry;
            archEntry.name = Utf8ToWide(name);
            archEntry.size = archive_entry_size(entry);
            archEntry.compressedSize = 0; // libarchive doesn't always provide this
            archEntry.isDirectory = (archive_entry_filetype(entry) == AE_IFDIR);
            archEntry.modTime = archive_entry_mtime(entry);

            size_t index = m_entries.size();
            m_entries.push_back(archEntry);
            m_entryMap[ToLower(archEntry.name)] = index;
        }

        archive_read_free(a);
    }
};

// ArchiveReaderFactory implementation

bool ArchiveReaderFactory::IsArchiveFile(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
        return false;

    return HasArchiveExtension(path.filename().wstring());
}

std::unique_ptr<IArchiveReader> ArchiveReaderFactory::Open(const std::filesystem::path& path)
{
    if (!IsArchiveFile(path))
        return nullptr;

    try
    {
        auto reader = std::make_unique<LibArchiveReader>(path);
        if (reader->ListEntries().empty())
            return nullptr;

        return reader;
    }
    catch (...)
    {
        return nullptr;
    }
}

bool ArchiveReaderFactory::HasArchiveExtension(const std::wstring& filename)
{
    auto ext = std::filesystem::path(filename).extension().wstring();
    if (ext.empty())
        return false;

    ext = ToLower(ext);

    static const std::vector<std::wstring> supportedExts = {
        L".zip", L".7z", L".rar"
    };

    return std::find(supportedExts.begin(), supportedExts.end(), ext) != supportedExts.end();
}

std::vector<std::wstring> ArchiveReaderFactory::GetSupportedExtensions()
{
    return { L".zip", L".7z", L".rar" };
}
