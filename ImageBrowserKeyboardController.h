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

    // The type-to-select query and when its last character was typed.
    struct TypeToSelectState
    {
        std::wstring query {};
        unsigned long long lastInputMs { 0 };
    };

    // How long a query lasts after its last character.
    constexpr unsigned long long kTypeToSelectTimeoutMs = 1200;

    // Clears the query once more than kTypeToSelectTimeoutMs passed since its
    // last character.
    void ExpireTypeToSelect(unsigned long long nowMs, TypeToSelectState& state);

    // One typed (lower-case) character: appends it to the query and selects
    // the first item after `currentSelection` (wrapping) whose label starts
    // with the query, ignoring case. A longer query that matches nothing
    // restarts from this character alone, so repeating a letter cycles
    // through its items. Expires an old query first.
    void TypeToSelectCharacter(
        wchar_t ch,
        unsigned long long nowMs,
        size_t itemCount,
        size_t currentSelection,
        const std::function<std::wstring(size_t)>& labelAt,
        const std::function<void(size_t)>& selectIndex,
        TypeToSelectState& state);

    // A key down: ignored (false) with no items or with Ctrl/Alt; then an old
    // query expires; navigation/system keys and keys without a printable
    // character (translated with the thread's keyboard state) are ignored;
    // anything else is typed (true).
    bool HandleTypeToSelect(
        const KeyEvent& event,
        unsigned long long nowMs,
        size_t itemCount,
        size_t currentSelection,
        const std::function<std::wstring(size_t)>& labelAt,
        const std::function<void(size_t)>& selectIndex,
        TypeToSelectState& state);
}
