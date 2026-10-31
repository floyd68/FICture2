#include "TestHarness.h"

#include "ImageBrowserKeyboardController.h"

#include <string>
#include <vector>

// Type-to-select without keyboard state: the matching rules for an already
// translated character, the query expiry, and the HandleTypeToSelect gating
// that returns before any key translation.

namespace
{
    namespace KC = ImageBrowserKeyboardController;

    struct Strip
    {
        std::vector<std::wstring> labels;
        size_t selected { static_cast<size_t>(-1) };
        KC::TypeToSelectState state {};

        void Type(wchar_t ch, unsigned long long nowMs)
        {
            KC::TypeToSelectCharacter(
                ch,
                nowMs,
                labels.size(),
                selected,
                [this](size_t i) { return labels[i]; },
                [this](size_t i) { selected = i; },
                state);
        }
    };

    Strip Labels()
    {
        Strip s;
        s.labels = { L"..", L"Apple.png", L"apricot.jpg", L"Banana.png", L"avocado.dds", L"cherry.png" };
        return s;
    }
}

FIC2_TEST(TypeToSelect_SearchesAfterTheSelectionAndWraps)
{
    Strip s = Labels();
    s.Type(L'b', 1000);
    FIC2_CHECK(s.selected == 3);
    FIC2_CHECK(s.state.query == L"b");
    FIC2_CHECK(s.state.lastInputMs == 1000);

    // From index 3 the next 'a' item is after it; then the search wraps.
    s.state = {};
    s.Type(L'a', 5000);
    FIC2_CHECK(s.selected == 4);     // avocado, after Banana
    s.state = {};
    s.Type(L'a', 9000);
    FIC2_CHECK(s.selected == 1);     // wrapped to Apple (case-insensitive)

    // The up item matches "."
    s.state = {};
    s.Type(L'.', 13000);
    FIC2_CHECK(s.selected == 0);
}

FIC2_TEST(TypeToSelect_LongerQueryNarrows)
{
    Strip s = Labels();
    s.Type(L'a', 1000);
    FIC2_CHECK(s.selected == 1);     // Apple
    s.Type(L'p', 1100);
    FIC2_CHECK(s.selected == 2);     // "ap": the next match after Apple is apricot
    s.Type(L'r', 1200);
    FIC2_CHECK(s.selected == 2);     // "apr": the search wraps back to apricot itself
    FIC2_CHECK(s.state.query == L"apr");
}

FIC2_TEST(TypeToSelect_RepeatedLetterCycles)
{
    Strip s = Labels();
    s.Type(L'a', 1000);
    FIC2_CHECK(s.selected == 1);
    // "aa" matches nothing: restart from 'a' after the selection.
    s.Type(L'a', 1100);
    FIC2_CHECK(s.selected == 2);
    FIC2_CHECK(s.state.query == L"a");
    s.Type(L'a', 1200);
    FIC2_CHECK(s.selected == 4);
    s.Type(L'a', 1300);
    FIC2_CHECK(s.selected == 1);     // wrapped

    // A character nothing matches keeps the selection; the query is that character.
    s.Type(L'z', 1400);
    FIC2_CHECK(s.selected == 1);
    FIC2_CHECK(s.state.query == L"z");
}

FIC2_TEST(TypeToSelect_QueryExpires)
{
    KC::TypeToSelectState state {};
    state.query = L"ap";
    state.lastInputMs = 1000;

    KC::ExpireTypeToSelect(1000 + KC::kTypeToSelectTimeoutMs, state);
    FIC2_CHECK(state.query == L"ap");    // exactly at the timeout: kept
    KC::ExpireTypeToSelect(1001 + KC::kTypeToSelectTimeoutMs, state);
    FIC2_CHECK(state.query.empty());

    // Typing after the timeout starts a new query.
    Strip s = Labels();
    s.Type(L'a', 1000);
    s.Type(L'c', 1000 + KC::kTypeToSelectTimeoutMs + 1);
    FIC2_CHECK(s.state.query == L"c");
    FIC2_CHECK(s.selected == 5);
}

FIC2_TEST(TypeToSelect_GatingBeforeTranslation)
{
    const auto label = [](size_t) { return std::wstring(L"apple"); };
    size_t selected = static_cast<size_t>(-1);
    const auto select = [&selected](size_t i) { selected = i; };

    KC::TypeToSelectState state {};
    state.query = L"ap";
    state.lastInputMs = 1000;
    KC::KeyEvent key {};
    key.keyCode = 'A';

    // No items: ignored, the query is untouched.
    FIC2_CHECK(!KC::HandleTypeToSelect(key, 99999, 0, selected, label, select, state));
    FIC2_CHECK(state.query == L"ap");

    // Ctrl or Alt: ignored before the expiry.
    key.control = true;
    FIC2_CHECK(!KC::HandleTypeToSelect(key, 99999, 3, selected, label, select, state));
    key.control = false;
    key.alt = true;
    FIC2_CHECK(!KC::HandleTypeToSelect(key, 99999, 3, selected, label, select, state));
    FIC2_CHECK(state.query == L"ap");
    key.alt = false;

    // A navigation key is ignored, but an old query expires first.
    key.keyCode = VK_DOWN;
    FIC2_CHECK(!KC::HandleTypeToSelect(key, 1500, 3, selected, label, select, state));
    FIC2_CHECK(state.query == L"ap");
    FIC2_CHECK(!KC::HandleTypeToSelect(key, 99999, 3, selected, label, select, state));
    FIC2_CHECK(state.query.empty());
    FIC2_CHECK(selected == static_cast<size_t>(-1));
}
