// IniStore.h — shared Unicode INI reader/writer with cross-process transactions
//
// Canonical copy: FICture2/IniStore.h
// Mirrored copy:  NIFDiff/app/IniStore.h  (keep identical via scripts/sync-ini-store.ps1)
//
// No project-specific dependencies (Windows and standard headers only) and no
// logging — safe to copy between FICture2 and NIFDiff. Contract for callers:
//
// - Reading: Load() reads the whole file once (UTF-16LE with a BOM, UTF-8 with
//   a BOM, or a legacy BOM-less file decoded with the ANSI code page). It takes
//   the file's transaction lock while reading; if the lock is not free within
//   the timeout it reads anyway. A commit swaps in a complete new file, so a
//   reader sees the old or the new file, never a partly written one; opening
//   is retried for a bounded time while the file is momentarily unavailable
//   (during ReplaceFileW the name can briefly be missing or unopenable).
// - Writing: every write is one read/modify/write transaction. Under a
//   cross-process named mutex (LockName(): a hash of the lowercased full path),
//   the file is re-read from disk, the edits are applied, and the whole file is
//   written as UTF-16LE with a BOM to a temporary file next to it, then swapped
//   in with ReplaceFileW (MoveFileExW when the INI does not exist yet). The
//   swap moves the previous file to BackupPath() and deletes it on success; a
//   failed swap moves it back, and a backup that could not be moved back is
//   read by Load() and restored by the next transaction. A legacy
//   ANSI or UTF-8 file is therefore migrated on its first write; a transaction
//   that changes nothing in a UTF-16LE file leaves it untouched.
//   SetString/SetInt/SetFloat write one key; Apply(Batch) and Transact() apply
//   several keys (or a computed read/modify/write) in one transaction.
// - Every write returns a WriteStatus: Committed, LockTimeout (the lock was not
//   acquired within the timeout; nothing was written) or CommitFailed (the file
//   could not be read or replaced; the previous file is kept and the temporary
//   file deleted). Callers log anything but Committed; nothing is retried
//   later, so a failed write is never replayed over newer contents.
// - A lock left by a process or thread that died (WAIT_ABANDONED) counts as
//   acquired: the file is the old or the new one, and it is re-read anyway.
// - Values keep everything after '=' (trimmed). ';' and '#' start a comment
//   only at the start of a line. Comments, blank lines and key order are kept
//   when the file is rewritten; a key that is set again keeps its line.
//
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class IniStore
{
public:
    using KeyMap = std::unordered_map<std::wstring, std::wstring>;
    using SectionMap = std::unordered_map<std::wstring, KeyMap>;

    enum class WriteStatus
    {
        Committed,      // written (or nothing to change)
        LockTimeout,    // lock not acquired in time; nothing written
        CommitFailed,   // file not read or not replaced; previous file kept
    };

    // How long a transaction (or a locked read) waits for another writer.
    static constexpr DWORD kDefaultLockTimeoutMs = 2000;
    // How long opening the INI and swapping in the new file are retried while
    // another handle (an editor, a virus scanner) briefly blocks them.
    static constexpr DWORD kReadRetryMs = 500;
    static constexpr DWORD kCommitRetryMs = 1000;
    // How often Load tries the INI and then its backup when both are missing
    // (a backup can be moved back between the two opens).
    static constexpr int kBackupHandoffAttempts = 3;

    static const char* StatusName(WriteStatus status)
    {
        switch (status)
        {
        case WriteStatus::Committed:
            return "Committed";
        case WriteStatus::LockTimeout:
            return "LockTimeout";
        case WriteStatus::CommitFailed:
            return "CommitFailed";
        }
        return "Unknown";
    }

    static IniStore Load(const std::wstring& path, DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        // Readers prefer the lock but do not depend on it: commits replace the
        // whole file, so an unlocked read still sees a complete file. Without
        // the lock a missing file may be a replace in progress, so it is
        // retried too.
        const Lock lock(path, lockTimeoutMs);
        IniStore ini;
        // A commit that failed (or was cut off) after moving the previous file
        // aside left it under the backup name, which is read then. Without the
        // lock a transaction may move it back between the two opens, so the
        // pair is tried a bounded number of times.
        for (int attempt = 0; attempt < kBackupHandoffAttempts; ++attempt)
        {
            if (ini.Read(path, attempt == 0 && !lock.Acquired()) != ReadResult::Missing)
            {
                break;
            }
            if (HooksForTesting().beforeBackupRead)
            {
                HooksForTesting().beforeBackupRead();
            }
            if (ini.Read(BackupPath(path), false) != ReadResult::Missing)
            {
                break;
            }
        }
        return ini;
    }

    std::wstring GetString(
        std::wstring_view section,
        std::wstring_view key,
        const std::wstring& defaultVal = L"") const
    {
        const auto sIt = m_sections.find(ToLower(section));
        if (sIt == m_sections.end())
        {
            return defaultVal;
        }
        const auto kIt = sIt->second.find(ToLower(key));
        if (kIt == sIt->second.end())
        {
            return defaultVal;
        }
        return kIt->second;
    }

    int GetInt(std::wstring_view section, std::wstring_view key, int defaultVal = 0) const
    {
        const std::wstring s = GetString(section, key);
        if (s.empty())
        {
            return defaultVal;
        }
        wchar_t* end = nullptr;
        const long v = std::wcstol(s.c_str(), &end, 10);
        return (end == s.c_str()) ? defaultVal : static_cast<int>(v);
    }

    float GetFloat(std::wstring_view section, std::wstring_view key, float defaultVal = 0.0f) const
    {
        const std::wstring s = GetString(section, key);
        if (s.empty())
        {
            return defaultVal;
        }
        wchar_t* end = nullptr;
        const double v = std::wcstod(s.c_str(), &end);
        return (end == s.c_str()) ? defaultVal : static_cast<float>(v);
    }

    bool IsLoaded() const
    {
        return m_loaded;
    }

    // In-memory edits, for Transact() callbacks. A value equal to the current
    // one changes nothing.
    void Put(std::wstring_view section, std::wstring_view key, std::wstring_view value)
    {
        const std::wstring keyName = Trim(key);
        const std::wstring keyLower = ToLower(keyName);
        if (keyLower.empty())
        {
            return;
        }
        const std::wstring sectionName = Trim(section);
        const std::wstring sectionLower = ToLower(sectionName);

        // A line break would end the line early; paths never contain one.
        std::wstring text(value);
        for (wchar_t& c : text)
        {
            if (c == L'\r' || c == L'\n')
            {
                c = L' ';
            }
        }
        const std::wstring stored = Trim(text);

        // The line reads use (the last one), else the end of the section's last
        // block (the top of the file for the unnamed section).
        constexpr std::size_t npos = static_cast<std::size_t>(-1);
        std::size_t keyLine = npos;
        std::size_t insertAt = sectionLower.empty() ? 0 : npos;
        std::wstring current;
        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            const Line line = Classify(m_lines[i]);
            if (line.kind == LineKind::Section)
            {
                current = ToLower(line.name);
                if (current == sectionLower)
                {
                    insertAt = i + 1;
                }
            }
            else if (line.kind == LineKind::Key && current == sectionLower)
            {
                insertAt = i + 1;
                if (ToLower(line.name) == keyLower)
                {
                    keyLine = i;
                }
            }
        }

        if (keyLine != npos)
        {
            const Line line = Classify(m_lines[keyLine]);
            if (line.value == stored)
            {
                return;
            }
            m_lines[keyLine] = line.name + L"=" + text;
        }
        else if (insertAt != npos)
        {
            m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(insertAt), keyName + L"=" + text);
        }
        else
        {
            m_lines.push_back(L"[" + sectionName + L"]");
            m_lines.push_back(keyName + L"=" + text);
        }

        m_sections[sectionLower][keyLower] = stored;
        m_dirty = true;
    }

    void PutInt(std::wstring_view section, std::wstring_view key, int value)
    {
        Put(section, key, std::to_wstring(value));
    }

    void PutFloat(std::wstring_view section, std::wstring_view key, float value)
    {
        Put(section, key, std::to_wstring(value));
    }

    // Several key updates applied in one transaction (one locked read, one commit).
    class Batch
    {
    public:
        Batch& Put(std::wstring section, std::wstring key, std::wstring value)
        {
            m_edits.push_back({ std::move(section), std::move(key), std::move(value) });
            return *this;
        }

        Batch& PutInt(std::wstring section, std::wstring key, int value)
        {
            return Put(std::move(section), std::move(key), std::to_wstring(value));
        }

        Batch& PutFloat(std::wstring section, std::wstring key, float value)
        {
            return Put(std::move(section), std::move(key), std::to_wstring(value));
        }

        bool Empty() const
        {
            return m_edits.empty();
        }

        void ApplyTo(IniStore& ini) const
        {
            for (const Edit& edit : m_edits)
            {
                ini.Put(edit.section, edit.key, edit.value);
            }
        }

    private:
        struct Edit
        {
            std::wstring section;
            std::wstring key;
            std::wstring value;
        };
        std::vector<Edit> m_edits;
    };

    // One read/modify/write transaction: under the file's lock, `edit` gets the
    // file as it is on disk now and changes it with Put*; the result is
    // committed if anything changed (or the file still needs its Unicode
    // migration). Keep `edit` short: other writers wait for it.
    static WriteStatus Transact(
        const std::wstring& path,
        const std::function<void(IniStore&)>& edit,
        DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        const Lock lock(path, lockTimeoutMs);
        if (!lock.Acquired())
        {
            return WriteStatus::LockTimeout;
        }

        if (!RestoreBackup(path))
        {
            // The previous file is still only under the backup name.
            return WriteStatus::CommitFailed;
        }

        IniStore ini;
        const ReadResult read = ini.Read(path, false);
        if (read == ReadResult::Failed)
        {
            // Never replace a file that could not be read.
            return WriteStatus::CommitFailed;
        }

        if (edit)
        {
            edit(ini);
        }

        const bool needsMigration = (read == ReadResult::Read) && (ini.m_encoding != Encoding::Utf16);
        if (!ini.m_dirty && !needsMigration)
        {
            return WriteStatus::Committed;
        }
        return ini.Commit(path) ? WriteStatus::Committed : WriteStatus::CommitFailed;
    }

    static WriteStatus Apply(
        const std::wstring& path,
        const Batch& batch,
        DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        return Transact(path, [&batch](IniStore& ini) { batch.ApplyTo(ini); }, lockTimeoutMs);
    }

    // Rewrites a legacy ANSI/UTF-8 INI as UTF-16LE with a BOM. A file that is
    // already UTF-16LE (or missing) is left untouched.
    // The name the previous file is moved to while a commit swaps in the new
    // one (ReplaceFileW's backup). It exists after a commit only when that
    // commit failed and could not move it back; Load reads it then, and the
    // next transaction restores it.
    static std::wstring BackupPath(const std::wstring& path)
    {
        return path + L".replace-backup";
    }

    // Whether the INI exists, or only its backup after an interrupted commit
    // (Load reads it and the next transaction restores it): either way the
    // configuration exists and must not be replaced by first-run defaults.
    static bool FileOrBackupExists(const std::wstring& path)
    {
        return Exists(path) || Exists(BackupPath(path));
    }

    // Tests only (null: normal behavior): `replace` and `move` replace the
    // file-swap calls of a commit, to inject the failures ReplaceFileW and
    // MoveFileExW document; `beforeBackupRead` runs in Load between a missing
    // INI and the read of its backup.
    struct TestHooks
    {
        BOOL (WINAPI* replace)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID) = nullptr;
        BOOL (WINAPI* move)(LPCWSTR, LPCWSTR, DWORD) = nullptr;
        void (*beforeBackupRead)() = nullptr;
    };
    static TestHooks& HooksForTesting()
    {
        static TestHooks hooks;
        return hooks;
    }

    static WriteStatus MigrateToUnicode(const std::wstring& path, DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        return Transact(path, nullptr, lockTimeoutMs);
    }

    static WriteStatus SetString(
        const std::wstring& path,
        const std::wstring& section,
        const std::wstring& key,
        const std::wstring& value,
        DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        return Transact(path, [&](IniStore& ini) { ini.Put(section, key, value); }, lockTimeoutMs);
    }

    static WriteStatus SetInt(
        const std::wstring& path,
        const std::wstring& section,
        const std::wstring& key,
        int value,
        DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        return SetString(path, section, key, std::to_wstring(value), lockTimeoutMs);
    }

    static WriteStatus SetFloat(
        const std::wstring& path,
        const std::wstring& section,
        const std::wstring& key,
        float value,
        DWORD lockTimeoutMs = kDefaultLockTimeoutMs)
    {
        return SetString(path, section, key, std::to_wstring(value), lockTimeoutMs);
    }

    // The named mutex every reader and writer of `path` uses:
    // Local\IniStore-<FNV-1a 64 of the lowercased GetFullPathNameW path>.
    static std::wstring LockName(const std::wstring& path)
    {
        std::wstring full = path;
        const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (needed > 0)
        {
            std::wstring buffer(needed, L'\0');
            const DWORD written = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
            if (written > 0 && written < needed)
            {
                buffer.resize(written);
                full = std::move(buffer);
            }
        }

        if (!full.empty())
        {
            std::wstring lower(full.size(), L'\0');
            const int lowered = LCMapStringEx(
                LOCALE_NAME_INVARIANT,
                LCMAP_LOWERCASE,
                full.c_str(),
                static_cast<int>(full.size()),
                lower.data(),
                static_cast<int>(lower.size()),
                nullptr,
                nullptr,
                0);
            if (lowered == static_cast<int>(full.size()))
            {
                full = std::move(lower);
            }
        }

        std::uint64_t hash = 14695981039346656037ull;
        for (const wchar_t c : full)
        {
            hash ^= static_cast<std::uint16_t>(c);
            hash *= 1099511628211ull;
        }

        wchar_t hex[17] {};
        for (int i = 15; i >= 0; --i)
        {
            hex[i] = L"0123456789abcdef"[hash & 0xF];
            hash >>= 4;
        }
        return std::wstring(L"Local\\IniStore-") + hex;
    }

    // Pipe-joined multi-value helpers (RecentFiles, OverrideFolders, …).
    static std::vector<std::wstring> SplitPipeList(std::wstring_view s)
    {
        std::vector<std::wstring> out;
        std::size_t start = 0;
        while (start < s.size())
        {
            std::size_t bar = s.find(L'|', start);
            if (bar == std::wstring_view::npos)
            {
                bar = s.size();
            }
            std::wstring_view part = s.substr(start, bar - start);
            while (!part.empty() && std::iswspace(part.front()))
            {
                part.remove_prefix(1);
            }
            while (!part.empty() && std::iswspace(part.back()))
            {
                part.remove_suffix(1);
            }
            if (!part.empty())
            {
                out.emplace_back(part);
            }
            start = bar + 1;
        }
        return out;
    }

    static std::wstring JoinPipeList(const std::vector<std::wstring>& parts)
    {
        std::wstring out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                out += L'|';
            }
            out += parts[i];
        }
        return out;
    }

