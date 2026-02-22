#pragma once

#include <Windows.h>

#include <functional>
#include <string>

namespace ImageBrowserKeyboardController
{
    struct KeyEvent
    {
        UINT keyCode { 0 };
        UINT scanCode { 0 };
        bool control { false };
        bool alt { false };
    };

    struct TypeToSelectState
    {
        std::wstring query {};
        unsigned long long lastInputMs { 0 };
    };

    bool HandleTypeToSelect(
        const KeyEvent& event,
        unsigned long long nowMs,
        size_t itemCount,
        size_t currentSelection,
        const std::function<std::wstring(size_t)>& labelAt,
        const std::function<void(size_t)>& selectIndex,
        TypeToSelectState& state);

    bool HandleTypeToSelectWithStateStorage(
        UINT keyCode,
        UINT scanCode,
        bool control,
        bool alt,
        unsigned long long nowMs,
        size_t itemCount,
        size_t currentSelection,
        const std::function<std::wstring(size_t)>& labelAt,
        const std::function<void(size_t)>& selectIndex,
        std::wstring& ioQuery,
        unsigned long long& ioLastInputMs);
}
