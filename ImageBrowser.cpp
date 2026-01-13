#include "ImageBrowser.h"

#include "framework.h"
#include "Resource.h"
#include "ThumbNavTile.h"
#include "ThumbImageTile.h"
#include "IpcCompareRequest.h"

#include "FD2D/FD2D.h"
#include "FD2D/MainImage.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <memory>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <cwctype>
#include <wrl/client.h>
#include <windowsx.h>
#include <shlobj.h>
#include <commdlg.h>

namespace
{
    constexpr float kThumbStripPadding = 8.0f; // matches thumbs->SetPadding(8)
    constexpr float kThumbMinSide = 32.0f;
    constexpr float kThumbMaxSide = 256.0f;
    constexpr float kThumbStripMinH = (kThumbStripPadding * 2.0f) + kThumbMinSide;
    constexpr float kThumbStripMaxH = (kThumbStripPadding * 2.0f) + kThumbMaxSide;
    constexpr float kSplitPanelDefaultHitThickness = 12.0f; // Splitter::m_hitAreaThickness default

    static float g_syncedThumbStripHeight = 0.0f;
    static bool g_hasSyncedThumbStripHeight = false;
    static std::vector<class ImageBrowserImpl*> g_allBrowsers {};
    static class ImageBrowserImpl* g_rootHorizontalHostBrowser = nullptr;
    static class ImageBrowserImpl* g_contextMenuBrowser = nullptr;
    static std::atomic<UINT_PTR> g_nextThumbApplyTimerId { 0x4D21 };

    static bool g_showNavItems = true;
    static bool g_showNavItemsInitialized = false;
    static bool g_showAlpha = true;
    static bool g_backgroundColorInitialized = false;
    static D2D1_COLOR_F g_focusedBrowserBackgroundColor = D2D1::ColorF(0.18f, 0.16f, 0.03f, 1.0f);

    static uint64_t Fnv1a64(const std::wstring& s)
    {
        // FNV-1a 64-bit over UTF-16 code units.
        uint64_t h = 14695981039346656037ull;
        for (wchar_t c : s)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    static std::wstring Hex64(uint64_t v)
    {
        wchar_t buf[32] {};
        (void)swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(v));
        return buf;
    }

    static std::wstring MakeStableThumbName(const wchar_t* prefix, const std::filesystem::path& p)
    {
        std::wstring s = p.wstring();
        for (auto& c : s)
        {
            c = static_cast<wchar_t>(towlower(c));
        }

        return std::wstring(prefix) + L"_" + Hex64(Fnv1a64(s)) + L"_tile";
    }

