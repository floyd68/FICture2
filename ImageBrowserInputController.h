#pragma once

#include "ImageBrowserThumbTypes.h"

#include <functional>
#include <vector>
#include <d2d1.h>
#include <windows.h>

class ImageBrowserInputController
{
public:
    struct MouseWheelContext
    {
        const std::vector<ThumbItem>* items { nullptr };
        size_t selectedIndex { 0 };
        bool hasMainRect { false };
        D2D1_RECT_F mainRect {};
        bool hasThumbRect { false };
        D2D1_RECT_F thumbRect {};
        std::function<void()> requestFocus;
        std::function<void(size_t)> selectIndex;
    };

    struct KeyContext
    {
        const std::vector<ThumbItem>* items { nullptr };
        size_t selectedIndex { 0 };
        bool hasThumbRect { false };
        D2D1_RECT_F thumbRect {};
        float thumbW { 0.0f };
        float thumbOuterSpacing { 0.0f };
        std::function<void(size_t)> selectIndex;
        std::function<void()> activateSelected;
        std::function<void()> queueNavigateUp;
        std::function<void()> queueToggleNavItems;
        std::function<void()> toggleAlphaCheckerboard;
        std::function<void()> fitToScreen;
        std::function<void()> pickBackgroundColor;
        std::function<void()> toggleSamplingQuality;
        std::function<void()> closeHorizontalThisBrowser;
        std::function<void()> openFileReplace;
        std::function<void()> openFileSplit;
    };

    bool HandleMouseWheel(const MouseWheelContext& ctx, WPARAM wParam, LPARAM lParam);
    bool HandleKeyDown(const KeyContext& ctx, UINT message, WPARAM wParam, LPARAM lParam);
    bool HandleKeyUp(const KeyContext& ctx, UINT message, WPARAM wParam, LPARAM lParam);

private:
    int m_thumbWheelRemainder { 0 };
};
