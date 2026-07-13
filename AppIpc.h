// AppIpc.h - single-instance IPC for FICture2.
//
// Protocol v2 (Waiting ack + staged client hard timeouts):
//   client -> server : u32 protocol version, u32 payload byte count,
//                      UTF-16 null-terminated path
//   server -> client : u32 Waiting ack (sent immediately on receipt,
//                      repeated every 500ms while the request is parked),
//                      then u32 Decision
//
// The Waiting ack decouples the client's patience from the server's UI
// thread: while the server's onRequest callback is still deciding (e.g.
// the primary instance is still booting), the repeated acks keep the
// client parked. This is what lets Explorer-driven consecutive opens land
// in the first instance even while that instance is still starting.
//
// The client never trusts the server unconditionally - it applies staged
// hard timeouts so no unpredictable server state can hang it:
//   1. No single-instance mutex -> no IPC at all, run standalone
//      (the caller's check in FICture2.cpp).
//   2. Pipe connect + first server message: 500ms each. No Waiting ack in
//      time means the server is not actually serving - run standalone.
//   3. After each Waiting ack: 2s of silence allowed. A healthy-but-busy
//      server refreshes the ack twice per second, so tripping this means
//      the server froze or died mid-exchange - run standalone.
#pragma once

#include <functional>
#include <string>

namespace AppIpc
{
    enum class Decision : unsigned long
    {
        Ignore = 0,         // request not handled - caller should run as a new instance
        CompareStarted = 1, // running instance started compare - caller should exit
    };

    // Starts a lightweight named-pipe server in a background thread.
    // Incoming requests contain a single UTF-16 file path. The callback runs
    // on a per-client worker thread (so one slow request cannot starve the
    // accept loop when several instances launch back-to-back) and may block
    // for its whole response budget; the client is parked on the Waiting ack
    // meanwhile. Returns whether the request was handled.
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest);

    // Tries to send a single file path to an existing server and receive a
    // decision. Returns false if the server is not available (no pipe /
    // connection error). Blocks past the connect stage until the server's
    // final Decision (bounded by the server-side response budget).
    bool TrySendPath(const std::wstring& path, Decision& outDecision);
}