    static bool TryGetIniFilePath(std::wstring& outIniFile)
    {
        wchar_t iniPath[MAX_PATH] {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, iniPath)))
        {
            outIniFile = std::wstring(iniPath) + L"\\FICture2\\FICture2.ini";
            return true;
        }
        return false;
    }

    static void EnsureBackgroundColorInitialized(FD2D::Backplate& backplate)
    {
        if (g_backgroundColorInitialized)
        {
            return;
        }
        g_backgroundColorInitialized = true;

        std::wstring iniFile;
        if (!TryGetIniFilePath(iniFile))
        {
            return;
        }

        wchar_t buf[128] {};
        const DWORD n = GetPrivateProfileStringW(L"Window", L"BackgroundColor", L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str());
        if (n == 0)
        {
            return;
        }

        int r = -1;
        int g = -1;
        int b = -1;
        if (swscanf_s(buf, L"%d,%d,%d", &r, &g, &b) != 3)
        {
            return;
        }

        r = (std::max)(0, (std::min)(255, r));
        g = (std::max)(0, (std::min)(255, g));
        b = (std::max)(0, (std::min)(255, b));

        backplate.SetClearColor(D2D1::ColorF(
            static_cast<float>(r) / 255.0f,
            static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f,
            1.0f));

        // Focused background color (optional).
        wchar_t buf2[128] {};
        const DWORD n2 = GetPrivateProfileStringW(L"Window", L"FocusedBackgroundColor", L"", buf2, static_cast<DWORD>(std::size(buf2)), iniFile.c_str());
        if (n2 > 0)
        {
            int fr = -1;
            int fg = -1;
            int fb = -1;
            if (swscanf_s(buf2, L"%d,%d,%d", &fr, &fg, &fb) == 3)
            {
                fr = (std::max)(0, (std::min)(255, fr));
                fg = (std::max)(0, (std::min)(255, fg));
                fb = (std::max)(0, (std::min)(255, fb));
                g_focusedBrowserBackgroundColor = D2D1::ColorF(
                    static_cast<float>(fr) / 255.0f,
                    static_cast<float>(fg) / 255.0f,
                    static_cast<float>(fb) / 255.0f,
                    1.0f);
            }
        }
    }

    static const wchar_t* DxgiFormatToString(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return L"DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return L"DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_BC1_UNORM:
            return L"DXGI_FORMAT_BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return L"DXGI_FORMAT_BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC2_UNORM:
            return L"DXGI_FORMAT_BC2_UNORM";
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return L"DXGI_FORMAT_BC2_UNORM_SRGB";
        case DXGI_FORMAT_BC3_UNORM:
            return L"DXGI_FORMAT_BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return L"DXGI_FORMAT_BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC4_UNORM:
            return L"DXGI_FORMAT_BC4_UNORM";
        case DXGI_FORMAT_BC4_SNORM:
            return L"DXGI_FORMAT_BC4_SNORM";
        case DXGI_FORMAT_BC5_UNORM:
            return L"DXGI_FORMAT_BC5_UNORM";
        case DXGI_FORMAT_BC5_SNORM:
            return L"DXGI_FORMAT_BC5_SNORM";
        case DXGI_FORMAT_BC6H_UF16:
            return L"DXGI_FORMAT_BC6H_UF16";
        case DXGI_FORMAT_BC6H_SF16:
            return L"DXGI_FORMAT_BC6H_SF16";
        case DXGI_FORMAT_BC7_UNORM:
            return L"DXGI_FORMAT_BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return L"DXGI_FORMAT_BC7_UNORM_SRGB";
        default:
            return L"DXGI_FORMAT_UNKNOWN";
        }
    }

    static int DxgiBitsPerPixel(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 32;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 4;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 8;
        default:
            return 0;
        }
    }

    class InfoBar : public FD2D::Wnd
    {
    public:
        InfoBar()
            : Wnd(L"infoBar")
        {
            SetPadding(6.0f);

            m_host = std::make_shared<FD2D::DockPanel>(L"infoDock");

            m_leftText = std::make_shared<FD2D::Text>(L"infoLeft");
            m_leftText->SetFont(L"Segoe UI", 11.0f);
            m_leftText->SetColor(D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f));
            m_leftText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_leftText->SetEllipsisTrimmingEnabled(true);

            m_rightText = std::make_shared<FD2D::Text>(L"infoRight");
            m_rightText->SetFont(L"Segoe UI", 11.0f);
            m_rightText->SetColor(D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f));
            m_rightText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_rightText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            m_rightText->SetFixedWidth(64.0f);

            // DockPanel: Fill stops subsequent docking, so add Right first, then Fill.
            m_host->AddChild(m_rightText);
            m_host->SetChildDock(m_rightText, FD2D::Dock::Right);
            m_host->AddChild(m_leftText);
            m_host->SetChildDock(m_leftText, FD2D::Dock::Fill);

            AddChild(m_host);
        }

        void SetLeftValue(const std::wstring& v)
        {
            if (m_leftText)
            {
                m_leftText->SetText(v);
            }
        }

        void SetRightValue(const std::wstring& v)
        {
            if (m_rightText)
            {
                m_rightText->SetText(v);
            }
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            // Must account for our own padding (top/bottom) + text line height to avoid clipping.
            // Text::Measure uses (fontSize * 1.2) as line height.
            const float fontSize = 12.0f;
            const float lineH = fontSize * 1.2f;
            const float h = (lineH + (2.0f * m_padding) + 2.0f);
            const float w = (available.w > 0.0f) ? available.w : 0.0f;
            m_desired = { w, h };
            return m_desired;
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            if (target != nullptr)
            {
                if (!m_bgBrush)
                {
                    (void)target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.11f, 1.0f), &m_bgBrush);
                }
                if (m_bgBrush)
                {
                    target->FillRectangle(LayoutRect(), m_bgBrush.Get());
                }
            }
            Wnd::OnRender(target);
        }

    private:
        std::shared_ptr<FD2D::DockPanel> m_host {};
        std::shared_ptr<FD2D::Text> m_leftText {};
        std::shared_ptr<FD2D::Text> m_rightText {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush {};
    };

    class PathBar : public FD2D::Wnd
    {
    public:
        PathBar()
            : Wnd(L"pathBar")
        {
            SetPadding(6.0f);
            m_text = std::make_shared<FD2D::Text>(L"pathText");
            m_text->SetFont(L"Segoe UI", 11.0f);
            m_text->SetColor(D2D1::ColorF(0.90f, 0.90f, 0.90f, 1.0f));
            m_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_text->SetEllipsisTrimmingEnabled(true);
            AddChild(m_text);
        }

        void SetRawValue(const std::wstring& v)
        {
            if (v == m_rawValue)
            {
                return;
            }

            m_rawValue = v;
            UpdateFittedText();
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            const float fontSize = 12.0f;
            const float lineH = fontSize * 1.2f;
            const float h = (lineH + (2.0f * m_padding) + 2.0f);
            const float w = (available.w > 0.0f) ? available.w : 0.0f;
            m_desired = { w, h };
            return m_desired;
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            Wnd::Arrange(finalRect);
            UpdateFittedText();
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            if (target != nullptr)
            {
                if (!m_bgBrush)
                {
                    (void)target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.11f, 1.0f), &m_bgBrush);
                }
                if (m_bgBrush)
                {
                    target->FillRectangle(LayoutRect(), m_bgBrush.Get());
                }
            }
            Wnd::OnRender(target);
        }

    private:
        void EnsureFormat()
        {
            if (m_format)
            {
                return;
            }

            IDWriteFactory* factory = FD2D::Core::DWriteFactory();
            if (!factory)
            {
                return;
            }

            (void)factory->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                11.0f,
                L"",
                &m_format);

            if (m_format)
            {
                (void)m_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }

        bool FitsWidth(const std::wstring& s, float maxWidth)
        {
            EnsureFormat();
            if (!m_format)
            {
                return true;
            }

            IDWriteFactory* factory = FD2D::Core::DWriteFactory();
            if (!factory)
            {
                return true;
            }

            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout {};
            (void)factory->CreateTextLayout(
                s.c_str(),
                static_cast<UINT32>(s.size()),
                m_format.Get(),
                100000.0f,
                1000.0f,
                &layout);

            if (!layout)
            {
                return true;
            }

            DWRITE_TEXT_METRICS metrics {};
            (void)layout->GetMetrics(&metrics);
            return metrics.widthIncludingTrailingWhitespace <= maxWidth;
        }

        static std::wstring NormalizeSeparators(std::wstring s)
        {
            for (auto& ch : s)
            {
                if (ch == L'/')
                {
                    ch = L'\\';
                }
            }
            return s;
        }

        static void SplitPathRemainder(const std::wstring& s, std::vector<std::wstring>& outParts)
        {
            outParts.clear();

            size_t start = 0;
            while (start < s.size())
            {
                size_t sep = s.find(L'\\', start);
                if (sep == std::wstring::npos)
                {
                    sep = s.size();
                }

                if (sep > start)
                {
                    outParts.push_back(s.substr(start, sep - start));
                }

                start = sep + 1;
            }
        }

        static void EnsureTrailingSlash(std::wstring& s)
        {
            if (!s.empty() && s.back() != L'\\')
            {
                s.push_back(L'\\');
            }
        }

        static std::wstring JoinTail(const std::vector<std::wstring>& parts, size_t tailCount)
        {
            if (tailCount == 0 || parts.empty())
            {
                return L"";
            }

            const size_t start = (parts.size() > tailCount) ? (parts.size() - tailCount) : 0;

            std::wstring out;
            for (size_t i = start; i < parts.size(); ++i)
            {
                if (!out.empty())
                {
                    out.push_back(L'\\');
                }
                out += parts[i];
            }
            return out;
        }

        std::wstring FolderEllipsize(const std::wstring& raw, float maxWidth)
        {
            if (raw.empty())
            {
                return raw;
            }

            const std::wstring s = NormalizeSeparators(raw);

            // If it already fits, keep as-is.
            if (FitsWidth(s, maxWidth))
            {
                return s;
            }

            std::wstring prefix;
            std::wstring remainder;

            // UNC path: \\server\share\...
            if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
            {
                size_t p = 2;
                const size_t serverEnd = s.find(L'\\', p);
                if (serverEnd == std::wstring::npos)
                {
                    return s;
                }
                const std::wstring server = s.substr(p, serverEnd - p);

                p = serverEnd + 1;
                const size_t shareEnd = s.find(L'\\', p);
                if (shareEnd == std::wstring::npos)
                {
                    return s;
                }
                const std::wstring share = s.substr(p, shareEnd - p);

                prefix = L"\\\\";
                prefix += server;
                prefix.push_back(L'\\');
                prefix += share;
                remainder = s.substr(shareEnd + 1);
            }
            // Drive path: C:\...
            else if (s.size() >= 2 && s[1] == L':')
            {
                if (s.size() >= 3 && s[2] == L'\\')
                {
                    prefix = s.substr(0, 3); // "C:\"
                    remainder = s.substr(3);
                }
                else
                {
                    prefix = s.substr(0, 2); // "C:"
                    remainder = s.substr(2);
                    EnsureTrailingSlash(prefix);
                    if (!remainder.empty() && remainder.front() == L'\\')
                    {
                        remainder.erase(remainder.begin());
                    }
                }
            }
            else
            {
                remainder = s;
            }

            std::vector<std::wstring> parts;
            SplitPathRemainder(remainder, parts);
            if (parts.empty())
            {
                return s;
            }

            // If even the filename doesn't fit, just return the filename and let Text do char-level ellipsis.
            const std::wstring fileOnly = parts.back();
            if (!FitsWidth(fileOnly, maxWidth))
            {
                return fileOnly;
            }

            std::wstring base = prefix;
            EnsureTrailingSlash(base);

            // Try keeping as many tail folders as possible: prefix + "...\\" + last N parts
            for (size_t tailCount = (parts.size() >= 2) ? (parts.size() - 1) : 1; tailCount >= 1; --tailCount)
            {
                std::wstring candidate = base;
                candidate += L"...\\";
                candidate += JoinTail(parts, tailCount);

                if (FitsWidth(candidate, maxWidth))
                {
                    return candidate;
                }

                if (tailCount == 1)
                {
                    break;
                }
            }

            // Fallback: prefix + "...\\" + filename
            std::wstring fallback = base;
            fallback += L"...\\";
            fallback += parts.back();
            return fallback;
        }

        void UpdateFittedText()
        {
            if (!m_text)
            {
                return;
            }

            const D2D1_RECT_F r = LayoutRect();
            float maxWidth = (r.right - r.left) - (2.0f * m_padding);
            if (maxWidth < 8.0f)
            {
                maxWidth = 8.0f;
            }

            const std::wstring fitted = FolderEllipsize(m_rawValue, maxWidth);
            if (fitted != m_lastFittedValue)
            {
                m_lastFittedValue = fitted;
                m_text->SetText(fitted);
            }
        }

        std::shared_ptr<FD2D::Text> m_text {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush {};
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format {};
        std::wstring m_rawValue {};
        std::wstring m_lastFittedValue {};
    };

    static void EnsureShowNavItemsInitialized()
    {
        if (g_showNavItemsInitialized)
        {
            return;
        }
        g_showNavItemsInitialized = true;

        wchar_t iniPath[MAX_PATH] {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, iniPath)))
        {
            const std::wstring iniFile = std::wstring(iniPath) + L"\\FICture2\\FICture2.ini";
            const int v = GetPrivateProfileIntW(L"Viewer", L"ShowNavItems", 1, iniFile.c_str());
            g_showNavItems = (v != 0);
        }
    }

    static void PersistShowNavItems()
    {
        wchar_t iniPath[MAX_PATH] {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, iniPath)))
        {
            const std::wstring iniFile = std::wstring(iniPath) + L"\\FICture2\\FICture2.ini";
            (void)WritePrivateProfileStringW(L"Viewer", L"ShowNavItems", g_showNavItems ? L"1" : L"0", iniFile.c_str());
        }
    }

    static std::wstring JoinFloatsCsv(const std::vector<float>& values)
    {
        std::wstring s;
        for (size_t i = 0; i < values.size(); ++i)
        {
            wchar_t buf[64] {};
            swprintf_s(buf, L"%.6f", values[i]);
            if (i != 0)
            {
                s += L",";
            }
            s += buf;
        }
        return s;
    }

    static std::vector<float> ParseFloatsCsv(const std::wstring& s)
    {
        std::vector<float> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t end = s.find(L',', start);
            if (end == std::wstring::npos)
            {
                end = s.size();
            }
            const std::wstring token = s.substr(start, end - start);
            if (!token.empty())
            {
                out.push_back(static_cast<float>(_wtof(token.c_str())));
            }
            start = end + 1;
        }
        return out;
    }

    static bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt)
    {
        return pt.x >= r.left &&
            pt.x <= r.right &&
            pt.y >= r.top &&
            pt.y <= r.bottom;
    }

    class ImageBrowserImpl : public FD2D::Wnd
    {
    public:
        explicit ImageBrowserImpl(const std::wstring& name, int paneCount, const std::wstring& initialFile = L"")
            : Wnd(name)
            , m_initialFile(initialFile)
        {
            EnsureShowNavItemsInitialized();
            m_showNavItems = g_showNavItems;
            m_thumbApplyTimerId = g_nextThumbApplyTimerId.fetch_add(1);
            g_allBrowsers.push_back(this);
            if (g_rootHorizontalHostBrowser == nullptr)
            {
                g_rootHorizontalHostBrowser = this;
            }
            SetPaneCount(paneCount);
            BuildUi();
        }

        ~ImageBrowserImpl() override
        {
            auto it = std::find(g_allBrowsers.begin(), g_allBrowsers.end(), this);
            if (it != g_allBrowsers.end())
            {
                g_allBrowsers.erase(it);
            }

            if (g_rootHorizontalHostBrowser == this)
            {
                g_rootHorizontalHostBrowser = g_allBrowsers.empty() ? nullptr : g_allBrowsers.front();
            }
        }

        void OnAttached(FD2D::Backplate& backplate) override
        {
            Wnd::OnAttached(backplate);
            EnsureBackgroundColorInitialized(backplate);
            // Default per-ImageBrowser background follows current global clear color.
            m_browserBackgroundColor = backplate.ClearColor();
            m_browserFocusedBackgroundColor = g_focusedBrowserBackgroundColor;
            if (BackplateRef() != nullptr && BackplateRef()->FocusedWnd() == nullptr)
            {
                RequestFocus();
            }
        }

        void SetPaneCount(int paneCount)
        {
            m_paneCount = (std::max)(1, (std::min)(4, paneCount));
            if (m_activePane >= static_cast<size_t>(m_paneCount))
            {
                m_activePane = 0;
            }
            if (m_rootSplit)
            {
                BuildMainPanes();
            }
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            m_desired = available;
            return m_desired;
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            Wnd::Arrange(finalRect);

            ApplySyncedThumbStripHeightIfNeeded();
            UpdateThumbSizingFromPane();

            // First layout: ensure the selected thumb is visible without user interaction.
            if (!m_initialThumbEnsured && m_thumbScroll && m_selectedFocus)
            {
                m_thumbScroll->EnsureCentered(m_selectedFocus->LayoutRect());
                m_initialThumbEnsured = true;
            }
        }

        void OnRenderD3D(ID3D11DeviceContext* context) override
        {
            // Per-ImageBrowser background (stationary, never pans with the image).
            // Draw in the D3D pass so it stays behind GPU-rendered images.
            FD2D::Backplate* bp = BackplateRef();
            if (bp != nullptr && bp->D3DDevice() != nullptr && m_mainPaneHost != nullptr)
            {
                const D2D1_RECT_F r = m_mainPaneHost->LayoutRect();
                if (r.right > r.left && r.bottom > r.top)
                {
                    const D2D1_COLOR_F baseBg = m_browserBackgroundColor;
                    const D2D1_COLOR_F focusedBg = m_browserFocusedBackgroundColor;
                    const D2D1_COLOR_F bg = HasFocus() ? focusedBg : baseBg;
                    (void)bp->ClearRectD3D(r, bg);

                    // Ensure the main image backdrop matches the ImageBrowser background
                    // so focus background doesn't "edge shift" when panning.
                    for (auto& img : m_mainImages)
                    {
                        if (img)
                        {
                            img->SetBackdropColor(bg);
                        }
                    }
                }
            }

            Wnd::OnRenderD3D(context);
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            // Splitter dragging re-arranges the SplitPanel subtree directly, but may not trigger
            // a full root re-Arrange pass. Drive responsive thumbnail sizing here so it updates
            // live while dragging.
            ApplySyncedThumbStripHeightIfNeeded();
            UpdateThumbSizingFromPane();
            RefreshInfoPanel();

            // D2D-only backend: fill per-ImageBrowser background before drawing children.
            // (On the D3D swapchain backend, D2D runs after the GPU image pass, so we must NOT fill here.)
            if (target != nullptr)
            {
                FD2D::Backplate* bp = BackplateRef();
                const bool d3dActive = (bp != nullptr && bp->D3DDevice() != nullptr);
                if (!d3dActive && m_mainPaneHost != nullptr)
                {
                    const D2D1_RECT_F r = m_mainPaneHost->LayoutRect();
                    if (r.right > r.left && r.bottom > r.top)
                    {
                        const D2D1_COLOR_F bg = HasFocus() ? m_browserFocusedBackgroundColor : m_browserBackgroundColor;
                        if (!m_browserBgBrush)
                        {
                            (void)target->CreateSolidColorBrush(bg, m_browserBgBrush.ReleaseAndGetAddressOf());
                        }
                        if (m_browserBgBrush)
                        {
                            m_browserBgBrush->SetColor(bg);
                            target->FillRectangle(r, m_browserBgBrush.Get());
                        }

                        for (auto& img : m_mainImages)
                        {
                            if (img)
                            {
                                img->SetBackdropColor(bg);
                            }
                        }
                    }
                }
            }

            Wnd::OnRender(target);

            if (target == nullptr)
            {
                return;
            }

            // Drag&drop overlay (drawn on top of the main image area only).
            if (m_dragOverlay != DragOverlayKind::None && m_mainPaneHost != nullptr)
            {
                const D2D1_RECT_F r = m_mainPaneHost->LayoutRect();
                if (r.right > r.left && r.bottom > r.top)
                {
                    if (m_dragOverlay == DragOverlayKind::Replace)
                    {
                        if (!m_dragReplaceBrush)
                        {
                            (void)target->CreateSolidColorBrush(
                                D2D1::ColorF(1.0f, 0.0f, 0.0f, 0.18f),
                                m_dragReplaceBrush.ReleaseAndGetAddressOf());
                        }
                        if (m_dragReplaceBrush)
                        {
                            target->FillRectangle(r, m_dragReplaceBrush.Get());
                        }
                    }
                    else if (m_dragOverlay == DragOverlayKind::Insert)
                    {
                        if (!m_dragInsertBrush)
                        {
                            (void)target->CreateSolidColorBrush(
                                D2D1::ColorF(0.0f, 1.0f, 0.0f, 0.18f),
                                m_dragInsertBrush.ReleaseAndGetAddressOf());
                        }
                        if (m_dragInsertBrush)
                        {
                            const float w = (std::max)(1.0f, r.right - r.left);
                            const float splitX = r.left + (w * 0.75f);
                            const D2D1_RECT_F rr { splitX, r.top, r.right, r.bottom };
                            target->FillRectangle(rr, m_dragInsertBrush.Get());
                        }
                    }
                }
            }
        }

        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override
        {
            switch (message)
            {
            case WM_FIC2_IPC_COMPARE:
                return HandleIpcCompareMessage(lParam);

            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN:
                HandleMouseButtonDownFocusMessage(lParam);
                break;

            case WM_RBUTTONUP:
                return HandleContextMenuMessage(lParam);

            case WM_FIC2_DEFERRED_ACTION:
                return HandleDeferredActionMessage();

            case WM_TIMER:
                if (HandleTimerMessage(wParam))
                    return true;
                break;

            case WM_MOUSEWHEEL:
                if (HandleMouseWheelMessage(wParam, lParam))
                    return true;
                break;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (HandleKeyDownMessage(message, wParam, lParam))
                    return true;
                break;

            case WM_KEYUP:
            case WM_SYSKEYUP:
                if (HandleKeyUpMessage(message, wParam, lParam))
                    return true;
                break;

            default:
                break;
            }

            return Wnd::OnMessage(message, wParam, lParam);
        }

        bool HandleIpcCompareMessage(LPARAM lParam)
        {
            auto* req = reinterpret_cast<IpcCompareRequest*>(lParam);
            if (req != nullptr)
            {
                req->compareStarted = TryStartCompareWithFileNameMatch(req->path);
                if (req->doneEvent != nullptr)
                {
                    SetEvent(reinterpret_cast<HANDLE>(req->doneEvent));
                }
            }
            return true;
        }

        void HandleMouseButtonDownFocusMessage(LPARAM lParam)
        {
            const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            // In compare mode the "root" ImageBrowser hosts other ImageBrowsers horizontally,
            // and its LayoutRect spans the whole backplate. Focus the most specific pane
            // under the cursor so focus visuals (and key routing) target the correct viewer.
            if (ImageBrowserImpl* best = FindImageBrowserAtPoint(pt))
            {
                best->RequestFocus();
                return;
            }

            if (RectContainsPoint(LayoutRect(), pt))
            {
                RequestFocus();
            }
        }

        ImageBrowserImpl* FindImageBrowserAtPoint(const POINT& pt) const
        {
            ImageBrowserImpl* best = nullptr;
            float bestArea = 0.0f;

            for (auto* b : g_allBrowsers)
            {
                if (b == nullptr)
                {
                    continue;
                }

                const D2D1_RECT_F r = b->LayoutRect();
                if (!RectContainsPoint(r, pt))
                {
                    continue;
                }

                const float w = (std::max)(0.0f, r.right - r.left);
                const float h = (std::max)(0.0f, r.bottom - r.top);
                const float area = w * h;

                // Choose the smallest containing rect (most specific/deepest pane).
                if (best == nullptr || area < bestArea)
                {
                    best = b;
                    bestArea = area;
                }
            }

            return best;
        }

        bool HandleContextMenuMessage(LPARAM lParam)
        {
            const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (!RectContainsPoint(LayoutRect(), pt))
            {
                return false;
            }

            // IMPORTANT:
            // In compare mode, a root ImageBrowser can contain other ImageBrowsers as child panes.
            // Windows message routing can therefore deliver WM_RBUTTONUP to a parent browser.
            // We must resolve the actual ImageBrowser under the cursor and dispatch the command to it.
            ImageBrowserImpl* target = FindImageBrowserAtPoint(pt);
            if (target == nullptr)
            {
                target = this;
            }

            // Ensure the target browser acts on the pane under the cursor.
            for (size_t i = 0; i < target->m_mainImages.size(); ++i)
            {
                if (target->m_mainImages[i] && RectContainsPoint(target->m_mainImages[i]->LayoutRect(), pt))
                {
                    target->SetActivePane(i);
                    break;
                }
            }

            FD2D::Backplate* backplate = BackplateRef();
            if (backplate == nullptr)
            {
                return false;
            }

            const HWND hwnd = backplate->Window();
            if (hwnd == nullptr)
            {
                return false;
            }

            g_contextMenuBrowser = target;

            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            HMENU hMenu = LoadMenuW(instance, MAKEINTRESOURCEW(IDR_MENU_IMAGEBROWSER_CONTEXT));
            if (hMenu != nullptr)
            {
                HMENU hPopup = GetSubMenu(hMenu, 0);
                if (hPopup != nullptr)
                {
                    // Dynamic state: enable/disable and toggle labels.
                    const int viewerCount = target->HorizontalViewerCount();
                    EnableMenuItem(
                        hPopup,
                        IDM_CTX_OPEN_NEW_IMAGE,
                        MF_BYCOMMAND | ((viewerCount <= 3) ? MF_ENABLED : MF_GRAYED));

                    // Close is available only when 2+ viewers exist, and we don't allow closing the root host browser.
                    const bool canClose = (viewerCount >= 2) && (g_rootHorizontalHostBrowser != nullptr) && (g_contextMenuBrowser != g_rootHorizontalHostBrowser);
                    EnableMenuItem(
                        hPopup,
                        IDM_CTX_CLOSE,
                        MF_BYCOMMAND | (canClose ? MF_ENABLED : MF_GRAYED));

                    ModifyMenuW(
                        hPopup,
                        IDM_CTX_TOGGLE_DIRECTORIES,
                        MF_BYCOMMAND | MF_STRING,
                        IDM_CTX_TOGGLE_DIRECTORIES,
                            g_showNavItems ? L"Hide Directories\tN" : L"Show Directories\tN");

                        ModifyMenuW(
                            hPopup,
                            IDM_CTX_TOGGLE_ALPHA,
                            MF_BYCOMMAND | MF_STRING,
                            IDM_CTX_TOGGLE_ALPHA,
                            g_showAlpha ? L"Hide Alpha\tA" : L"Show Alpha\tA");

                    POINT ptScreen = pt;
                    ClientToScreen(hwnd, &ptScreen);

                    SetForegroundWindow(hwnd);
                    const UINT cmd = TrackPopupMenuEx(
                        hPopup,
                        TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                        ptScreen.x,
                        ptScreen.y,
                        hwnd,
                        nullptr);
                    PostMessageW(hwnd, WM_NULL, 0, 0);

                    if (cmd != 0)
                    {
                        if (g_contextMenuBrowser != nullptr)
                        {
                            g_contextMenuBrowser->HandleContextMenuCommand(cmd);
                        }
                    }
                }
                DestroyMenu(hMenu);
            }

            return true;
        }

        bool HandleDeferredActionMessage()
        {
            if (m_deferredKind != DeferredActionKind::None)
            {
                RunDeferredAction();
                return true;
            }
            return false;
        }

        bool HandleTimerMessage(WPARAM wParam)
        {
            if (wParam != m_thumbApplyTimerId)
            {
                return false;
            }

            if (BackplateRef() != nullptr)
            {
                KillTimer(BackplateRef()->Window(), m_thumbApplyTimerId);
            }

            if (m_hasPendingApply && m_pendingApplyIndex < m_items.size())
            {
                ApplyMainFromIndex(m_pendingApplyIndex);
            }
            m_hasPendingApply = false;
            return true;
        }

        bool HandleMouseWheelMessage(WPARAM wParam, LPARAM lParam)
        {
            // Thumbnail wheel: scrolling should also move selection (and update main image).
            const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            // Wheel over main image should affect the ImageBrowser under the cursor, even if it doesn't
            // currently have focus. Make it the active input source so zoom/view-sync behaves intuitively.
            if (m_mainPaneHost != nullptr && RectContainsPoint(m_mainPaneHost->LayoutRect(), pt))
            {
                RequestFocus();
            }

            if (m_thumbScroll && !m_items.empty())
            {
                if (RectContainsPoint(m_thumbScroll->LayoutRect(), pt))
                {
                    // IMPORTANT:
                    // Debounced apply uses a window WM_TIMER, and Backplate routes non-mouse messages
                    // (including WM_TIMER) only to the focused Wnd. Wheel should therefore also
                    // establish focus so selection + main-image apply stay consistent.
                    RequestFocus();

                    // Use accumulated wheel delta so high-resolution wheels/trackpads still step predictably.
                    m_thumbWheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
                    const int steps = m_thumbWheelRemainder / WHEEL_DELTA;
                    m_thumbWheelRemainder = m_thumbWheelRemainder % WHEEL_DELTA;

                    if (steps != 0)
                    {
                        const int dir = (steps > 0) ? -1 : 1; // wheel-up selects previous, wheel-down selects next
                        const int count = std::abs(steps);

                        size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                        size_t next = cur;

                        for (int s = 0; s < count; ++s)
                        {
                            // Advance to next/prev IMAGE item (skip folders/"..").
                            size_t probe = next;
                            bool found = false;
                            while (true)
                            {
                                if (dir < 0)
                                {
                                    if (probe == 0)
                                    {
                                        break;
                                    }
                                    probe--;
                                }
                                else
                                {
                                    probe++;
                                    if (probe >= m_items.size())
                                    {
                                        break;
                                    }
                                }

                                if (m_items[probe].kind == ThumbItemKind::Image)
                                {
                                    found = true;
                                    break;
                                }
                            }

                            if (!found)
                            {
                                break;
                            }
                            next = probe;
                        }

                        if (next != cur)
                        {
                            // Debounce main image apply to avoid thrashing disk/decoders while spinning the wheel.
                            SelectItemByIndex(next, MainApplyMode::Immediate, true /*ensureCentered*/);
                        }
                    }

                    return true;
                }
            }

            return false;
        }

        bool HandleKeyDownMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            UNREFERENCED_PARAMETER(message);
            UNREFERENCED_PARAMETER(lParam);

            // Per request:
            // - Use KEYDOWN only for Left/Right so holding the key continuously steps.
            // - Handle all other actions on KEYUP.
            switch (wParam)
            {
            case VK_LEFT:
                if (!m_items.empty())
                {
                    const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                    const size_t next = (cur == 0) ? 0 : (cur - 1);
                    SelectItemByIndex(next, MainApplyMode::Immediate);
                }
                return true;

            case VK_RIGHT:
                if (!m_items.empty())
                {
                    const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                    const size_t next = (cur + 1 >= m_items.size()) ? (m_items.size() - 1) : (cur + 1);
                    SelectItemByIndex(next, MainApplyMode::Immediate);
                }
                return true;

            default:
                return false;
            }
        }

        bool HandleKeyUpMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            UNREFERENCED_PARAMETER(message);
            UNREFERENCED_PARAMETER(lParam);

            const bool ctrl = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            const bool shift = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            const bool alt = ((GetKeyState(VK_MENU) & 0x8000) != 0);

            auto PageStep = [this]() -> size_t
            {
                if (!m_thumbScroll)
                {
                    return 1;
                }
                const D2D1_RECT_F r = m_thumbScroll->LayoutRect();
                const float w = r.right - r.left;
                if (w <= 1.0f)
                {
                    return 1;
                }
                const float itemExtent = (std::max)(1.0f, m_thumbW + m_thumbOuterSpacing);
                const int count = static_cast<int>(std::floor(w / itemExtent));
                return static_cast<size_t>((std::max)(1, count));
            };

            switch (wParam)
            {
            case VK_F4:
                if (ctrl)
                {
                    QueueCloseHorizontalThisBrowser();
                    return true;
                }
                return false;

            case VK_UP:
                if (!alt) return false;
            case VK_BACK:
                QueueNavigateUp();
                return true;

            case 'N':
                QueueToggleNavItems();
                return true;

            case 'A':
            case 'a':
                ToggleAlphaCheckerboard();
                return true;

            case 'X':
            case 'x':
                FitToScreen();
                return true;

            case 'B':
            case 'b':
                PickAndApplyBackgroundColor();
                return true;

            case VK_RETURN:
                if (!m_items.empty() && m_selectedIndex < m_items.size())
                {
                    ActivateSelected();
                }
                return true;

            case 'O':
            case 'o':
                if (ctrl && shift)
                {
                    OpenFileDialog(OpenDialogMode::SplitHorizontalNewBrowser);
                    return true;
                }
                if (ctrl)
                {
                    OpenFileDialog(OpenDialogMode::ReplaceCurrent);
                    return true;
                }
                return false;

            case '1':
            case '2':
            case '3':
            case '4':
                {
                    const int idx = static_cast<int>(wParam - '1');
                    SetActivePane(static_cast<size_t>(idx));
                    return true;
                }

            case VK_HOME:
                if (!m_items.empty())
                {
                    SelectItemByIndex(0, MainApplyMode::Immediate);
                }
                return true;

            case VK_END:
                if (!m_items.empty())
                {
                    SelectItemByIndex(m_items.size() - 1, MainApplyMode::Immediate);
                }
                return true;

            case VK_PRIOR: // Page Up
                if (!m_items.empty())
                {
                    const size_t step = PageStep();
                    const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                    const size_t next = (cur > step) ? (cur - step) : 0;
                    SelectItemByIndex(next, MainApplyMode::Immediate);
                }
                return true;

            case VK_NEXT: // Page Down
                if (!m_items.empty())
                {
                    const size_t step = PageStep();
                    const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                    size_t next = cur + step;
                    if (next >= m_items.size())
                    {
                        next = m_items.size() - 1;
                    }
                    SelectItemByIndex(next, MainApplyMode::Immediate);
                }
                return true;

            default:
                return false;
            }
        }

        bool OnFileDrop(const std::wstring& path, const POINT& clientPt) override
        {
            if (path.empty())
            {
                return false;
            }

            // IMPORTANT:
            // In compare mode, this ImageBrowser can contain other ImageBrowser panes as children (split host).
            // If the drop is not for *this* browser's main pane, let children try first so drops work on
            // any pane, not only the first/root ImageBrowser.
            if (Wnd::OnFileDrop(path, clientPt))
            {
                return true;
            }

            // Only accept drops onto the main image region (not the thumbnail strip).
            if (m_mainPaneHost == nullptr || !RectContainsPoint(m_mainPaneHost->LayoutRect(), clientPt))
            {
                return false;
            }

            ClearDragOverlay();

            const D2D1_RECT_F mainRect = m_mainPaneHost->LayoutRect();
            const float mainW = (std::max)(1.0f, mainRect.right - mainRect.left);
            const float relX = (static_cast<float>(clientPt.x) - mainRect.left) / mainW;

            const std::filesystem::path p(path);

            // Right 1/4: insert new ImageBrowser to the right and open the dropped path there.
            if (relX >= 0.75f)
            {
                QueueInsertHorizontalWithPathAfterThis(p);
                return true;
            }

            // Select the pane under the cursor (for 2-4 pane layouts).
            for (size_t i = 0; i < m_mainImages.size(); ++i)
            {
                if (m_mainImages[i] && RectContainsPoint(m_mainImages[i]->LayoutRect(), clientPt))
                {
                    SetActivePane(i);
                    break;
                }
            }

            if (std::filesystem::exists(p) && std::filesystem::is_directory(p))
            {
                QueueDeferredAction(DeferredActionKind::NavigateToFolder, p);
                return true;
            }

            QueueDeferredAction(DeferredActionKind::NavigateToFile, p);
            return true;
        }

        bool OnFileDrag(const std::wstring& path, const POINT& clientPt, FD2D::FileDragVisual& outVisual) override
        {
            // Let child panes handle first (for compare mode where this browser hosts other browsers).
            if (Wnd::OnFileDrag(path, clientPt, outVisual))
            {
                return true;
            }

            if (m_mainPaneHost == nullptr)
            {
                outVisual = FD2D::FileDragVisual::None;
                ClearDragOverlay();
                return false;
            }

            const D2D1_RECT_F r = m_mainPaneHost->LayoutRect();
            if (!RectContainsPoint(r, clientPt))
            {
                outVisual = FD2D::FileDragVisual::None;
                ClearDragOverlay();
                return false;
            }

            const float w = (std::max)(1.0f, r.right - r.left);
            const float relX = (static_cast<float>(clientPt.x) - r.left) / w;
            if (relX < 0.75f)
            {
                m_dragOverlay = DragOverlayKind::Replace;
                outVisual = FD2D::FileDragVisual::Replace;
            }
            else
            {
                m_dragOverlay = DragOverlayKind::Insert;
                outVisual = FD2D::FileDragVisual::Insert;
            }

            Invalidate();
            return true;
        }

        void OnFileDragLeave() override
        {
            Wnd::OnFileDragLeave();
            ClearDragOverlay();
        }

        bool TryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath)
        {
            if (incomingFilePath.empty())
            {
                return false;
            }

            const std::wstring currentPath = ActiveMainPath();
            if (currentPath.empty())
            {
                return false;
            }

            const auto NormalizeLowerName = [](const std::wstring& p) -> std::wstring
            {
                std::filesystem::path fp(p);
                std::wstring name = fp.filename().wstring();
                for (auto& c : name)
                {
                    c = static_cast<wchar_t>(towlower(c));
                }
                return name;
            };

            const std::wstring incomingName = NormalizeLowerName(incomingFilePath);
            const std::wstring currentName = NormalizeLowerName(currentPath);
            if (incomingName.empty() || currentName.empty())
            {
                return false;
            }

            if (incomingName != currentName)
            {
                return false;
            }

            SplitHorizontalWithFile(std::filesystem::path(incomingFilePath));
            return true;
        }

        std::wstring GetDisplayedFilePath() const
        {
            return ActiveMainPath();
        }

        std::wstring GetCurrentFolderPath() const
        {
            return m_currentFolder.wstring();
        }

        // Captures split ratios of the horizontal host tree (preorder), excluding per-browser root splits.
        std::vector<float> CaptureHorizontalSplitRatios() const
        {
            std::vector<float> out;
            CaptureSplitRatiosRecursive(m_hHost, out);
            return out;
        }

        void ApplyHorizontalSplitRatios(const std::vector<float>& ratios)
        {
            size_t idx = 0;
            ApplySplitRatiosRecursive(m_hHost, ratios, idx);
            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        // Restore this browser to show a given file (rebuild thumbs + select/apply).
        void RestoreOpenFile(const std::wstring& filePath)
        {
            if (filePath.empty())
            {
                return;
            }

            const std::filesystem::path p(filePath);
            if (!std::filesystem::exists(p) || !std::filesystem::is_regular_file(p))
            {
                return;
            }

            m_currentFolder = p.parent_path();
            m_initialThumbEnsured = false;

            RebuildThumbList(p);

            // Select/apply exact match if present.
            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].kind == ThumbItemKind::Image && m_items[i].path == p)
                {
                    SelectItemByIndex(i, MainApplyMode::Immediate, true /*ensureCentered*/);
                    return;
                }
            }

            // Fallback: show first image in folder.
            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].kind == ThumbItemKind::Image)
                {
                    SelectItemByIndex(i, MainApplyMode::Immediate, true /*ensureCentered*/);
                    return;
                }
            }
        }

        void RestoreOpenFolder(const std::wstring& folderPath)
        {
            if (folderPath.empty())
            {
                return;
            }
            NavigateToFolder(std::filesystem::path(folderPath));
        }

        void AddHorizontalViewerForRestore(const std::wstring& filePath)
        {
            if (filePath.empty())
            {
                return;
            }
            SplitHorizontalWithFile(std::filesystem::path(filePath));
        }

        void AddHorizontalViewerForRestoreFolder(const std::wstring& folderPath)
        {
            if (folderPath.empty())
            {
                return;
            }

            // Always apply horizontal splitting at the root host browser.
            if (g_rootHorizontalHostBrowser != nullptr && g_rootHorizontalHostBrowser != this)
            {
                g_rootHorizontalHostBrowser->AddHorizontalViewerForRestoreFolder(folderPath);
                return;
            }

            EnsureHorizontalHost();
            if (m_hPanes.empty())
            {
                return;
            }

            if (static_cast<int>(m_hPanes.size()) >= 4)
            {
                return;
            }

            static int s_splitIdFolder = 10001;
            const std::wstring childName = L"browser_split_" + std::to_wstring(s_splitIdFolder++);
            auto newWnd = CreateImageBrowser(childName, 1, L"");
            auto newBrowser = std::dynamic_pointer_cast<ImageBrowserImpl>(newWnd);
            if (newBrowser != nullptr)
            {
                newBrowser->RestoreOpenFolder(folderPath);
            }
            m_hPanes.push_back(newWnd);

            RebuildHorizontalHost();

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        void ForceApplySyncedThumbStripHeight()
        {
            ApplySyncedThumbStripHeightIfNeeded(true);
        }

    private:
        enum class MainApplyMode
        {
            None,
            Debounced,
            Immediate,
        };

        enum class ThumbItemKind
        {
            Up,
            Folder,
            Image,
        };

        struct ThumbItem
        {
            ThumbItemKind kind { ThumbItemKind::Image };
            std::filesystem::path path {};
            std::shared_ptr<FD2D::Wnd> focus {};
            std::shared_ptr<FD2D::ThumbImage> image {};
            std::shared_ptr<ThumbNavTile> navTile {};
            std::shared_ptr<ThumbImageTile> imageTile {};
        };

        static constexpr ULONGLONG kKeyRepeatMinIntervalMs = 60;
        static constexpr UINT WM_FIC2_DEFERRED_ACTION = WM_APP + 0x7A11;
        static constexpr UINT WM_FIC2_IPC_COMPARE = WM_APP + 0x7A12;

        enum class DeferredActionKind
        {
            None,
            ToggleNavItems,
            NavigateToFolder,
            NavigateToFile,
            SplitHorizontalWithFile,
            InsertHorizontalWithPathAfterName,
            CloseHorizontalByName,
            NavigateUp,
            ActivateSelected,
        };

        enum class OpenDialogMode
        {
            ReplaceCurrent,
            SplitHorizontalNewBrowser,
        };

        void UpdateThumbSizingFromPane()
        {
            if (!m_thumbScroll)
            {
                return;
            }

            float paneH = 0.0f;
            if (g_hasSyncedThumbStripHeight)
            {
                // During session restore, individual panes can be briefly laid out with different
                // thumb-strip heights until the first full Arrange pass settles. If we size thumbs
                // from per-pane LayoutRect() in that window, 2nd+ panes can jump to huge thumbnails.
                // Use the globally synced strip height when available so all panes size consistently.
                paneH = g_syncedThumbStripHeight;
            }
            else
            {
                const D2D1_RECT_F scrollRect = m_thumbScroll->LayoutRect();
                paneH = scrollRect.bottom - scrollRect.top;
            }
            if (paneH <= 1.0f)
            {
                return;
            }

            // Keep thumbnail spacing constant; only scale the thumbnail square with the pane height.
            constexpr float contentPadding = 4.0f; // matches thumbs->SetPadding(4)
            const float availableForThumb = paneH - (contentPadding * 2.0f);

            float newSide = availableForThumb;
            newSide = (std::max)(32.0f, (std::min)(256.0f, newSide));

            // Avoid thrashing while dragging the splitter.
            if (std::abs(newSide - m_thumbW) < 1.0f)
            {
                return;
            }

            m_thumbW = newSide;
            m_thumbH = newSide;

            if (m_thumbScroll)
            {
                m_thumbScroll->SetScrollStep((std::max)(48.0f, newSide * 0.75f));
            }

            for (auto& item : m_items)
            {
                if (item.imageTile)
                {
                    item.imageTile->SetFixedSize({ m_thumbW, m_thumbH });
                }
                if (item.navTile)
                {
                    item.navTile->SetFixedSize({ m_thumbW, m_thumbH });
                    item.navTile->Invalidate();
                }
            }

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        void BuildUi()
        {
            // Root: vertical split (main panes + thumb strip)
            auto rootSplit = std::make_shared<FD2D::SplitPanel>(L"rootSplit", FD2D::SplitterOrientation::Vertical);
            // Default: give more space to the main image; thumbnail strip starts shorter.
            rootSplit->SetSplitRatio(0.85f);

            // Thumbnail strip sizing:
            // The splitter's min/max should match the thumbnail min/max size so resizing stays consistent.
            rootSplit->SetSecondPaneMinExtent(kThumbStripMinH);
            rootSplit->SetSecondPaneMaxExtent(kThumbStripMaxH);
            rootSplit->SetConstraintPropagation(FD2D::ConstraintPropagation::Minimum);

            AddChild(rootSplit);
            m_rootSplit = rootSplit;

            BuildMainPanes();

            auto thumbs = std::make_shared<FD2D::StackPanel>(L"thumbs", FD2D::Orientation::Horizontal);
            thumbs->SetSpacing(4.0f);
            thumbs->SetPadding(4.0f);
            auto thumbScroll = std::make_shared<FD2D::ScrollView>(L"thumbScroll");
            thumbScroll->SetScrollStep(96.0f);
            thumbScroll->SetSmoothScrollEnabled(true);
            thumbScroll->SetSmoothTimeMs(110);
            thumbScroll->SetVerticalScrollEnabled(false);
            thumbScroll->SetContent(thumbs);

            rootSplit->SetSecondChild(thumbScroll);
            m_thumbScroll = thumbScroll;

            rootSplit->OnSplitChanged([this](float)
            {
                if (m_thumbScroll == nullptr)
                {
                    return;
                }

                const D2D1_RECT_F r = m_thumbScroll->LayoutRect();
                const float h = (std::max)(0.0f, r.bottom - r.top);
                if (h <= 0.0f)
                {
                    return;
                }

                g_syncedThumbStripHeight = (std::max)(kThumbStripMinH, (std::min)(kThumbStripMaxH, h));
                g_hasSyncedThumbStripHeight = true;

                for (auto* b : g_allBrowsers)
                {
                    if (b != nullptr && b != this)
                    {
                        b->ApplySyncedThumbStripHeightIfNeeded(true);
                    }
                }

                if (BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestLayout();
                }
            });

            constexpr float thumbW = 128.0f;
            constexpr float thumbH = 128.0f;
            m_thumbPanel = thumbs;
            m_thumbW = thumbW;
            m_thumbH = thumbH;
            m_thumbLabelDip = 0.0f;
            m_thumbItemSpacing = 0.0f;
            m_thumbOuterSpacing = 8.0f;

            if (!m_initialFile.empty())
            {
                m_currentFolder = std::filesystem::path(m_initialFile).parent_path();
                RebuildThumbList(m_initialFile);
            }
            else
            {
                // Start empty; a session restore or user navigation will populate.
                m_currentFolder.clear();
                RebuildThumbList({});
            }

            ApplyActiveSelectionStyle();
        }

        void ApplySyncedThumbStripHeightIfNeeded(bool force = false)
        {
            if (!g_hasSyncedThumbStripHeight || m_rootSplit == nullptr)
            {
                return;
            }

            if (m_rootSplit->Orientation() != FD2D::SplitterOrientation::Vertical)
            {
                return;
            }

            const D2D1_RECT_F rootR = m_rootSplit->LayoutRect();
            const float totalH = (std::max)(0.0f, rootR.bottom - rootR.top);
            if (totalH <= 0.0f)
            {
                return;
            }

            const float desiredSecond = (std::max)(kThumbStripMinH, (std::min)(kThumbStripMaxH, g_syncedThumbStripHeight));

            // Estimate available height like SplitPanel::Arrange (childArea minus splitter thickness).
            const float availableH = (std::max)(1.0f, totalH - kSplitPanelDefaultHitThickness);
            const float ratio = 1.0f - (desiredSecond / availableH);

            if (!force)
            {
                // If we're already close, avoid churning layouts.
                if (m_thumbScroll != nullptr)
                {
                    const D2D1_RECT_F tr = m_thumbScroll->LayoutRect();
                    const float cur = (std::max)(0.0f, tr.bottom - tr.top);
                    if (std::abs(cur - desiredSecond) < 1.0f)
                    {
                        return;
                    }
                }
            }

            m_rootSplit->SetSplitRatio((std::max)(0.0f, (std::min)(1.0f, ratio)));

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        void BuildMainPanes()
        {
            if (!m_rootSplit)
            {
                return;
            }

            // Create/recreate pane host
            m_mainImages.clear();
            m_mainGrid.reset();
            m_mainDock.reset();

            if (m_paneCount == 1)
            {
                auto mainImage = std::make_shared<FD2D::MainImage>(L"mainImage0");
                EnsureMainPathSlots();
                if (!m_initialFile.empty())
                {
                    mainImage->SetSourceFile(m_initialFile);
                    m_mainPaths[0] = m_initialFile;
                }
                mainImage->SetOnViewChanged([this](const FD2D::Image::ViewTransform& vt)
                {
                    OnActiveMainViewChanged(vt);
                });
                mainImage->SetOnClick([this]()
                {
                    SetActivePane(0);
                });
                ApplyIniToMainImage(*mainImage);
                m_mainImages.push_back(mainImage);
                m_mainPaneHost = mainImage;
            }
            else
            {
                auto grid = std::make_shared<FD2D::GridPanel>(L"mainGrid");

                // Layout: 2 panes = 1 row x 2 cols, 3-4 panes = 2x2.
                std::vector<FD2D::GridLength> cols;
                std::vector<FD2D::GridLength> rows;
                cols.push_back({ FD2D::GridLength::Type::Star, 1.0f });
                cols.push_back({ FD2D::GridLength::Type::Star, 1.0f });

                if (m_paneCount == 2)
                {
                    rows.push_back({ FD2D::GridLength::Type::Star, 1.0f });
                }
                else
                {
                    rows.push_back({ FD2D::GridLength::Type::Star, 1.0f });
                    rows.push_back({ FD2D::GridLength::Type::Star, 1.0f });
                }

                grid->SetColumns(cols);
                grid->SetRows(rows);

                for (int i = 0; i < m_paneCount; ++i)
                {
                    auto img = std::make_shared<FD2D::MainImage>(L"mainImage" + std::to_wstring(i));
                    EnsureMainPathSlots();
                    if (!m_initialFile.empty())
                    {
                        img->SetSourceFile(m_initialFile);
                        m_mainPaths[static_cast<size_t>(i)] = m_initialFile;
                    }
                    img->SetOnViewChanged([this](const FD2D::Image::ViewTransform& vt)
                    {
                        OnActiveMainViewChanged(vt);
                    });
                    const size_t idx = static_cast<size_t>(i);
                    img->SetOnClick([this, idx]()
                    {
                        SetActivePane(idx);
                    });
                    ApplyIniToMainImage(*img);
                    m_mainImages.push_back(img);

                    int col = (m_paneCount == 2) ? i : (i % 2);
                    int row = (m_paneCount == 2) ? 0 : (i / 2);
                    grid->AddChild(img);
                    grid->SetChildCell(img, col, row);
                }

                m_mainGrid = grid;
                m_mainPaneHost = grid;
            }

            if (!m_infoBar)
            {
                m_infoBar = std::make_shared<InfoBar>();
            }
            if (!m_pathBar)
            {
                m_pathBar = std::make_shared<PathBar>();
            }

            // Main area layout: DockPanel where bottom is info bar and fill is main image/grid.
            auto dock = std::make_shared<FD2D::DockPanel>(L"mainDock");
            dock->AddChild(m_pathBar);
            dock->SetChildDock(m_pathBar, FD2D::Dock::Top);
            dock->AddChild(m_infoBar);
            dock->SetChildDock(m_infoBar, FD2D::Dock::Bottom);
            dock->AddChild(m_mainPaneHost);
            dock->SetChildDock(m_mainPaneHost, FD2D::Dock::Fill);
            m_mainDock = dock;
            m_rootSplit->SetFirstChild(dock);

            ApplyActiveSelectionStyle();
        }

        void EnsureMainPathSlots()
        {
            const size_t needed = static_cast<size_t>((std::max)(1, m_paneCount));
            if (m_mainPaths.size() != needed)
            {
                m_mainPaths.resize(needed);
            }
        }

        std::wstring ActiveMainPath() const
        {
            if (m_mainPaths.empty())
            {
                return L"";
            }
            size_t idx = m_activePane;
            if (idx >= m_mainPaths.size())
            {
                idx = 0;
            }
            return m_mainPaths[idx];
        }

        std::wstring ActiveMainFileNameLower() const
        {
            std::wstring p = ActiveMainPath();
            auto main = ActiveMainImage();
            if (main)
            {
                const auto li = main->GetLoadedInfo();
                if (!li.sourcePath.empty())
                {
                    p = li.sourcePath;
                }
            }
            return ToLower(std::filesystem::path(p).filename().wstring());
        }

        void RefreshInfoPanel()
        {
            if (!m_infoBar)
            {
                return;
            }

            auto main = ActiveMainImage();
            const std::wstring activePath = ActiveMainPath();

            std::wstring displayedFullPath = activePath;
            if (main)
            {
                const auto li = main->GetLoadedInfo();
                if (!li.sourcePath.empty())
                {
                    displayedFullPath = li.sourcePath;
                }
            }

            if (m_pathBar)
            {
                const std::wstring pathDisp = displayedFullPath.empty() ? L"-" : displayedFullPath;
                if (pathDisp != m_lastPathText)
                {
                    m_lastPathText = pathDisp;
                    m_pathBar->SetRawValue(pathDisp);
                }
            }

            uint32_t w = 0;
            uint32_t h = 0;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            if (main)
            {
                const auto li = main->GetLoadedInfo();
                w = li.width;
                h = li.height;
                fmt = li.format;
            }

            int zoomPct = 100;
            if (main)
            {
                zoomPct = static_cast<int>(std::round(main->ZoomScale() * 100.0f));
            }

            std::wstring dim;
            if (w > 0 && h > 0)
            {
                const int bpp = DxgiBitsPerPixel(fmt);
                if (bpp > 0)
                {
                    dim = std::to_wstring(w) + L" x " + std::to_wstring(h) + L" x " + std::to_wstring(bpp);
                }
                else
                {
                    dim = std::to_wstring(w) + L" x " + std::to_wstring(h) + L" x ?";
                }
            }
            else
            {
                dim = L"-";
            }

            std::wstring text;
            text.reserve(256);
            text += dim;
            text += L" | ";
            text += DxgiFormatToString(fmt);

            if (text != m_lastInfoText)
            {
                m_lastInfoText = text;
                m_infoBar->SetLeftValue(text);
            }

            const std::wstring zoomText = std::to_wstring(zoomPct) + L"%";
            if (zoomText != m_lastZoomText)
            {
                m_lastZoomText = zoomText;
                m_infoBar->SetRightValue(zoomText);
            }
        }

        void ApplyIniToMainImage(FD2D::Image& mainImage)
        {
            wchar_t iniPath[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, iniPath)))
            {
                std::wstring iniFile = std::wstring(iniPath) + L"\\FICture2\\FICture2.ini";
                wchar_t zoomStiffnessStr[32];
                DWORD result = GetPrivateProfileStringW(
                    L"Image",
                    L"ZoomStiffness",
                    L"80.0",
                    zoomStiffnessStr,
                    static_cast<DWORD>(std::size(zoomStiffnessStr)),
                    iniFile.c_str());
                if (result > 0)
                {
                    float zoomStiffness = static_cast<float>(_wtof(zoomStiffnessStr));
                    if (zoomStiffness >= 10.0f && zoomStiffness <= 500.0f)
                    {
                        mainImage.SetZoomStiffness(zoomStiffness);
                    }
                }
            }
        }

        std::shared_ptr<FD2D::Image> ActiveMainImage() const
        {
            if (m_mainImages.empty())
            {
                return nullptr;
            }

            size_t idx = m_activePane;
            if (idx >= m_mainImages.size())
            {
                idx = 0;
            }
            return m_mainImages[idx];
        }

        void SetActivePane(size_t index)
        {
            if (m_mainImages.empty())
            {
                m_activePane = 0;
                return;
            }

            if (index >= m_mainImages.size())
            {
                index = m_mainImages.size() - 1;
            }

            m_activePane = index;
            ApplyActiveSelectionStyle();
        }

        void ApplyActiveSelectionStyle()
        {
            // Main images do not render selection borders; keep active-pane purely as a logical state.
        }

        void ApplyMainFromIndex(size_t index)
        {
            auto mainImage = ActiveMainImage();
            if (!mainImage || index >= m_items.size())
            {
                return;
            }

            if (m_items[index].kind != ThumbItemKind::Image)
            {
                return;
            }

            // Cancel any pending debounced apply.
            m_hasPendingApply = false;
            if (BackplateRef() != nullptr)
            {
                KillTimer(BackplateRef()->Window(), m_thumbApplyTimerId);
            }

            mainImage->SetLoadingSpinnerEnabled(true);
            const std::wstring p = m_items[index].path.wstring();
            mainImage->SetSourceFile(p);
            EnsureMainPathSlots();
            if (m_activePane < m_mainPaths.size())
            {
                m_mainPaths[m_activePane] = p;
            }
            mainImage->Invalidate();
            RefreshInfoPanel();
        }

        void ScheduleApply(size_t index)
        {
            auto mainImage = ActiveMainImage();
            if (!mainImage || index >= m_items.size())
            {
                return;
            }

            if (m_items[index].kind != ThumbItemKind::Image)
            {
                return;
            }

            mainImage->SetLoadingSpinnerEnabled(false);
            m_pendingApplyIndex = index;
            m_hasPendingApply = true;

            if (BackplateRef() != nullptr)
            {
                HWND hwnd = BackplateRef()->Window();
                KillTimer(hwnd, m_thumbApplyTimerId);
                SetTimer(hwnd, m_thumbApplyTimerId, 150, nullptr);
            }
        }

        void SelectItemByIndex(size_t index, MainApplyMode mode, bool ensureCentered = true)
        {
            if (m_items.empty())
            {
                return;
            }

            if (index >= m_items.size())
            {
                index = m_items.size() - 1;
            }

            if (m_selectedIndex < m_items.size())
            {
                if (m_items[m_selectedIndex].image)
                {
                    m_items[m_selectedIndex].image->SetSelected(false);
                }
                if (m_items[m_selectedIndex].navTile)
                {
                    m_items[m_selectedIndex].navTile->SetSelected(false);
                }
            }

            m_selectedIndex = index;
            m_selectedFocus = m_items[m_selectedIndex].focus;

            if (m_items[m_selectedIndex].image)
            {
                m_items[m_selectedIndex].image->SetSelected(true);
            }
            if (m_items[m_selectedIndex].navTile)
            {
                m_items[m_selectedIndex].navTile->SetSelected(true);
            }

            if (m_thumbScroll && m_selectedFocus)
            {
                if (ensureCentered)
                {
                    m_thumbScroll->EnsureCentered(m_selectedFocus->LayoutRect());
                }
                else
                {
                    m_thumbScroll->EnsureVisible(m_selectedFocus->LayoutRect(), kThumbStripPadding);
                }
            }

            if (m_items[m_selectedIndex].kind == ThumbItemKind::Image && mode == MainApplyMode::Immediate)
            {
                ApplyMainFromIndex(m_selectedIndex);
            }
            else if (m_items[m_selectedIndex].kind == ThumbItemKind::Image && mode == MainApplyMode::Debounced)
            {
                ScheduleApply(m_selectedIndex);
            }

            // Folder compare sync:
            // If we are in compare mode (2+ ImageBrowsers) and a new image is selected in the thumbnail list,
            // propagate its filename to other ImageBrowsers. Receivers select the same filename in their
            // current directory (if present).
            if (!m_syncSuppressBroadcast && g_allBrowsers.size() >= 2 && m_selectedIndex < m_items.size())
            {
                const ThumbItem& item = m_items[m_selectedIndex];
                if (item.kind == ThumbItemKind::Image)
                {
                    const std::wstring fileNameLower = ToLower(item.path.filename().wstring());
                    if (!fileNameLower.empty())
                    {
                        for (auto* b : g_allBrowsers)
                        {
                            if (b == nullptr || b == this)
                            {
                                continue;
                            }
                            b->OnSyncedFileNameSelected(fileNameLower);
                        }
                    }
                }
            }

            m_initialThumbEnsured = true;
        }

        static std::wstring ToLower(std::wstring s)
        {
            for (auto& c : s)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            return s;
        }

        void OnSyncedFileNameSelected(const std::wstring& fileNameLower)
        {
            if (fileNameLower.empty())
            {
                return;
            }

            // Avoid ping-pong rebroadcast.
            if (m_syncSuppressBroadcast)
            {
                return;
            }

            // If we already have this file selected, do nothing.
            if (m_selectedIndex < m_items.size() && m_items[m_selectedIndex].kind == ThumbItemKind::Image)
            {
                const std::wstring curLower = ToLower(m_items[m_selectedIndex].path.filename().wstring());
                if (!curLower.empty() && curLower == fileNameLower)
                {
                    return;
                }
            }

            // Find same filename among image items in current directory.
            size_t match = static_cast<size_t>(-1);
            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].kind != ThumbItemKind::Image)
                {
                    continue;
                }

                const std::wstring nameLower = ToLower(m_items[i].path.filename().wstring());
                if (!nameLower.empty() && nameLower == fileNameLower)
                {
                    match = i;
                    break;
                }
            }

            if (match == static_cast<size_t>(-1))
            {
                return;
            }

            m_syncSuppressBroadcast = true;
            SelectItemByIndex(match, MainApplyMode::Immediate, true /*ensureCentered*/);
            m_syncSuppressBroadcast = false;
        }

        void OnActiveMainViewChanged(const FD2D::Image::ViewTransform& vt)
        {
            UNREFERENCED_PARAMETER(vt);
            RefreshInfoPanel();

            if (m_viewSyncSuppressBroadcast)
            {
                return;
            }

            // Only sync in compare mode (2+ ImageBrowsers).
            if (g_allBrowsers.size() < 2)
            {
                return;
            }

            // Only propagate when this ImageBrowser is the focused one (input source).
            if (!HasFocus())
            {
                return;
            }

            // Only propagate to other ImageBrowsers displaying the same file name.
            // Use loaded source path when available to avoid stale ActiveMainPath during transitions.
            const std::wstring myNameLower = ActiveMainFileNameLower();
            if (myNameLower.empty())
            {
                return;
            }

            for (auto* b : g_allBrowsers)
            {
                if (!b || b == this)
                {
                    continue;
                }

                const std::wstring otherNameLower = b->ActiveMainFileNameLower();
                if (otherNameLower.empty() || otherNameLower != myNameLower)
                {
                    continue;
                }

                b->ApplySyncedViewTransform(vt);
            }
        }

        void ApplySyncedViewTransform(const FD2D::Image::ViewTransform& vt)
        {
            auto main = ActiveMainImage();
            if (!main)
            {
                return;
            }

            m_viewSyncSuppressBroadcast = true;
            main->SetViewTransform(vt, false /*notify*/);
            m_viewSyncSuppressBroadcast = false;
        }

        void ActivateSelected()
        {
            QueueDeferredAction(DeferredActionKind::ActivateSelected);
        }

        void QueueToggleNavItems()
        {
            QueueDeferredAction(DeferredActionKind::ToggleNavItems);
        }

        void ToggleNavItems()
        {
            EnsureShowNavItemsInitialized();
            g_showNavItems = !g_showNavItems;
            PersistShowNavItems();

            // Apply globally to all ImageBrowsers.
            for (auto* b : g_allBrowsers)
            {
                if (b == nullptr)
                {
                    continue;
                }

                b->m_showNavItems = g_showNavItems;

                std::filesystem::path prefer {};
                if (b->m_selectedIndex < b->m_items.size())
                {
                    prefer = b->m_items[b->m_selectedIndex].path;
                }
                b->RebuildThumbList(prefer);
            }
        }

        void QueueNavigateUp()
        {
            QueueDeferredAction(DeferredActionKind::NavigateUp);
        }

        void NavigateUp()
        {
            if (m_currentFolder.empty())
            {
                return;
            }

            std::filesystem::path parent = m_currentFolder.parent_path();
            if (parent.empty() || parent == m_currentFolder)
            {
                return;
            }

            NavigateToFolder(parent);
        }

        void ActivateSelectedImpl()
        {
            if (m_selectedIndex >= m_items.size())
            {
                return;
            }

            const ThumbItem item = m_items[m_selectedIndex];
            if (item.kind == ThumbItemKind::Image)
            {
                ApplyMainFromIndex(m_selectedIndex);
            }
            else if (item.kind == ThumbItemKind::Up || item.kind == ThumbItemKind::Folder)
            {
                NavigateToFolder(item.path);
            }
        }

        void NavigateToFolder(const std::filesystem::path& folder)
        {
            if (folder.empty() || !std::filesystem::exists(folder) || !std::filesystem::is_directory(folder))
            {
                return;
            }

            m_currentFolder = folder;
            m_initialThumbEnsured = false;
            RebuildThumbList({});

            if (m_thumbScroll)
            {
                m_thumbScroll->SetScrollX(0.0f);
            }

            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].kind == ThumbItemKind::Image)
                {
                    ApplyMainFromIndex(i);
                    break;
                }
            }
        }

        void NavigateToFile(const std::filesystem::path& filePath)
        {
            if (filePath.empty() || !std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath))
            {
                return;
            }

            if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(filePath.wstring()))
            {
                return;
            }

            const std::filesystem::path folder = filePath.parent_path();
            if (folder.empty() || !std::filesystem::exists(folder) || !std::filesystem::is_directory(folder))
            {
                return;
            }

            m_currentFolder = folder;
            m_initialThumbEnsured = false;
            RebuildThumbList(filePath, MainApplyMode::Immediate);
        }

        void RebuildThumbList(const std::filesystem::path& preferSelectPath, MainApplyMode selectMode = MainApplyMode::None)
        {
            if (!m_thumbPanel)
            {
                return;
            }

            m_items.clear();
            m_selectedIndex = static_cast<size_t>(-1);
            m_selectedFocus.reset();

            std::vector<std::filesystem::path> folders;
            std::vector<std::filesystem::path> files;

            if (!m_currentFolder.empty() && std::filesystem::exists(m_currentFolder))
            {
                for (auto& entry : std::filesystem::directory_iterator(m_currentFolder))
                {
                    const std::filesystem::path p = entry.path();
                    if (entry.is_directory())
                    {
                        folders.push_back(p);
                    }
                    else if (entry.is_regular_file())
                    {
                        if (ImageCore::DecoderRegistry::Instance().IsSupportedPath(p.wstring()))
                        {
                            files.push_back(p);
                        }
                    }
                }
            }

            std::sort(folders.begin(), folders.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
            {
                return a.filename().wstring() < b.filename().wstring();
            });

            std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
            {
                return a.filename().wstring() < b.filename().wstring();
            });

            int tileId = 0;

            std::vector<std::wstring> desiredOrder;
            desiredOrder.reserve(folders.size() + files.size() + 8);
            std::unordered_set<std::wstring> desiredNames;
            desiredNames.reserve(folders.size() + files.size() + 8);

            auto getExistingChild = [this](const std::wstring& name) -> std::shared_ptr<FD2D::Wnd>
            {
                const auto& children = m_thumbPanel->Children();
                auto it = children.find(name);
                if (it == children.end())
                {
                    return nullptr;
                }
                return it->second;
            };

            if (m_showNavItems)
            {
                std::filesystem::path parent = m_currentFolder.parent_path();
                if (!parent.empty() && parent != m_currentFolder)
                {
                    std::wstring name = MakeStableThumbName(L"nav_up", parent);
                    auto tile = std::dynamic_pointer_cast<ThumbNavTile>(getExistingChild(name));
                    if (!tile)
                    {
                        // If name exists with the wrong type, replace it.
                        (void)m_thumbPanel->RemoveChild(name);
                        tile = std::make_shared<ThumbNavTile>(name);
                        (void)m_thumbPanel->AddChild(tile);
                    }

                    tile->SetFixedSize({ m_thumbW, m_thumbH });
                    tile->SetText(L"..");
                    tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
                    tile->SetIcon(ThumbNavTile::IconKind::Up);
                    tile->SetOnClick([this]()
                    {
                        QueueNavigateUp();
                    });

                    desiredOrder.push_back(name);
                    desiredNames.emplace(name);
                    m_items.push_back({ ThumbItemKind::Up, parent, tile, nullptr, tile, nullptr });
                    ++tileId;
                }

                for (const auto& dir : folders)
                {
                    std::wstring name = MakeStableThumbName(L"nav_folder", dir);
                    auto tile = std::dynamic_pointer_cast<ThumbNavTile>(getExistingChild(name));
                    if (!tile)
                    {
                        (void)m_thumbPanel->RemoveChild(name);
                        tile = std::make_shared<ThumbNavTile>(name);
                        (void)m_thumbPanel->AddChild(tile);
                    }

                    tile->SetFixedSize({ m_thumbW, m_thumbH });
                    tile->SetText(dir.filename().wstring());
                    tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
                    tile->SetIcon(ThumbNavTile::IconKind::Folder);
                    tile->SetOnClick([this, dir]()
                    {
                        QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                    });

                    desiredOrder.push_back(name);
                    desiredNames.emplace(name);
                    m_items.push_back({ ThumbItemKind::Folder, dir, tile, nullptr, tile, nullptr });
                    ++tileId;
                }
            }

            for (const auto& p : files)
            {
                std::wstring name = MakeStableThumbName(L"thumb_img", p);
                auto thumbTile = std::dynamic_pointer_cast<ThumbImageTile>(getExistingChild(name));
                if (!thumbTile)
                {
                    (void)m_thumbPanel->RemoveChild(name);
                    thumbTile = std::make_shared<ThumbImageTile>(name);
                    thumbTile->SetFixedSize({ m_thumbW, m_thumbH });
                    thumbTile->SetSourceFile(p.wstring());
                    thumbTile->SetCaption(p.filename().wstring());
                    (void)m_thumbPanel->AddChild(thumbTile);
                }
                else
                {
                    // Keep the already-loaded thumbnail; only update sizing/caption.
                    thumbTile->SetFixedSize({ m_thumbW, m_thumbH });
                    thumbTile->SetCaption(p.filename().wstring());
                }

                const size_t index = m_items.size();
                thumbTile->SetOnClick([this, index]()
                {
                    RequestFocus();
                    SelectItemByIndex(index, MainApplyMode::Immediate);
                });

                desiredOrder.push_back(name);
                desiredNames.emplace(name);

                m_items.push_back({ ThumbItemKind::Image, p, thumbTile, thumbTile->ImageWnd(), nullptr, thumbTile });
                ++tileId;
            }

            // Remove children that are no longer desired (avoids accumulating stale tiles).
            {
                std::vector<std::wstring> toRemove;
                for (const auto& kv : m_thumbPanel->Children())
                {
                    if (desiredNames.find(kv.first) == desiredNames.end())
                    {
                        toRemove.push_back(kv.first);
                    }
                }

                for (const auto& name : toRemove)
                {
                    (void)m_thumbPanel->RemoveChild(name);
                }
            }

            // Reorder without detaching existing children (prevents thumbnail reload flicker).
            (void)m_thumbPanel->ReorderChildren(desiredOrder);

            // Restore selection
            size_t selectIndex = 0;
            if (!preferSelectPath.empty())
            {
                for (size_t i = 0; i < m_items.size(); ++i)
                {
                    if (m_items[i].path == preferSelectPath)
                    {
                        selectIndex = i;
                        break;
                    }
                }
            }

            if (!m_items.empty())
            {
                SelectItemByIndex(selectIndex, selectMode);
            }

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
                Invalidate();
            }
        }

        void OpenFileDialog(OpenDialogMode mode)
        {
            wchar_t fileName[MAX_PATH] {};

            // Filter format: "Desc\0pattern\0Desc\0pattern\0\0"
            const wchar_t* filter =
                L"Supported images (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.dds;*.tga)\0"
                L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.dds;*.tga\0"
                L"All files (*.*)\0"
                L"*.*\0\0";

            OPENFILENAMEW ofn {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = BackplateRef() ? BackplateRef()->Window() : nullptr;
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
            ofn.lpstrFilter = filter;
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
            ofn.lpstrTitle = L"Open Image";

            std::wstring initialDir;
            if (!m_currentFolder.empty() && std::filesystem::exists(m_currentFolder) && std::filesystem::is_directory(m_currentFolder))
            {
                initialDir = m_currentFolder.wstring();
                ofn.lpstrInitialDir = initialDir.c_str();
            }

            if (!GetOpenFileNameW(&ofn))
            {
                return; // cancelled
            }

            const std::filesystem::path chosen = std::filesystem::path(fileName);
            if (!std::filesystem::exists(chosen) || !std::filesystem::is_regular_file(chosen))
            {
                return;
            }

            if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(chosen.wstring()))
            {
                MessageBoxW(ofn.hwndOwner, L"Selected file type is not supported.", L"FICture2", MB_OK | MB_ICONWARNING);
                return;
            }

            // Defer UI tree mutation to avoid reentrancy issues during message dispatch.
            QueueDeferredAction(mode == OpenDialogMode::SplitHorizontalNewBrowser ? DeferredActionKind::SplitHorizontalWithFile : DeferredActionKind::NavigateToFile, chosen);
        }

        int HorizontalViewerCount() const
        {
            if (g_rootHorizontalHostBrowser == nullptr)
            {
                return 1;
            }

            if (g_rootHorizontalHostBrowser->m_hPanes.empty())
            {
                return 1;
            }

            return static_cast<int>(g_rootHorizontalHostBrowser->m_hPanes.size());
        }

        bool TryPickImageFile(std::filesystem::path& outPath, const wchar_t* title)
        {
            wchar_t fileName[MAX_PATH] {};

            // Filter format: "Desc\0pattern\0Desc\0pattern\0\0"
            const wchar_t* filter =
                L"Supported images (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.dds;*.tga)\0"
                L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.dds;*.tga\0"
                L"All files (*.*)\0"
                L"*.*\0\0";

            OPENFILENAMEW ofn {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = BackplateRef() ? BackplateRef()->Window() : nullptr;
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
            ofn.lpstrFilter = filter;
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
            ofn.lpstrTitle = (title != nullptr) ? title : L"Open Image";

            std::wstring initialDir;
            if (!m_currentFolder.empty() && std::filesystem::exists(m_currentFolder) && std::filesystem::is_directory(m_currentFolder))
            {
                initialDir = m_currentFolder.wstring();
                ofn.lpstrInitialDir = initialDir.c_str();
            }

            if (!GetOpenFileNameW(&ofn))
            {
                return false; // cancelled
            }

            const std::filesystem::path chosen = std::filesystem::path(fileName);
            if (!std::filesystem::exists(chosen) || !std::filesystem::is_regular_file(chosen))
            {
                return false;
            }

            if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(chosen.wstring()))
            {
                MessageBoxW(ofn.hwndOwner, L"Selected file type is not supported.", L"FICture2", MB_OK | MB_ICONWARNING);
                return false;
            }

            outPath = chosen;
            return true;
        }

        void FitToScreen()
        {
            auto main = ActiveMainImage();
            if (!main)
            {
                return;
            }

            // Make this ImageBrowser the input source so the existing view-sync routine can propagate.
            RequestFocus();

            // Fit-to-screen is simply "reset view"; existing linked/matched sync will apply if filenames match.
            auto vt = main->GetViewTransform();
            vt.zoomScale = 1.0f;
            vt.targetZoomScale = 1.0f;
            vt.zoomVelocity = 0.0f;
            vt.panX = 0.0f;
            vt.panY = 0.0f;
            main->SetViewTransform(vt, true /*notify*/);
            RefreshInfoPanel();
        }

        void HandleContextMenuCommand(UINT cmd)
        {
            switch (cmd)
            {
            case IDM_CTX_OPEN_IMAGE:
                OpenFileDialog(OpenDialogMode::ReplaceCurrent);
                break;
            case IDM_CTX_OPEN_NEW_IMAGE:
                {
                    if (HorizontalViewerCount() > 3)
                    {
                        break;
                    }

                    std::filesystem::path chosen;
                    if (TryPickImageFile(chosen, L"Open New Image"))
                    {
                        QueueInsertHorizontalWithPathAfterThis(chosen);
                    }
                }
                break;
            case IDM_CTX_CLOSE:
                QueueCloseHorizontalThisBrowser();
                break;
            case IDM_CTX_BACKGROUND_COLOR:
                PickAndApplyBackgroundColor();
                break;
            case IDM_CTX_FOCUSED_BACKGROUND_COLOR:
                PickAndApplyFocusedBackgroundColor();
                break;
            case IDM_CTX_FIT_TO_SCREEN:
                FitToScreen();
                break;
            case IDM_CTX_TOGGLE_DIRECTORIES:
                QueueToggleNavItems(); // same as 'N' key (global)
                break;
            case IDM_CTX_TOGGLE_ALPHA:
                ToggleAlphaCheckerboard();
                break;
            default:
                // Show/Hide Alpha is implemented in a later step.
                break;
            }
        }

        void PickAndApplyBackgroundColor()
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr || bp->Window() == nullptr)
            {
                return;
            }

            static COLORREF s_custom[16] {};

            const D2D1_COLOR_F cur = bp->ClearColor();
            const auto toByte = [](float v) -> BYTE
            {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return static_cast<BYTE>(std::floor(v * 255.0f + 0.5f));
            };

            CHOOSECOLORW cc {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = bp->Window();
            cc.lpCustColors = s_custom;
            cc.rgbResult = RGB(toByte(cur.r), toByte(cur.g), toByte(cur.b));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (!ChooseColorW(&cc))
            {
                return; // cancelled
            }

            const BYTE r = GetRValue(cc.rgbResult);
            const BYTE g = GetGValue(cc.rgbResult);
            const BYTE b = GetBValue(cc.rgbResult);
            const D2D1_COLOR_F next = D2D1::ColorF(
                static_cast<float>(r) / 255.0f,
                static_cast<float>(g) / 255.0f,
                static_cast<float>(b) / 255.0f,
                1.0f);

            bp->SetClearColor(next);
            // Keep per-ImageBrowser backgrounds in sync with global clear.
            for (auto* br : g_allBrowsers)
            {
                if (br)
                {
                    br->m_browserBackgroundColor = next;
                }
            }

            std::wstring iniFile;
            if (TryGetIniFilePath(iniFile))
            {
                wchar_t rgb[64] {};
                swprintf_s(rgb, L"%u,%u,%u", static_cast<unsigned>(r), static_cast<unsigned>(g), static_cast<unsigned>(b));
                (void)WritePrivateProfileStringW(L"Window", L"BackgroundColor", rgb, iniFile.c_str());
            }
        }

        void PickAndApplyFocusedBackgroundColor()
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr || bp->Window() == nullptr)
            {
                return;
            }

            static COLORREF s_custom[16] {};

            const auto toByte = [](float v) -> BYTE
            {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return static_cast<BYTE>(std::floor(v * 255.0f + 0.5f));
            };

            CHOOSECOLORW cc {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = bp->Window();
            cc.lpCustColors = s_custom;
            cc.rgbResult = RGB(
                toByte(g_focusedBrowserBackgroundColor.r),
                toByte(g_focusedBrowserBackgroundColor.g),
                toByte(g_focusedBrowserBackgroundColor.b));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (!ChooseColorW(&cc))
            {
                return; // cancelled
            }

            const BYTE r = GetRValue(cc.rgbResult);
            const BYTE g = GetGValue(cc.rgbResult);
            const BYTE b = GetBValue(cc.rgbResult);

            g_focusedBrowserBackgroundColor = D2D1::ColorF(
                static_cast<float>(r) / 255.0f,
                static_cast<float>(g) / 255.0f,
                static_cast<float>(b) / 255.0f,
                1.0f);

            for (auto* br : g_allBrowsers)
            {
                if (br)
                {
                    br->m_browserFocusedBackgroundColor = g_focusedBrowserBackgroundColor;
                }
            }

            if (bp->Window() != nullptr)
            {
                InvalidateRect(bp->Window(), nullptr, FALSE);
            }

            std::wstring iniFile;
            if (TryGetIniFilePath(iniFile))
            {
                wchar_t rgb[64] {};
                swprintf_s(rgb, L"%u,%u,%u", static_cast<unsigned>(r), static_cast<unsigned>(g), static_cast<unsigned>(b));
                (void)WritePrivateProfileStringW(L"Window", L"FocusedBackgroundColor", rgb, iniFile.c_str());
            }
        }

        void ToggleAlphaCheckerboard()
        {
            g_showAlpha = !g_showAlpha;
            const bool checkerEnabled = !g_showAlpha;

            for (auto* b : g_allBrowsers)
            {
                if (b == nullptr)
                {
                    continue;
                }

                for (auto& img : b->m_mainImages)
                {
                    if (img)
                    {
                        img->SetAlphaCheckerboardEnabled(checkerEnabled);
                    }
                }

                // Keep info bars in sync.
                b->RefreshInfoPanel();
            }
        }

        void QueueCloseHorizontalThisBrowser()
        {
            // Do not allow closing the root host browser.
            if (g_rootHorizontalHostBrowser == this)
            {
                return;
            }

            // Require at least 2 viewers.
            if (HorizontalViewerCount() < 2)
            {
                return;
            }

            RequestFocus();
            m_deferredKind = DeferredActionKind::CloseHorizontalByName;
            m_deferredText = Name();
            if (BackplateRef() != nullptr)
            {
                PostMessageW(BackplateRef()->Window(), WM_FIC2_DEFERRED_ACTION, 0, 0);
            }
        }

        void QueueDeferredAction(DeferredActionKind kind, const std::filesystem::path& path = {})
        {
            // Ensure any deferred action runs on the ImageBrowser that originated it.
            // (Mouse messages are often handled by child tiles, so the parent ImageBrowser may not see WM_LBUTTONDOWN.)
            RequestFocus();
            m_deferredKind = kind;
            m_deferredPath = path;
            if (BackplateRef() != nullptr)
            {
                PostMessageW(BackplateRef()->Window(), WM_FIC2_DEFERRED_ACTION, 0, 0);
            }
        }

        void QueueInsertHorizontalWithPathAfterThis(const std::filesystem::path& path)
        {
            RequestFocus();
            m_deferredKind = DeferredActionKind::InsertHorizontalWithPathAfterName;
            m_deferredPath = path;
            m_deferredText = Name();
            if (BackplateRef() != nullptr)
            {
                PostMessageW(BackplateRef()->Window(), WM_FIC2_DEFERRED_ACTION, 0, 0);
            }
        }

        void RunDeferredAction()
        {
            const DeferredActionKind kind = m_deferredKind;
            const std::filesystem::path path = m_deferredPath;
            const std::wstring text = m_deferredText;
            m_deferredKind = DeferredActionKind::None;
            m_deferredPath.clear();
            m_deferredText.clear();

            switch (kind)
            {
            case DeferredActionKind::ToggleNavItems:
                ToggleNavItems();
                break;
            case DeferredActionKind::NavigateToFolder:
                NavigateToFolder(path);
                break;
            case DeferredActionKind::NavigateToFile:
                NavigateToFile(path);
                break;
            case DeferredActionKind::SplitHorizontalWithFile:
                SplitHorizontalWithFile(path);
                break;
            case DeferredActionKind::InsertHorizontalWithPathAfterName:
                InsertHorizontalWithPathAfterName(text, path);
                break;
            case DeferredActionKind::CloseHorizontalByName:
                CloseHorizontalByName(text);
                break;
            case DeferredActionKind::NavigateUp:
                NavigateUp();
                break;
            case DeferredActionKind::ActivateSelected:
                ActivateSelectedImpl();
                break;
            default:
                break;
            }
        }

        void CloseHorizontalByName(const std::wstring& name)
        {
            if (name.empty())
            {
                return;
            }

            // Always apply closing at the root host browser.
            if (g_rootHorizontalHostBrowser != nullptr && g_rootHorizontalHostBrowser != this)
            {
                g_rootHorizontalHostBrowser->CloseHorizontalByName(name);
                return;
            }

            EnsureHorizontalHost();
            if (m_hPanes.size() < 2)
            {
                return;
            }

            // Pane 0 is this root browser's existing UI root, not an ImageBrowserImpl.
            for (size_t i = 1; i < m_hPanes.size(); ++i)
            {
                auto paneBrowser = std::dynamic_pointer_cast<ImageBrowserImpl>(m_hPanes[i]);
                if (paneBrowser && paneBrowser->Name() == name)
                {
                    m_hPanes.erase(m_hPanes.begin() + static_cast<std::ptrdiff_t>(i));
                    RebuildHorizontalHost();
                    if (BackplateRef() != nullptr)
                    {
                        BackplateRef()->RequestLayout();
                    }
                    return;
                }
            }
        }

        void InsertHorizontalWithPathAfterName(const std::wstring& afterName, const std::filesystem::path& path)
        {
            if (g_rootHorizontalHostBrowser != nullptr && g_rootHorizontalHostBrowser != this)
            {
                g_rootHorizontalHostBrowser->InsertHorizontalWithPathAfterName(afterName, path);
                return;
            }

            if (path.empty())
            {
                return;
            }

            EnsureHorizontalHost();
            if (m_hPanes.empty())
            {
                return;
            }

            if (static_cast<int>(m_hPanes.size()) >= 4)
            {
                return;
            }

            size_t insertIndex = m_hPanes.size();
            for (size_t i = 0; i < m_hPanes.size(); ++i)
            {
                if (m_hPanes[i] && m_hPanes[i]->Name() == afterName)
                {
                    insertIndex = i + 1;
                    break;
                }
            }

            static int s_insertId = 20001;
            const std::wstring childName = L"browser_insert_" + std::to_wstring(s_insertId++);
            auto newWnd = CreateImageBrowser(childName, 1, L"");
            auto newBrowser = std::dynamic_pointer_cast<ImageBrowserImpl>(newWnd);
            if (newBrowser)
            {
                if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
                {
                    newBrowser->RestoreOpenFolder(path.wstring());
                }
                else
                {
                    newBrowser->RestoreOpenFile(path.wstring());
                }
            }

            if (insertIndex > m_hPanes.size())
            {
                insertIndex = m_hPanes.size();
            }

            m_hPanes.insert(m_hPanes.begin() + static_cast<std::ptrdiff_t>(insertIndex), newWnd);
            RebuildHorizontalHost();

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        void EnsureHorizontalHost()
        {
            if (!m_hPanes.empty())
            {
                return;
            }

            // Pane 0 is this browser's existing UI root.
            if (m_rootSplit)
            {
                m_hPanes.push_back(m_rootSplit);
            }
        }

        void SplitHorizontalWithFile(const std::filesystem::path& filePath)
        {
            if (filePath.empty())
            {
                return;
            }

            // Always apply horizontal splitting at the root host browser.
            // Otherwise, if the user triggers Ctrl+Shift+O from a non-root pane, we'd build a nested split-tree
            // inside that pane (breaking equal-width distribution across all ImageBrowsers).
            if (g_rootHorizontalHostBrowser != nullptr && g_rootHorizontalHostBrowser != this)
            {
                g_rootHorizontalHostBrowser->SplitHorizontalWithFile(filePath);
                return;
            }

            EnsureHorizontalHost();
            if (m_hPanes.empty())
            {
                return;
            }

            if (static_cast<int>(m_hPanes.size()) >= 4)
            {
                if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
                {
                    MessageBoxW(
                        BackplateRef()->Window(),
                        L"Maximum 4 viewers are supported.",
                        L"FICture2",
                        MB_OK | MB_ICONINFORMATION);
                }
                return;
            }

            static int s_splitId = 1;
            const std::wstring childName = L"browser_split_" + std::to_wstring(s_splitId++);
            auto newBrowser = CreateImageBrowser(childName, 1, filePath.wstring());
            m_hPanes.push_back(newBrowser);

            RebuildHorizontalHost();

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        std::shared_ptr<FD2D::Wnd> BuildEqualWidthSplitTree(const std::vector<std::shared_ptr<FD2D::Wnd>>& panes)
        {
            const size_t n = panes.size();
            if (n == 0)
            {
                return nullptr;
            }
            if (n == 1)
            {
                return panes[0];
            }

            auto makeSplit = [](const std::wstring& name, float ratio, const std::shared_ptr<FD2D::Wnd>& a, const std::shared_ptr<FD2D::Wnd>& b) -> std::shared_ptr<FD2D::SplitPanel>
            {
                auto sp = std::make_shared<FD2D::SplitPanel>(name, FD2D::SplitterOrientation::Horizontal);
                sp->SetSplitRatio(ratio);
                sp->SetConstraintPropagation(FD2D::ConstraintPropagation::None);
                sp->SetFirstChild(a);
                sp->SetSecondChild(b);
                return sp;
            };

            static int s_hostId = 1;

            if (n == 2)
            {
                return makeSplit(L"hSplit2_" + std::to_wstring(s_hostId++), 0.5f, panes[0], panes[1]);
            }
            if (n == 3)
            {
                // 1/3 | (2/3 split into 1/2 + 1/2) => 1/3, 1/3, 1/3
                auto right = makeSplit(L"hSplit3_r_" + std::to_wstring(s_hostId++), 0.5f, panes[1], panes[2]);
                return makeSplit(L"hSplit3_" + std::to_wstring(s_hostId++), 1.0f / 3.0f, panes[0], right);
            }
            // n == 4
            {
                // (1/2 split into 1/2 + 1/2) | (1/2 split into 1/2 + 1/2) => quarters
                auto left = makeSplit(L"hSplit4_l_" + std::to_wstring(s_hostId++), 0.5f, panes[0], panes[1]);
                auto right = makeSplit(L"hSplit4_r_" + std::to_wstring(s_hostId++), 0.5f, panes[2], panes[3]);
                return makeSplit(L"hSplit4_" + std::to_wstring(s_hostId++), 0.5f, left, right);
            }
        }

        void RebuildHorizontalHost()
        {
            // Rebuild the full host so all panes become equal width.
            // (Nested splits with 0.5 ratios produce 50/25/25 otherwise.)
            const auto host = BuildEqualWidthSplitTree(m_hPanes);
            if (!host)
            {
                return;
            }

            // Replace this Wnd's children with the new host tree.
            ClearChildren();
            AddChild(host);
            m_hHost = host;
        }

        int m_paneCount { 1 };
        size_t m_activePane { 0 };

        std::shared_ptr<FD2D::SplitPanel> m_rootSplit {};
        std::shared_ptr<FD2D::Wnd> m_mainPaneHost {};
        std::shared_ptr<FD2D::GridPanel> m_mainGrid {};
        std::shared_ptr<FD2D::DockPanel> m_mainDock {};
        std::shared_ptr<PathBar> m_pathBar {};
        std::shared_ptr<InfoBar> m_infoBar {};
        std::wstring m_lastPathText {};
        std::wstring m_lastInfoText {};
        std::wstring m_lastZoomText {};
        std::vector<std::shared_ptr<FD2D::Image>> m_mainImages {};
        std::vector<std::wstring> m_mainPaths {};

        std::shared_ptr<FD2D::Wnd> m_selectedFocus {};
        std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
        std::shared_ptr<FD2D::StackPanel> m_thumbPanel {};
        std::vector<ThumbItem> m_items {};
        size_t m_selectedIndex { static_cast<size_t>(-1) };
        bool m_initialThumbEnsured { false };
        ULONGLONG m_lastKeyNavMs { 0 };
        int m_thumbWheelRemainder { 0 };
        bool m_hasPendingApply { false };
        size_t m_pendingApplyIndex { static_cast<size_t>(-1) };

        std::filesystem::path m_currentFolder {};
        bool m_showNavItems { true };
        float m_thumbW { 128.0f };
        float m_thumbH { 128.0f };
        float m_thumbLabelDip { 0.0f };
        float m_thumbItemSpacing { 0.0f };
        float m_thumbOuterSpacing { 8.0f }; // spacing between thumbnail "items" in the horizontal strip

        DeferredActionKind m_deferredKind { DeferredActionKind::None };
        std::filesystem::path m_deferredPath {};
        std::wstring m_deferredText {};

        std::wstring m_initialFile {};
        std::vector<std::shared_ptr<FD2D::Wnd>> m_hPanes {};
        std::shared_ptr<FD2D::Wnd> m_hHost {};

        // (no focus-background state)
        bool m_syncSuppressBroadcast { false };
        bool m_viewSyncSuppressBroadcast { false };
        UINT_PTR m_thumbApplyTimerId { 0 };

        enum class DragOverlayKind
        {
            None,
            Replace,
            Insert
        };

        void ClearDragOverlay()
        {
            if (m_dragOverlay != DragOverlayKind::None)
            {
                m_dragOverlay = DragOverlayKind::None;
                Invalidate();
            }
        }

        DragOverlayKind m_dragOverlay { DragOverlayKind::None };
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dragReplaceBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dragInsertBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_browserBgBrush {};

        // ImageBrowser background colors (used for focus indication).
        // NOTE: base defaults match the global clear; focused defaults to a dark yellow accent.
        D2D1_COLOR_F m_browserBackgroundColor { 0.09f, 0.09f, 0.10f, 1.0f };
        D2D1_COLOR_F m_browserFocusedBackgroundColor { 0.18f, 0.16f, 0.03f, 1.0f };

        static void CaptureSplitRatiosRecursive(const std::shared_ptr<FD2D::Wnd>& node, std::vector<float>& out)
        {
            if (!node)
            {
                return;
            }

            auto sp = std::dynamic_pointer_cast<FD2D::SplitPanel>(node);
            if (sp && sp->Orientation() == FD2D::SplitterOrientation::Horizontal)
            {
                out.push_back(sp->SplitRatio());
            }

            for (const auto& ch : node->ChildrenInOrder())
            {
                CaptureSplitRatiosRecursive(ch, out);
            }
        }

        static void ApplySplitRatiosRecursive(const std::shared_ptr<FD2D::Wnd>& node, const std::vector<float>& ratios, size_t& idx)
        {
            if (!node)
            {
                return;
            }

            auto sp = std::dynamic_pointer_cast<FD2D::SplitPanel>(node);
            if (sp && sp->Orientation() == FD2D::SplitterOrientation::Horizontal)
            {
                if (idx < ratios.size())
                {
                    sp->SetSplitRatio(ratios[idx]);
                }
                idx++;
            }

            for (const auto& ch : node->ChildrenInOrder())
            {
                ApplySplitRatiosRecursive(ch, ratios, idx);
            }
        }
    };
}

