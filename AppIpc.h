// AppIpc.h - single-instance IPC for FICture2.
//
// Protocol v2 (Waiting ack + staged client hard timeouts):
//   client -> server : u32 protocol version, u32 payload byte count,
//                      UTF-16 null-terminated path
//   server -> client : u32 Waiting ack (sent on receipt), then u32 Decision
//
// The server-side decision is immediate: the onRequest callback only checks
// the file name against the primary instance's open-documents snapshot and
// queues the path (see IpcOpenRequest.h) - it never waits for the UI to load
// anything. The server runs it on the client's worker, without another thread.
//
// The client never trusts the server unconditionally - staged hard bounds:
//   1. No single-instance mutex -> no IPC at all, run standalone.
//   2. Pipe connect + first server message: 500ms each.
//   3. After each Waiting ack: 2s silence allowed, AND at most 3 Waiting
//      acks total. Tripping either bound means the server froze or is
//      wedged mid-decision - the client stops waiting and opens alone.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace AppIpc
{
    enum class Decision : unsigned long
    {
        Ignore = 0,         // request not handled - caller should run as a new instance
        CompareStarted = 1, // running instance accepted/queued the file - caller should exit
    };

    using RequestHandler = std::function<Decision(const std::wstring&)>;

    // The app's pipe. Tests use their own name so they never reach a running
    // FICture2 (the pipe is created without FILE_FLAG_FIRST_PIPE_INSTANCE).
    inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\FICture2_IPC";

    // Clients served at once; the accept thread waits for a free slot before
    // it listens again, so further clients wait in WaitNamedPipeW.
    inline constexpr std::size_t kMaxClientWorkers = 8;
    // A client's whole exchange (request, reply, closing its end) must finish
    // within this, or the server drops it and frees the slot.
    inline constexpr std::chrono::milliseconds kClientExchangeTimeout { 2000 };

    // Owned named-pipe server: one accept thread plus at most
    // kMaxClientWorkers client threads, all joined by Stop. Every wait of the
    // accept and client threads (connect, read, write) also wakes on the stop
    // event, so an idle server or a connected client that sends nothing does
    // not delay Stop.
    class Server
    {
    public:
        Server();
        // Stops and joins, retrying until every thread joined (like
        // FolderEnumerationService); the app stops the server in its cleanup.
        ~Server();

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        // Starts listening on `pipeName`. Incoming requests contain a single
        // UTF-16 file path; `onRequest` runs on that client's worker and must
        // decide at once (queue-or-decline). One start per server: false when
        // already started or stopped, or when the threads could not start.
        bool Start(const std::wstring& pipeName, RequestHandler onRequest);

        // Signals the stop event, cancels pending pipe I/O and joins every
        // thread if all of them finish within `bound`. Otherwise returns false
        // and keeps the remaining threads (not detached), so a later Stop can
        // still join them. Repeatable; true for a server that never started.
        bool Stop(std::chrono::milliseconds bound);

        // Tests/diagnostics: live accept + client threads, and live clients.
        std::size_t WorkerCount() const;
        std::size_t ClientWorkerCount() const;

        struct State;

    private:
        std::shared_ptr<State> m_state;
    };

    // Tries to send a single file path to the server on `pipeName` and receive
    // a decision. Returns false if the server is not available. Bounded by the
    // staged client timeouts above.
    bool TrySendPath(const std::wstring& pipeName, const std::wstring& path, Decision& outDecision);
}
