#include "ImageBrowserThumbStripSizing.h"

bool ThumbStripSizing::SetUpgradesDeferred(bool deferred)
{
    if (deferred == m_upgradesDeferred)
    {
        return false;
    }
    m_upgradesDeferred = deferred;
    return true;
}

void ThumbStripSizing::DeferBroadcast(float height)
{
    m_broadcastPending = true;
    m_broadcastHeight = height;
}

std::optional<float> ThumbStripSizing::TakeBroadcast(bool splitterDragging)
{
    if (!m_broadcastPending || splitterDragging)
    {
        return std::nullopt;
    }
    m_broadcastPending = false;
    return m_broadcastHeight;
}
