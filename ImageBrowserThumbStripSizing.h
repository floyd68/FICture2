#pragma once

#include <optional>

// The thumbnail strip's sizing state, without UI objects: the tile side (the
// row height; navigation tiles are square), when it was last applied (the
// throttle input of ImageBrowserThumbLayoutCoordinator::EvaluateThumbSize),
// whether thumbnail size upgrades are held during a splitter drag, and a
// strip height changed during a drag that is broadcast to the other viewers
// once the drag ends. Evaluating the size and applying it to tiles stay with
// the browser.
class ThumbStripSizing
{
public:
    static constexpr float kDefaultSide = 128.0f;
    // Between thumbnail items in the strip.
    static constexpr float kOuterSpacing = 8.0f;

    float Side() const { return m_side; }
    unsigned long long LastApplyMs() const { return m_lastApplyMs; }

    void ResetSide() { m_side = kDefaultSide; }
    void SetSide(float side) { m_side = side; }
    void MarkApplied(unsigned long long nowMs) { m_lastApplyMs = nowMs; }

    bool UpgradesDeferred() const { return m_upgradesDeferred; }
    // Returns true when the state changed (the tiles must be told).
    bool SetUpgradesDeferred(bool deferred);

    // The strip height changed during a splitter drag: broadcast it later.
    // A later height replaces an earlier one.
    void DeferBroadcast(float height);
    bool BroadcastPending() const { return m_broadcastPending; }
    // The pending height once no splitter is dragged, which is then no
    // longer pending; nothing while dragging or when none is pending.
    std::optional<float> TakeBroadcast(bool splitterDragging);

private:
    float m_side { kDefaultSide };
    unsigned long long m_lastApplyMs { 0 };
    bool m_upgradesDeferred { false };
    bool m_broadcastPending { false };
    float m_broadcastHeight { 0.0f };
};
