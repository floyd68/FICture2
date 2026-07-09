#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// SimpleIniFile — single-pass, in-memory INI reader
//
// Reads the entire INI file from disk ONCE. Use this instead of calling
// GetPrivateProfileStringW / GetPrivateProfileIntW, which re-open and
// re-parse the file on every call (typically 50–100 ms on cold HDD/SSD).
//
// Supports:
//   - ANSI  files (common when created by the W APIs on older Windows)
//   - UTF-16 LE files with BOM (written by WritePrivateProfileStringW on Win10+)
//   - Sections: [SectionName]
//   - Key=Value pairs (leading/trailing whitespace trimmed)
//   - Inline comments beginning with ';'
//   - Whole-line comments beginning with ';' or '#'
//
// Writes (WritePrivateProfileStringW) are left unchanged; they are called
// only at shutdown / save points and do not contribute to startup latency.
// ---------------------------------------------------------------------------
class SimpleIniFile
{
public:
    using KeyMap     = std::unordered_map<std::wstring, std::wstring>;
    using SectionMap = std::unordered_map<std::wstring, KeyMap>;

    // Attempt to load and parse the file. Check IsLoaded() before using.
    static SimpleIniFile Load(const std::wstring& path)
    {
        SimpleIniFile ini;

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr)
        {
            return ini;
        }

        // Read entire file into a byte buffer.
        (void)fseek(fp, 0, SEEK_END);
        const long flen = ftell(fp);
        (void)fseek(fp, 0, SEEK_SET);

        if (flen <= 0)
        {
            fclose(fp);
            return ini;
        }

        std::vector<uint8_t> raw(static_cast<size_t>(flen));
        const size_t nRead = fread(raw.data(), 1, raw.size(), fp);
        fclose(fp);

        if (nRead == 0)
        {
            return ini;
        }

        // Decode to a single wstring, detecting UTF-16 LE BOM.
        std::wstring content;
        if (nRead >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
        {
            // UTF-16 LE with BOM — reinterpret bytes directly.
            const size_t wchars = (nRead - 2) / sizeof(wchar_t);
            content.assign(
                reinterpret_cast<const wchar_t*>(raw.data() + 2),
                wchars);
        }
        else
        {
            // ANSI — convert using the current ANSI code page.
            const int wlen = MultiByteToWideChar(
                CP_ACP, 0,
                reinterpret_cast<const char*>(raw.data()),
                static_cast<int>(nRead),
                nullptr, 0);
            if (wlen > 0)
            {
                content.resize(static_cast<size_t>(wlen));
                MultiByteToWideChar(
                    CP_ACP, 0,
                    reinterpret_cast<const char*>(raw.data()),
                    static_cast<int>(nRead),
                    content.data(), wlen);
            }
        }

        ini.Parse(content);
        ini.m_loaded = true;
        return ini;
    }

    // Returns the value string, or defaultVal if section/key not found.
    std::wstring GetString(
        const std::wstring& section,
        const std::wstring& key,
        const std::wstring& defaultVal = L"") const
    {
        const auto sIt = m_sections.find(Lower(section));
        if (sIt == m_sections.end())
        {
            return defaultVal;
        }
        const auto kIt = sIt->second.find(Lower(key));
        if (kIt == sIt->second.end())
        {
            return defaultVal;
        }
        return kIt->second;
    }

    // Returns the value as int, or defaultVal if section/key not found or
    // the value cannot be converted.
    int GetInt(
        const std::wstring& section,
        const std::wstring& key,
        int defaultVal = 0) const
    {
        const std::wstring s = GetString(section, key);
        if (s.empty())
        {
            return defaultVal;
        }
        try
        {
            return std::stoi(s);
        }
        catch (...)
        {
            return defaultVal;
        }
    }

    // Returns the value as float, or defaultVal if not found.
    float GetFloat(
        const std::wstring& section,
        const std::wstring& key,
        float defaultVal = 0.0f) const
    {
        const std::wstring s = GetString(section, key);
        if (s.empty())
        {
            return defaultVal;
        }
        return static_cast<float>(_wtof(s.c_str()));
    }

    bool IsLoaded() const { return m_loaded; }

private:
    SectionMap m_sections;
    bool m_loaded = false;

    // Parse a decoded wide-character buffer.
    void Parse(const std::wstring& content)
    {
        std::wstring currentSection;
        size_t pos = 0;
        const size_t len = content.size();

        while (pos < len)
        {
            // Find end of line (\r\n or \n or \r).
            size_t eol = pos;
            while (eol < len && content[eol] != L'\n' && content[eol] != L'\r')
            {
                ++eol;
            }

            // Line = content[pos .. eol)
            const std::wstring_view line(content.data() + pos, eol - pos);

            // Advance pos past the line terminator(s).
            pos = eol;
            if (pos < len && content[pos] == L'\r') { ++pos; }
            if (pos < len && content[pos] == L'\n') { ++pos; }

            // Trim leading whitespace.
            size_t start = 0;
            while (start < line.size() && iswspace(line[start])) { ++start; }
            if (start == line.size()) { continue; }

            const wchar_t first = line[start];

            // Comment line.
            if (first == L';' || first == L'#') { continue; }

            // Section header.
            if (first == L'[')
            {
                const size_t close = line.find(L']', start + 1);
                if (close != std::wstring_view::npos)
                {
                    currentSection = Lower(Trim(std::wstring(line.substr(start + 1, close - start - 1))));
                }
                continue;
            }

            // Key = Value
            const size_t eq = line.find(L'=', start);
            if (eq != std::wstring_view::npos)
            {
                std::wstring key = Lower(Trim(std::wstring(line.substr(start, eq - start))));
                std::wstring val = Trim(std::wstring(line.substr(eq + 1)));

                // Strip inline comment.
                const size_t semi = val.find(L';');
                if (semi != std::wstring::npos)
                {
                    val = Trim(val.substr(0, semi));
                }

                if (!key.empty())
                {
                    m_sections[currentSection][std::move(key)] = std::move(val);
                }
            }
        }
    }

    static std::wstring Trim(std::wstring s)
    {
        size_t start = 0;
        while (start < s.size() && iswspace(s[start])) { ++start; }
        size_t end = s.size();
        while (end > start && iswspace(s[end - 1])) { --end; }
        return s.substr(start, end - start);
    }

    static std::wstring Lower(std::wstring s)
    {
        for (auto& c : s) { c = static_cast<wchar_t>(towlower(c)); }
        return s;
    }
};