std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, int paneCount, const std::wstring& initialFile)
{
    return std::make_shared<ImageBrowserImpl>(name, paneCount, initialFile);
}

bool ImageBrowser_TryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath)
{
    if (g_rootHorizontalHostBrowser == nullptr)
    {
        return false;
    }

    return g_rootHorizontalHostBrowser->TryStartCompareWithFileNameMatch(incomingFilePath);
}

void ImageBrowser_OpenFileInRoot(const std::wstring& filePath)
{
    if (g_rootHorizontalHostBrowser == nullptr)
    {
        return;
    }
    g_rootHorizontalHostBrowser->RestoreOpenFile(filePath);
}

void ImageBrowser_SaveSessionToIni(const std::wstring& iniFile)
{
    if (iniFile.empty() || g_rootHorizontalHostBrowser == nullptr)
    {
        return;
    }

    // Save viewer count + per-viewer displayed file.
    const int count = static_cast<int>((std::min)(static_cast<size_t>(4), g_allBrowsers.size()));
    {
        wchar_t buf[32] {};
        _itow_s(count, buf, 10);
        (void)WritePrivateProfileStringW(L"Session", L"ViewerCount", buf, iniFile.c_str());
    }

    // Thumb strip height (if known).
    if (g_hasSyncedThumbStripHeight)
    {
        wchar_t buf[64] {};
        swprintf_s(buf, L"%.2f", g_syncedThumbStripHeight);
        (void)WritePrivateProfileStringW(L"Session", L"ThumbStripHeight", buf, iniFile.c_str());
    }

    // Horizontal split ratios (preorder).
    {
        const std::wstring csv = JoinFloatsCsv(g_rootHorizontalHostBrowser->CaptureHorizontalSplitRatios());
        (void)WritePrivateProfileStringW(L"Session", L"HorizontalSplitRatios", csv.c_str(), iniFile.c_str());
    }

    for (int i = 0; i < count; ++i)
    {
        auto* b = g_allBrowsers[static_cast<size_t>(i)];
        if (!b)
        {
            continue;
        }

        const std::wstring sec = L"Viewer" + std::to_wstring(i);
        (void)WritePrivateProfileStringW(sec.c_str(), L"DisplayedFile", b->GetDisplayedFilePath().c_str(), iniFile.c_str());
        (void)WritePrivateProfileStringW(sec.c_str(), L"CurrentFolder", b->GetCurrentFolderPath().c_str(), iniFile.c_str());
    }
}

