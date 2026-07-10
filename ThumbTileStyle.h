#pragma once

#include <algorithm>

// Shared visual constants for thumbnail strip tiles.
// Image tiles and nav tiles must agree on caption styling.
namespace ThumbTileStyle
{
    inline constexpr const wchar_t* kCaptionFontFamily = L"Segoe UI";

    // Caption font size scales with tile height, clamped to a readable range.
    inline float CaptionFontSize(float tileHeight)
    {
        return (std::max)(10.0f, (std::min)(14.0f, tileHeight * 0.11f));
    }
}