private:
    enum class Encoding
    {
        None,       // no file was read
        Utf16,      // UTF-16LE with a BOM (what Commit writes)
        Utf8,       // UTF-8 with a BOM
        Ansi,       // no BOM: the active ANSI code page
    };

    enum class ReadResult
    {
        Read,
        Missing,
        Failed,
    };

    enum class LineKind
    {
        Other,      // blank, comment or unparsable
        Section,
        Key,
    };

    struct Line
    {
        LineKind kind = LineKind::Other;
        std::wstring name;      // section name or key, trimmed, original case
        std::wstring value;     // trimmed value of a key line
    };

    // The file's named mutex, held for the lifetime of the object.
    class Lock
    {
    public:
        Lock(const std::wstring& path, DWORD timeoutMs)
        {
            m_mutex = CreateMutexW(nullptr, FALSE, LockName(path).c_str());
            if (m_mutex == nullptr)
            {
                return;
            }
            const DWORD wait = WaitForSingleObject(m_mutex, timeoutMs);
            m_acquired = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
        }

        ~Lock()
        {
            if (m_acquired)
            {
                (void)ReleaseMutex(m_mutex);
            }
            if (m_mutex != nullptr)
            {
                CloseHandle(m_mutex);
            }
        }

        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

        bool Acquired() const
        {
            return m_acquired;
        }

    private:
        HANDLE m_mutex = nullptr;
        bool m_acquired = false;
    };

    std::vector<std::wstring> m_lines;
    SectionMap m_sections;
    Encoding m_encoding = Encoding::None;
    bool m_loaded = false;
    bool m_dirty = false;

    static std::wstring ToLower(std::wstring_view s)
    {
        std::wstring out(s);
        for (wchar_t& c : out)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return out;
    }

    static std::wstring Trim(std::wstring_view s)
    {
        std::size_t start = 0;
        while (start < s.size() && std::iswspace(s[start]))
        {
            ++start;
        }
        std::size_t end = s.size();
        while (end > start && std::iswspace(s[end - 1]))
        {
            --end;
        }
        return std::wstring(s.substr(start, end - start));
    }

    static Line Classify(std::wstring_view line)
    {
        Line out;
        std::size_t start = 0;
        while (start < line.size() && std::iswspace(line[start]))
        {
            ++start;
        }
        if (start == line.size())
        {
            return out;
        }

        const wchar_t first = line[start];
        if (first == L';' || first == L'#')
        {
            return out;
        }

        if (first == L'[')
        {
            const std::size_t close = line.find(L']', start + 1);
            if (close != std::wstring_view::npos)
            {
                out.kind = LineKind::Section;
                out.name = Trim(line.substr(start + 1, close - start - 1));
            }
            return out;
        }

        const std::size_t eq = line.find(L'=', start);
        if (eq != std::wstring_view::npos)
        {
            out.name = Trim(line.substr(start, eq - start));
            if (!out.name.empty())
            {
                out.kind = LineKind::Key;
                // The whole value after '=': ';' is legal in file and folder
                // names, so it starts a comment only at the start of a line.
                out.value = Trim(line.substr(eq + 1));
            }
        }
        return out;
    }

    static bool IsTransientError(DWORD error)
    {
        return error == ERROR_SHARING_VIOLATION ||
            error == ERROR_LOCK_VIOLATION ||
            error == ERROR_ACCESS_DENIED ||                 // also a pending delete
            error == ERROR_UNABLE_TO_REMOVE_REPLACED ||     // ReplaceFileW: both files unchanged
            error == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||    // ReplaceFileW with a backup: both files unchanged
            error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;    // ReplaceFileW: previous file now the backup; retried as a move
    }

    static bool Exists(const std::wstring& path)
    {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static BOOL SwapReplace(const std::wstring& path, const std::wstring& replacement, const std::wstring& backup)
    {
        const auto replace = HooksForTesting().replace ? HooksForTesting().replace : &ReplaceFileW;
        return replace(path.c_str(), replacement.c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
    }

    static BOOL SwapMove(const std::wstring& from, const std::wstring& to)
    {
        const auto move = HooksForTesting().move ? HooksForTesting().move : &MoveFileExW;
        return move(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    // Under the lock: when the INI is missing but its backup exists (a commit
    // failed or was cut off after moving the previous file aside), moves the
    // backup back, retried for a bounded time. False when it is still only
    // the backup.
    static bool RestoreBackup(const std::wstring& path)
    {
        const std::wstring backup = BackupPath(path);
        const ULONGLONG deadline = GetTickCount64() + kCommitRetryMs;
        while (!Exists(path) && Exists(backup))
        {
            if (SwapMove(backup, path) || GetTickCount64() >= deadline)
            {
                break;
            }
            Sleep(10);
        }
        return Exists(path) || !Exists(backup);
    }

    static bool ReadAll(HANDLE file, std::vector<std::uint8_t>& out)
    {
        LARGE_INTEGER size {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64ll << 20))
        {
            return false;
        }
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t done = 0;
        while (done < out.size())
        {
            DWORD got = 0;
            if (!ReadFile(file, out.data() + done, static_cast<DWORD>(out.size() - done), &got, nullptr))
            {
                return false;
            }
            if (got == 0)
            {
                break;
            }
            done += got;
        }
        out.resize(done);
        return true;
    }

    ReadResult Read(const std::wstring& path, bool retryMissing)
    {
        std::vector<std::uint8_t> raw;
        const ULONGLONG deadline = GetTickCount64() + kReadRetryMs;
        for (;;)
        {
            const HANDLE file = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            DWORD error = ERROR_SUCCESS;
            if (file != INVALID_HANDLE_VALUE)
            {
                const bool ok = ReadAll(file, raw);
                error = ok ? ERROR_SUCCESS : GetLastError();
                CloseHandle(file);
                if (ok)
                {
                    break;
                }
            }
            else
            {
                error = GetLastError();
                const bool missing = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
                if (missing && (!retryMissing || GetTickCount64() >= deadline))
                {
                    return ReadResult::Missing;
                }
                if (missing)
                {
                    Sleep(10);
                    continue;
                }
            }
            if (!IsTransientError(error) || GetTickCount64() >= deadline)
            {
                return ReadResult::Failed;
            }
            Sleep(10);
        }

        std::wstring content;
        if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
        {
            m_encoding = Encoding::Utf16;
            content.assign(
                reinterpret_cast<const wchar_t*>(raw.data() + 2),
                (raw.size() - 2) / sizeof(wchar_t));
        }
        else
        {
            std::size_t skip = 0;
            UINT codePage = CP_ACP;
            m_encoding = Encoding::Ansi;
            if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
            {
                skip = 3;
                codePage = CP_UTF8;
                m_encoding = Encoding::Utf8;
            }
            const char* bytes = reinterpret_cast<const char*>(raw.data() + skip);
            const int byteCount = static_cast<int>(raw.size() - skip);
            const int wlen = (byteCount > 0)
                ? MultiByteToWideChar(codePage, 0, bytes, byteCount, nullptr, 0)
                : 0;
            if (wlen > 0)
            {
                content.resize(static_cast<std::size_t>(wlen));
                MultiByteToWideChar(codePage, 0, bytes, byteCount, content.data(), wlen);
            }
        }

        Parse(content);
        m_loaded = !raw.empty();
        return ReadResult::Read;
    }

    void Parse(const std::wstring& content)
    {
        m_lines.clear();
        m_sections.clear();

        std::wstring currentSection;
        std::size_t pos = 0;
        const std::size_t len = content.size();

        while (pos < len)
        {
            std::size_t eol = pos;
            while (eol < len && content[eol] != L'\n' && content[eol] != L'\r')
            {
                ++eol;
            }

            m_lines.emplace_back(content.data() + pos, eol - pos);

            pos = eol;
            if (pos < len && content[pos] == L'\r')
            {
                ++pos;
            }
            if (pos < len && content[pos] == L'\n')
            {
                ++pos;
            }

            const Line line = Classify(m_lines.back());
            if (line.kind == LineKind::Section)
            {
                currentSection = ToLower(line.name);
            }
            else if (line.kind == LineKind::Key)
            {
                m_sections[currentSection][ToLower(line.name)] = line.value;
            }
        }
    }

    // Writes the document to a temporary file next to `path` and swaps it in.
    // On failure the previous file is kept and the temporary file deleted.
    bool Commit(const std::wstring& path) const
    {
        std::wstring text;
        for (const std::wstring& line : m_lines)
        {
            text += line;
            text += L"\r\n";
        }
        std::vector<std::uint8_t> bytes { 0xFF, 0xFE };
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        bytes.insert(bytes.end(), data, data + text.size() * sizeof(wchar_t));

        std::wstring tempPath;
        HANDLE temp = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 16 && temp == INVALID_HANDLE_VALUE; ++attempt)
        {
            tempPath = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(attempt) + L".tmp";
            temp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (temp == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
            {
                return false;
            }
        }
        if (temp == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD written = 0;
        const bool wrote =
            WriteFile(temp, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() &&
            FlushFileBuffers(temp);
        CloseHandle(temp);

        // ReplaceFileW moves the previous file to the backup name, so a swap
        // that fails halfway (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) can be undone
        // instead of leaving no INI. A leftover backup next to an existing INI
        // is stale (Transact restored any needed one before reading).
        const std::wstring backup = BackupPath(path);
        bool committed = false;
        if (wrote)
        {
            if (Exists(path))
            {
                (void)DeleteFileW(backup.c_str());
            }
            const ULONGLONG deadline = GetTickCount64() + kCommitRetryMs;
            for (;;)
            {
                committed = (Exists(path) ? SwapReplace(path, tempPath, backup) : SwapMove(tempPath, path)) != FALSE;
                if (committed)
                {
                    break;
                }
                // A target that disappeared meanwhile (or was moved to the
                // backup) is retried as a move.
                const DWORD error = GetLastError();
                if ((!IsTransientError(error) && error != ERROR_FILE_NOT_FOUND) || GetTickCount64() >= deadline)
                {
                    break;
                }
                Sleep(10);
            }
        }

        if (committed)
        {
            (void)DeleteFileW(backup.c_str());
        }
        else
        {
            // Keep the previous file: move it back if the swap moved it aside.
            // If even that fails it stays under the backup name, which Load
            // reads and the next transaction restores; never delete it here.
            (void)RestoreBackup(path);
            (void)DeleteFileW(tempPath.c_str());
        }
        return committed;
    }
};
