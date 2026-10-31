#pragma once

#include <d2d1.h>

#include <optional>
#include <string>

// The viewer settings shared by every image browser of a window and the INI
// keys that keep them:
//   [Viewer] ShowNavItems            1 / 0 (default 1)
//   [Window] BackgroundColor         "r,g,b" (0-255; the window's clear colour)
//   [Window] FocusedBackgroundColor  "r,g,b"
//   [Image]  ZoomStiffness           10-500 (default 80; other values ignored)
// The alpha checkerboard is shared but not saved. Values are loaded once;
// each setter writes its key (when an INI path is given).
class ViewerSettings
{
public:
    static constexpr D2D1_COLOR_F kDefaultFocusedBackgroundColor { 0.18f, 0.16f, 0.03f, 1.0f };
    static constexpr float kDefaultZoomStiffness = 80.0f;
    static constexpr float kMinZoomStiffness = 10.0f;
    static constexpr float kMaxZoomStiffness = 500.0f;

    bool Loaded() const { return m_loaded; }
    // Keeps the defaults: later Load calls do nothing (tests).
    void MarkLoaded() { m_loaded = true; }
    // Loads the settings from `iniPath` the first time it is called; an empty
    // path or an unreadable file keeps the defaults. Returns the saved
    // background colour when this call read one (the window applies it).
    std::optional<D2D1_COLOR_F> Load(const std::wstring& iniPath);

    bool ShowNavItems() const { return m_showNavItems; }
    bool AlphaCheckerboard() const { return m_alphaCheckerboard; }
    D2D1_COLOR_F FocusedBackgroundColor() const { return m_focusedBackgroundColor; }
    float ZoomStiffness() const { return m_zoomStiffness; }

    void SetShowNavItems(bool show, const std::wstring& iniPath);
    void SetFocusedBackgroundColor(const D2D1_COLOR_F& color, const std::wstring& iniPath);
    void SetAlphaCheckerboard(bool enabled) { m_alphaCheckerboard = enabled; }
    // The background colour lives in the window (its clear colour); this
    // only saves it.
    static void SaveBackgroundColor(const D2D1_COLOR_F& color, const std::wstring& iniPath);

    // "r,g,b" with 0-255 components (clamped); false for anything else.
    static bool ParseRgb(const std::wstring& text, D2D1_COLOR_F& outColor);
    static std::wstring FormatRgb(const D2D1_COLOR_F& color);

private:
    bool m_loaded { false };
    bool m_showNavItems { true };
    bool m_alphaCheckerboard { false };
    D2D1_COLOR_F m_focusedBackgroundColor { kDefaultFocusedBackgroundColor };
    float m_zoomStiffness { kDefaultZoomStiffness };
};