bool ImageBrowser_TryRestoreSessionFromIni(const std::wstring& iniFile)
{
    if (iniFile.empty() || g_rootHorizontalHostBrowser == nullptr)
    {
        return false;
    }

    wchar_t buf[8192] {};

    const int count = GetPrivateProfileIntW(L"Session", L"ViewerCount", 0, iniFile.c_str());
    if (count <= 0)
    {
        return false;
    }

    const int clampedCount = (std::max)(1, (std::min)(4, count));

#if defined(_DEBUG)
    {
        wchar_t msg[1024] {};
        swprintf_s(msg, L"[FICture2] RestoreSession: ini='%s' ViewerCount=%d (clamped=%d)\n", iniFile.c_str(), count, clampedCount);
        OutputDebugStringW(msg);
    }
#endif

    // Load thumb strip height (optional).
    const DWORD nThumb = GetPrivateProfileStringW(L"Session", L"ThumbStripHeight", L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str());
    if (nThumb > 0)
    {
        const float h = static_cast<float>(_wtof(buf));
        if (h > 1.0f)
        {
            g_syncedThumbStripHeight = h;
            g_hasSyncedThumbStripHeight = true;
        }
    }

    // Read viewer state list.
    struct ViewerState
    {
        std::wstring filePath;
        std::wstring folderPath;
    };

    std::vector<ViewerState> viewers;
    viewers.reserve(static_cast<size_t>(clampedCount));
    for (int i = 0; i < clampedCount; ++i)
    {
        const std::wstring sec = L"Viewer" + std::to_wstring(i);
        buf[0] = 0;
        const DWORD nFile = GetPrivateProfileStringW(sec.c_str(), L"DisplayedFile", L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str());
        std::wstring file = (nFile > 0 && buf[0] != 0) ? std::wstring(buf) : std::wstring();

        buf[0] = 0;
        const DWORD nFolder = GetPrivateProfileStringW(sec.c_str(), L"CurrentFolder", L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str());
        std::wstring folder = (nFolder > 0 && buf[0] != 0) ? std::wstring(buf) : std::wstring();

        viewers.push_back({ std::move(file), std::move(folder) });
    }

#if defined(_DEBUG)
    for (int i = 0; i < clampedCount; ++i)
    {
        const auto& v = viewers[static_cast<size_t>(i)];
        wchar_t msg[2048] {};
        swprintf_s(
            msg,
            L"[FICture2] RestoreSession: Viewer%d file='%s' folder='%s'\n",
            i,
            v.filePath.c_str(),
            v.folderPath.c_str());
        OutputDebugStringW(msg);
    }
#endif

    // Apply root viewer.
    if (!viewers.empty())
    {
        if (!viewers[0].filePath.empty())
        {
            g_rootHorizontalHostBrowser->RestoreOpenFile(viewers[0].filePath);
        }
        else if (!viewers[0].folderPath.empty())
        {
            g_rootHorizontalHostBrowser->RestoreOpenFolder(viewers[0].folderPath);
        }
    }

    // Add additional viewers.
    for (int i = 1; i < clampedCount; ++i)
    {
        const auto& v = viewers[static_cast<size_t>(i)];
        if (!v.filePath.empty())
        {
            g_rootHorizontalHostBrowser->AddHorizontalViewerForRestore(v.filePath);
        }
        else if (!v.folderPath.empty())
        {
            g_rootHorizontalHostBrowser->AddHorizontalViewerForRestoreFolder(v.folderPath);
        }
    }

    // Apply thumb strip height across all (will clamp to min/max on layout).
    if (g_hasSyncedThumbStripHeight)
    {
        for (auto* b : g_allBrowsers)
        {
            if (b)
            {
                b->ForceApplySyncedThumbStripHeight();
            }
        }
    }

    // Apply horizontal split ratios (optional).
    buf[0] = 0;
    const DWORD nRat = GetPrivateProfileStringW(L"Session", L"HorizontalSplitRatios", L"", buf, static_cast<DWORD>(std::size(buf)), iniFile.c_str());
    if (nRat > 0 && buf[0] != 0)
    {
        const std::vector<float> ratios = ParseFloatsCsv(buf);
        g_rootHorizontalHostBrowser->ApplyHorizontalSplitRatios(ratios);
    }

    return true;
}

