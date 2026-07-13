#pragma once

#include <windows.h>

#include <charconv>
#include <cstdint>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace CommonUtil
{
    inline unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    inline float Clamp01(float v)
    {
        if (v < 0.0f)
        {
            return 0.0f;
        }
        if (v > 1.0f)
        {
            return 1.0f;
        }
        return v;
    }

    inline unsigned ToByte255(float v)
    {
        return static_cast<unsigned>(std::floor(Clamp01(v) * 255.0f + 0.5f));
    }

    inline std::wstring ToLower(std::wstring value)
    {
        for (auto& ch : value)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        return value;
    }

    inline std::wstring ToUpper(std::wstring value)
    {
        for (auto& ch : value)
        {
            ch = static_cast<wchar_t>(towupper(ch));
        }
        return value;
    }

    inline std::wstring_view TrimAscii(std::wstring_view s)
    {
        while (!s.empty() && iswspace(s.front()))
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && iswspace(s.back()))
        {
            s.remove_suffix(1);
        }
        return s;
    }

    // Parses an ASCII decimal integer from a wide string (INI / config values).
    inline std::optional<int> TryParseInt(std::wstring_view s)
    {
        s = TrimAscii(s);
        if (s.empty() || s.size() >= 64)
        {
            return std::nullopt;
        }

        char buf[64] {};
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] > 127)
            {
                return std::nullopt;
            }
            buf[i] = static_cast<char>(s[i]);
        }

        int value = 0;
        const char* begin = buf;
        const char* end = buf + s.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc{} || ptr != end)
        {
            return std::nullopt;
        }
        return value;
    }

    // Parses an ASCII floating-point value from a wide string (INI / config values).
    inline std::optional<float> TryParseFloat(std::wstring_view s)
    {
        s = TrimAscii(s);
        if (s.empty() || s.size() >= 64)
        {
            return std::nullopt;
        }

        char buf[64] {};
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] > 127)
            {
                return std::nullopt;
            }
            buf[i] = static_cast<char>(s[i]);
        }

        float value = 0.0f;
        const char* begin = buf;
        const char* end = buf + s.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc{} || ptr != end)
        {
            return std::nullopt;
        }
        return value;
    }

    inline std::wstring NormalizePath(const std::wstring& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::wstring abs;
        DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (needed > 0)
        {
            abs.resize(static_cast<size_t>(needed));
            DWORD written = GetFullPathNameW(path.c_str(), needed, &abs[0], nullptr);
            if (written > 0 && written < needed)
            {
                abs.resize(static_cast<size_t>(written));
            }
            else if (written == 0)
            {
                abs = path;
            }
        }
        else
        {
            abs = path;
        }

        std::wstring out;
        try
        {
            std::filesystem::path fp(abs);
            fp = fp.lexically_normal();
            fp.make_preferred();
            out = fp.wstring();
        }
        catch (...)
        {
            out = abs;
        }

        for (auto& ch : out)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
            ch = static_cast<wchar_t>(towlower(ch));
        }

        return out;
    }

    inline uint64_t Fnv1a64(const std::wstring& s)
    {
        uint64_t h = 14695981039346656037ull;
        for (wchar_t c : s)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    inline std::wstring Hex64(uint64_t v)
    {
        return std::format(L"{:016X}", v);
    }

    inline std::wstring NormalizePathLowerForCompare(const std::wstring& path)
    {
        std::wstring s = ToLower(path);
        for (auto& c : s)
        {
            if (c == L'\\')
            {
                c = L'/';
            }
        }
        while (!s.empty() && s.back() == L'/')
        {
            s.pop_back();
        }
        return s;
    }

    inline std::wstring NormalizePathLowerForCompare(const std::filesystem::path& path)
    {
        return NormalizePathLowerForCompare(path.wstring());
    }

    inline bool PathEqualsInsensitive(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        return NormalizePathLowerForCompare(a) == NormalizePathLowerForCompare(b);
    }

    inline bool PathEqualsInsensitive(
        const std::filesystem::path& hostA,
        const std::wstring& innerA,
        const std::filesystem::path& hostB,
        const std::wstring& innerB)
    {
        return NormalizePathLowerForCompare(hostA) == NormalizePathLowerForCompare(hostB) &&
            NormalizePathLowerForCompare(innerA) == NormalizePathLowerForCompare(innerB);
    }
}
