// AppIpc.h - single-instance IPC for FICture2.
//
// Protocol v2 (Waiting ack + staged client hard timeouts):
//   client -> server : u32 protocol version, u32 payload byte count,
//                      UTF-16 null-terminated path
//   server -> client : u32 Waiting ack (sent immediately on receipt,
//                      repeated while the request is pending),
//                      then u32 Decision
//
// The server-side decision is designed to be immediate: the onRequest
// callback only checks the file name against the primary instance's
// open-documents snapshot and queues the path (see IpcOpenRequest.h) - it
// never waits for the UI to load anything. The Waiting acks therefore only
// bridge server hiccups, not multi-second image loads.
//
// The client never trusts the server unconditionally - staged hard bounds:
//   1. No single-instance mutex -> no IPC at all, run standalone.
//   2. Pipe connect + first server message: 500ms each.
//   3. After each Waiting ack: 2s silence allowed, AND at most 3 Waiting
//      acks total. Tripping either bound means the server froze or is
//      wedged mid-decision - the client stops waiting and opens alone.
#pragma once

#include <functional>
#include <string>

namespace AppIpc
{
    enum class Decision : unsigned long
    {
        Ignore = 0,         // request not handled - caller should run as a new instance
        CompareStarted = 1, // running instance accepted/queued the file - caller should exit
    };

    // Starts a lightweight named-pipe server in a background thread.
    // Incoming requests contain a single UTF-16 file path. The callback runs
    // on a per-client worker thread and is expected to decide quickly
    // (queue-or-decline). Returns whether the request was handled.
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest);

    // Tries to send a single file path to an existing server and receive a
    // decision. Returns false if the server is not available. Bounded by the
    // staged client timeouts above.
    bool TrySendPath(const std::wstring& path, Decision& outDecision);
}
