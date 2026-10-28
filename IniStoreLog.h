#pragma once

#include "AppLog.h"
#include "IniStore.h"

// IniStore is shared with NIFDiff and cannot log; FICture2 callers pass every
// write's status through here so a write that was skipped (LockTimeout) or
// failed (CommitFailed) is logged.
inline IniStore::WriteStatus LogIniWrite(IniStore::WriteStatus status, const char* what)
{
    if (status != IniStore::WriteStatus::Committed)
    {
        FIC2_LOG_WARN("[Ini] {} not saved: {}", what, IniStore::StatusName(status));
    }
    return status;
}
