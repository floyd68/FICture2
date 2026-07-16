// IpcOpenRequest.h - shared gate + queue between IPC worker threads and the
// UI thread for single-instance "open this file" forwarding.
//
// Earlier shape (IpcCompareRequest): the IPC worker marshaled each request
// onto the UI thread and PARKED its client on Waiting acks until the UI had
// actually started compare - so back-to-back Explorer opens serialized behind
// load/work and tripped post-ack timeouts. Now the accept/reject decision
// (same-file-name gate + pane capacity) is taken on the IPC worker against
// this mutex-protected snapshot, the accepted path is queued, and the client
// is answered immediately; the UI drains the queue when it can.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Broadcast command id carried through FD2D::Backplate::WM_FD2D_BROADCAST.
// No payload - tells the UI to drain the IPC open queue.
inline constexpr UINT CMD_FIC2_IPC_OPEN = WM_APP + 0x7A12;

struct IpcOpenQueue
{
    // Forward-into-a-pane exists for comparing the SAME file name from
    // different folders side by side. An incoming path is accepted only when
    // its file name matches a document already open (or already queued); an
    // empty, fully started viewer accepts any file. Anything else belongs in
    // its own window.
    //
    // Called on an IPC worker thread. Returns whether the path was queued
    // (client is told CompareStarted and exits) or declined.
    bool TryEnqueue(const std::wstring& path, std::size_t maxPanes)
    {
        const std::wstring name = FileNameLower(path);
        if (name.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (shuttingDown)
        {
            return false;
        }

        bool match = false;
        if (openNamesLower.empty() && pending.empty())
        {
            // uiReady distinguishes "empty because nothing is loaded" from
            // "empty because the primary instance is still booting / about to
            // restore a session" - during that window unmatchable requests are
            // declined rather than parked.
            match = uiReady;
        }
        else
        {
            for (const std::wstring& open : openNamesLower)
            {
                match = match || (open == name);
            }
            for (const std::wstring& queuedPath : pending)
            {
                match = match || (FileNameLower(queuedPath) == name);
            }
        }
        if (!match)
        {
            return false;
        }

        if (loadedCount + pending.size() >= maxPanes)
        {
            return false;
        }

        pending.push_back(path);
        return true;
    }

    // Seed with the primary instance's own command-line file(s) so concurrent
    // Explorer multi-select forwards can match before the first document
    // snapshot lands.
    void SeedExpected(const std::vector<std::wstring>& paths)
    {
        std::lock_guard<std::mutex> lock(mutex);
        openNamesLower.clear();
        for (const std::wstring& p : paths)
        {
            std::wstring name = FileNameLower(p);
            if (!name.empty())
            {
                openNamesLower.push_back(std::move(name));
            }
        }
        loadedCount = openNamesLower.size();
    }

    void MarkUiReady()
    {
        std::lock_guard<std::mutex> lock(mutex);
        uiReady = true;
    }

    void MarkShuttingDown()
    {
        std::lock_guard<std::mutex> lock(mutex);
        shuttingDown = true;
    }

    static std::wstring FileNameLower(const std::wstring& path)
    {
        std::wstring name = std::filesystem::path(path).filename().wstring();
        for (wchar_t& c : name)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return name;
    }

    std::mutex mutex;
    std::vector<std::wstring> openNamesLower;
    std::size_t loadedCount = 0;
    std::deque<std::wstring> pending;
    bool uiReady = false;
    bool shuttingDown = false;
};
