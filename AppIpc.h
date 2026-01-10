#pragma once

#include <functional>
#include <string>

namespace AppIpc
{
    enum class Decision : unsigned long
    {
        Ignore = 0,
        CompareStarted = 1,
    };

    // Starts a lightweight named-pipe server in a background thread.
    // Incoming requests contain a single UTF-16 file path.
    // The callback returns whether the request was handled as a "compare start" or ignored.
    void StartServer(const std::function<Decision(const std::wstring&)>& onRequest);

    // Tries to send a single file path to an existing server and receive a decision.
    // Returns false if the server is not available (no pipe / connection error).
    bool TrySendPath(const std::wstring& path, Decision& outDecision);
}

