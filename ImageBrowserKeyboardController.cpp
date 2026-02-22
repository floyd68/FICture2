#include "ImageBrowserKeyboardController.h"

#include <cwctype>

namespace
{
    bool IsNavigationOrSystemKey(UINT keyCode)
    {
        switch (keyCode)
        {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RETURN:
        case VK_TAB:
        case VK_DELETE:
        case VK_INSERT:
        case VK_F1:
        case VK_F2:
        case VK_F3:
        case VK_F4:
        case VK_F5:
        case VK_F6:
        case VK_F7:
        case VK_F8:
        case VK_F9:
        case VK_F10:
        case VK_F11:
        case VK_F12:
            return true;
        default:
            return false;
        }
    }

    bool TryGetPrintableKey(const ImageBrowserKeyboardController::KeyEvent& event, wchar_t& outChar)
    {
        wchar_t chars[4] {};
        BYTE keyState[256] {};
        if (!GetKeyboardState(keyState))
        {
            return false;
        }

        const int converted = ToUnicode(event.keyCode, event.scanCode, keyState, chars, 4, 0);
        if (converted < 0)
        {
            wchar_t clearBuf[4] {};
            (void)ToUnicode(event.keyCode, event.scanCode, keyState, clearBuf, 4, 0);
            return false;
        }
        if (converted <= 0)
        {
            return false;
        }

        const wchar_t ch = chars[0];
        if (!iswprint(ch))
        {
            return false;
        }

        outChar = static_cast<wchar_t>(towlower(ch));
        return true;
    }

    bool StartsWithInsensitive(const std::wstring& text, const std::wstring& prefix)
    {
        if (prefix.size() > text.size())
        {
            return false;
        }

        for (size_t i = 0; i < prefix.size(); ++i)
        {
            const wchar_t tc = static_cast<wchar_t>(towlower(text[i]));
            const wchar_t pc = static_cast<wchar_t>(towlower(prefix[i]));
            if (tc != pc)
            {
                return false;
            }
        }
        return true;
    }

    bool TrySelectByQuery(
        const std::wstring& query,
        size_t itemCount,
        size_t currentSelection,
        const std::function<std::wstring(size_t)>& labelAt,
        const std::function<void(size_t)>& selectIndex)
    {
        if (query.empty() || itemCount == 0)
        {
            return false;
        }

        size_t start = 0;
        if (currentSelection < itemCount)
        {
            start = (currentSelection + 1) % itemCount;
        }

        for (size_t offset = 0; offset < itemCount; ++offset)
        {
            const size_t idx = (start + offset) % itemCount;
            if (StartsWithInsensitive(labelAt(idx), query))
            {
                selectIndex(idx);
                return true;
            }
        }

        return false;
    }
}

bool ImageBrowserKeyboardController::HandleTypeToSelect(
    const KeyEvent& event,
    unsigned long long nowMs,
    size_t itemCount,
    size_t currentSelection,
    const std::function<std::wstring(size_t)>& labelAt,
    const std::function<void(size_t)>& selectIndex,
    TypeToSelectState& state)
{
    if (itemCount == 0)
    {
        return false;
    }

    if (event.control || event.alt)
    {
        return false;
    }

    if (nowMs - state.lastInputMs > 1200)
    {
        state.query.clear();
    }

    if (IsNavigationOrSystemKey(event.keyCode))
    {
        return false;
    }

    wchar_t ch = 0;
    if (!TryGetPrintableKey(event, ch))
    {
        return false;
    }

    std::wstring nextQuery = state.query;
    nextQuery.push_back(ch);
    if (!TrySelectByQuery(nextQuery, itemCount, currentSelection, labelAt, selectIndex) && nextQuery.size() > 1)
    {
        nextQuery.assign(1, ch);
        (void)TrySelectByQuery(nextQuery, itemCount, currentSelection, labelAt, selectIndex);
    }

    state.query = nextQuery;
    state.lastInputMs = nowMs;
    return true;
}

bool ImageBrowserKeyboardController::HandleTypeToSelectWithStateStorage(
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
    unsigned long long& ioLastInputMs)
{
    KeyEvent keyEvent {};
    keyEvent.keyCode = keyCode;
    keyEvent.scanCode = scanCode;
    keyEvent.control = control;
    keyEvent.alt = alt;

    TypeToSelectState state {};
    state.query = ioQuery;
    state.lastInputMs = ioLastInputMs;
    const bool handled = HandleTypeToSelect(
        keyEvent,
        nowMs,
        itemCount,
        currentSelection,
        labelAt,
        selectIndex,
        state);
    ioQuery = state.query;
    ioLastInputMs = state.lastInputMs;
    return handled;
}
