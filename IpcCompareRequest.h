#pragma once

#include <string>

// Cross-thread request object used to marshal an IPC compare request onto the UI thread.
// Ownership: created/freed by the IPC server thread; the UI thread only mutates fields and signals the event.
struct IpcCompareRequest
{
    std::wstring path {};
    void* doneEvent { nullptr }; // HANDLE
    bool compareStarted { false };
};

