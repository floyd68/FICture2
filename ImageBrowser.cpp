#include "ImageBrowser.h"

#include "framework.h"
#include "Resource.h"
#include "ThumbNavTile.h"
#include "ThumbImageTile.h"
#include "ImageBrowserMainPane.h"
#include "ImageBrowserDragDrop.h"
#include "ImageBrowserDragOverlay.h"
#include "ImageBrowserThumbTypes.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserNavigation.h"
#include "ImageBrowserInputController.h"
#include "IpcCompareRequest.h"
#include "Ficture2Backplate.h"
#include "AppSetup.h"

#include "FD2D/FD2D.h"
#include "FD2D/Core.h"
#include "FD2D/MainImage.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <vector>
#include <cwctype>
#include <wrl/client.h>
#include <wincodec.h>
#include <windowsx.h>
#include <shlobj.h>
#include <commdlg.h>

namespace
{
    class ImageBrowserImpl;

    constexpr float kThumbStripPadding = 8.0f; // matches thumbs->SetPadding(8)
    constexpr float kThumbMinSide = 32.0f;
    constexpr float kThumbMaxSide = 256.0f;
    constexpr float kThumbStripMinH = (kThumbStripPadding * 2.0f) + kThumbMinSide;
    constexpr float kThumbStripMaxH = (kThumbStripPadding * 2.0f) + kThumbMaxSide;
    constexpr float kSplitPanelDefaultHitThickness = 12.0f; // Splitter::m_hitAreaThickness default

    static float g_syncedThumbStripHeight = 0.0f;
    static bool g_hasSyncedThumbStripHeight = false;
    static class ImageBrowserImpl* g_rootHorizontalHostBrowser = nullptr;
    static class ImageBrowserImpl* g_contextMenuBrowser = nullptr;
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

    static std::wstring MakeStableThumbName(const wchar_t* prefix, const VirtualPath& vp)
    {
        std::wstring s = vp.GetDisplayPath();
        for (auto& c : s)
        {
            c = static_cast<wchar_t>(towlower(c));
        }

        return std::wstring(prefix) + L"_" + Hex64(Fnv1a64(s)) + L"_tile";
    }

    static std::wstring NormalizePathLowerForCompare(const std::filesystem::path& p)
    {
        std::wstring s = p.wstring();
        for (auto& c : s)
        {
            if (c == L'/')
            {
                c = L'\\';
            }
            c = static_cast<wchar_t>(towlower(c));
        }
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
        {
            s.pop_back();
        }
        return s;
    }

    static bool PathEqualsInsensitive(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        return NormalizePathLowerForCompare(a) == NormalizePathLowerForCompare(b);
    }

    static bool PathEqualsInsensitive(const std::filesystem::path& a, const VirtualPath& b)
    {
        return NormalizePathLowerForCompare(a) == NormalizePathLowerForCompare(b.hostPath);
    }

    static bool PathEqualsInsensitive(const VirtualPath& a, const VirtualPath& b)
    {
        return a == b;
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

    static std::wstring ToLowerString(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t c)
            {
                return static_cast<wchar_t>(towlower(c));
            });
        return value;
    }

    static std::wstring ArchiveFormatLabelForPath(const std::wstring& path)
    {
        if (path.empty())
        {
            return L"";
        }

        auto vpath = VirtualPath::Parse(path);
        if (!vpath)
        {
            return L"";
        }

        if (!vpath->IsInArchive() && !vpath->IsArchiveFile())
        {
            return L"";
        }

        const std::wstring ext = ToLowerString(vpath->hostPath.extension().wstring());
        if (ext == L".zip")
        {
            return L"ZIP";
        }
        if (ext == L".7z")
        {
            return L"7Z";
        }
        if (ext == L".rar")
        {
            return L"RAR";
        }
        if (ext == L".ba2")
        {
            return L"BA2";
        }

        return L"";
    }

    static std::wstring SamplingLabel(bool highQuality, FD2D::Backplate* backplate)
    {
        const bool usingD3D = (backplate != nullptr && backplate->D3DDevice() != nullptr);
        if (usingD3D)
        {
            return highQuality ? L"D3D11 Anisotropic" : L"D3D11 Point";
        }

        const FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
        if (highQuality)
        {
            return (d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D HQ Cubic" : L"D2D Linear";
        }

        return (d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D Nearest" : L"D2D Linear";
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
        explicit ImageBrowserImpl(const std::wstring& name, const std::wstring& initialFile = L"")
            : Wnd(name)
            , m_initialFile(initialFile)
        {
            EnsureShowNavItemsInitialized();
            m_showNavItems = g_showNavItems;
            BuildUi();
        }

        ~ImageBrowserImpl() override
        {
            UnregisterFromEventBus();
        }

        void OnAttached(FD2D::Backplate& backplate) override
        {
            Wnd::OnAttached(backplate);
            RegisterWithEventBus();
            EnsureBackgroundColorInitialized(backplate);
            // Default per-ImageBrowser background follows current global clear color.
            m_browserBackgroundColor = backplate.ClearColor();
            m_browserFocusedBackgroundColor = g_focusedBrowserBackgroundColor;
            if (BackplateRef() != nullptr && BackplateRef()->FocusedWnd() == nullptr)
            {
                RequestFocus();
            }
        }

        void OnDetached() override
        {
            UnregisterFromEventBus();
            Wnd::OnDetached();
        }

        std::shared_ptr<Ficture2Backplate::EventBus> EventBusRef() const
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr)
            {
                return nullptr;
            }

            auto* ficBp = dynamic_cast<Ficture2Backplate*>(bp);
            if (ficBp == nullptr)
            {
                return nullptr;
            }

            return ficBp->BusPtr();
        }

        void RegisterWithEventBus()
        {
            if (!m_eventBus.expired())
            {
                return;
            }

            auto bus = EventBusRef();
            if (!bus)
            {
                return;
            }

            m_eventBus = bus;
            bus->RegisterImageBrowser(this);
            if (m_eventBusToken == 0)
            {
                m_eventBusToken = bus->Subscribe([this](const Ficture2Backplate::ImageBrowserEvent& ev)
                {
                    HandleBusEvent(ev);
                });
            }

            if (g_rootHorizontalHostBrowser == nullptr)
            {
                g_rootHorizontalHostBrowser = this;
            }
        }

        void UnregisterFromEventBus()
        {
            auto bus = m_eventBus.lock();
            if (!bus)
            {
                if (g_rootHorizontalHostBrowser == this)
                {
                    g_rootHorizontalHostBrowser = nullptr;
                }
                return;
            }

            bus->UnregisterImageBrowser(this);

            if (m_eventBusToken != 0)
            {
                bus->Unsubscribe(m_eventBusToken);
                m_eventBusToken = 0;
            }

            if (g_rootHorizontalHostBrowser == this)
            {
                const auto remaining = bus->ImageBrowsersSnapshot();
                g_rootHorizontalHostBrowser = remaining.empty()
                    ? nullptr
                    : static_cast<ImageBrowserImpl*>(remaining.front());
            }

            if (g_contextMenuBrowser == this)
            {
                g_contextMenuBrowser = nullptr;
            }

            m_eventBus.reset();
        }

        size_t ImageBrowserCount() const
        {
            auto bus = m_eventBus.lock();
            if (!bus)
            {
                return 0;
            }
            return bus->ImageBrowserCount();
        }

        std::vector<ImageBrowserImpl*> ImageBrowsersSnapshot() const
        {
            std::vector<ImageBrowserImpl*> out;
            auto bus = m_eventBus.lock();
            if (!bus)
            {
                return out;
            }

            const auto browsers = bus->ImageBrowsersSnapshot();
            out.reserve(browsers.size());
            for (auto* b : browsers)
            {
                if (b != nullptr)
                {
                    out.push_back(static_cast<ImageBrowserImpl*>(b));
                }
            }
            return out;
        }

        void HandleBusEvent(const Ficture2Backplate::ImageBrowserEvent& ev)
        {
            if (ev.source == this)
            {
                return;
            }

            switch (ev.type)
            {
            case Ficture2Backplate::ImageBrowserEvent::Type::ThumbStripHeightChanged:
                g_syncedThumbStripHeight = ev.thumbStripHeight;
                g_hasSyncedThumbStripHeight = true;
                (void)ApplySyncedThumbStripHeightIfNeeded(true);
                if (BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestLayout();
                }
                break;
            case Ficture2Backplate::ImageBrowserEvent::Type::FileNameSelected:
                OnSyncedFileNameSelected(ev.fileNameLower);
                break;
            case Ficture2Backplate::ImageBrowserEvent::Type::ViewTransformChanged:
                if (!ev.fileNameLower.empty())
                {
                    const std::wstring otherNameLower = ActiveMainFileNameLower();
                    if (!otherNameLower.empty() && otherNameLower == ev.fileNameLower)
                    {
                        ApplySyncedViewTransform(ev.viewTransform);
                    }
                }
                break;
            case Ficture2Backplate::ImageBrowserEvent::Type::ShowNavItemsChanged:
                ApplyShowNavItems(ev.boolValue);
                break;
            case Ficture2Backplate::ImageBrowserEvent::Type::BackgroundColorChanged:
                ApplyBrowserBackgroundColor(ev.color);
                break;
            case Ficture2Backplate::ImageBrowserEvent::Type::FocusedBackgroundColorChanged:
                ApplyFocusedBackgroundColor(ev.color);
                break;
            case Ficture2Backplate::ImageBrowserEvent::Type::AlphaCheckerboardChanged:
                ApplyAlphaCheckerboard(ev.boolValue);
                break;
            default:
                break;
            }
        }

        void ApplyShowNavItems(bool showNavItems)
        {
            m_showNavItems = showNavItems;

            VirtualPath prefer {};
            if (m_selectedIndex < m_items.size())
            {
                prefer = m_items[m_selectedIndex].path;
            }
            RebuildThumbList(prefer);
        }

        void ApplyBrowserBackgroundColor(const D2D1_COLOR_F& color)
        {
            m_browserBackgroundColor = color;
        }

        void ApplyFocusedBackgroundColor(const D2D1_COLOR_F& color)
        {
            m_browserFocusedBackgroundColor = color;
        }

        void ApplyAlphaCheckerboard(bool checkerEnabled)
        {
            if (m_mainImage)
            {
                m_mainImage->SetAlphaCheckerboardEnabled(checkerEnabled);
            }

            // Keep info bars in sync.
            RefreshInfoPanel();
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            m_desired = available;
            return m_desired;
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            Wnd::Arrange(finalRect);

            if (m_selectedFocus)
            {
                const D2D1_RECT_F focusRect = m_selectedFocus->LayoutRect();
                m_thumbScroll->EnsureCentered(focusRect, true);
            }

        }

        void OnRenderD3D(ID3D11DeviceContext* context) override
        {
            // Per-ImageBrowser background (stationary, never pans with the image).
            // Draw in the D3D pass so it stays behind GPU-rendered images.
            FD2D::Backplate* bp = BackplateRef();
            if (bp != nullptr && bp->D3DDevice() != nullptr && m_mainImage != nullptr)
            {
                const D2D1_RECT_F r = m_mainImage->LayoutRect();
                if (r.right > r.left && r.bottom > r.top)
                {
                    const D2D1_COLOR_F baseBg = m_browserBackgroundColor;
                    const D2D1_COLOR_F focusedBg = m_browserFocusedBackgroundColor;
                    const D2D1_COLOR_F bg = HasFocus() ? focusedBg : baseBg;
                    (void)bp->ClearRectD3D(r, bg);

                    // Ensure the main image backdrop matches the ImageBrowser background
                    // so focus background doesn't "edge shift" when panning.
                    if (m_mainImage)
                    {
                        m_mainImage->SetBackdropColor(bg);
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
            (void)ApplySyncedThumbStripHeightIfNeeded();
            (void)UpdateThumbSizingFromPane();
            RefreshInfoPanel();

            // D2D-only backend: fill per-ImageBrowser background before drawing children.
            // (On the D3D swapchain backend, D2D runs after the GPU image pass, so we must NOT fill here.)
            if (target != nullptr)
            {
                FD2D::Backplate* bp = BackplateRef();
                const bool d3dActive = (bp != nullptr && bp->D3DDevice() != nullptr);
                if (!d3dActive && m_mainImage != nullptr)
                {
                    const D2D1_RECT_F r = m_mainImage->LayoutRect();
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

                        if (m_mainImage)
                        {
                            m_mainImage->SetBackdropColor(bg);
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
            if (m_dragOverlay != ImageBrowserDragOverlay::Kind::None && m_mainImage != nullptr)
            {
                if (!m_dragOverlayLayer)
                {
                    m_dragOverlayLayer = std::make_unique<ImageBrowserDragOverlay>();
                }
                m_dragOverlayLayer->Draw(target, m_mainImage->LayoutRect(), m_dragOverlay);
            }

            // Draw folder icon in main image area if a folder is selected
            if (m_selectedIndex < m_items.size() &&
                (m_items[m_selectedIndex].kind == ThumbItemKind::Folder || m_items[m_selectedIndex].kind == ThumbItemKind::Up) &&
                m_mainImage != nullptr)
            {
                const D2D1_RECT_F r = m_mainImage->LayoutRect();
                if (r.right > r.left && r.bottom > r.top)
                {
                    const float w = r.right - r.left;
                    const float h = r.bottom - r.top;
                    const float iconSize = (std::min)(w, h) * 0.30f;
                    const float iconX = r.left + (w - iconSize) * 0.5f;
                    const float iconY = r.top + (h - iconSize) * 0.5f;

                    if (EnsureFolderBitmap(target))
                    {
                        const D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + iconSize, iconY + iconSize);
                        target->DrawBitmap(
                            m_folderBitmap.Get(),
                            dst,
                            1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                            D2D1::RectF(0.0f, 0.0f, m_folderBitmap->GetSize().width, m_folderBitmap->GetSize().height));
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

            case WM_RBUTTONUP:
                return HandleContextMenuMessage(lParam);

            case WM_FIC2_DEFERRED_ACTION:
                return HandleDeferredActionMessage();

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

        bool HandleContextMenuMessage(LPARAM lParam)
        {
            const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (!RectContainsPoint(LayoutRect(), pt))
            {
                return false;
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

            g_contextMenuBrowser = this;

            if (m_mainImage)
            {
                auto vt = m_mainImage->GetViewTransform();
                vt.targetZoomScale = vt.zoomScale;
                vt.zoomVelocity = 0.0f;
                m_mainImage->SetViewTransform(vt, false /*notify*/);
            }

            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            HMENU hMenu = LoadMenuW(instance, MAKEINTRESOURCEW(IDR_MENU_IMAGEBROWSER_CONTEXT));
            if (hMenu != nullptr)
            {
                HMENU hPopup = GetSubMenu(hMenu, 0);
                if (hPopup != nullptr)
                {
                    // Dynamic state: enable/disable and toggle labels.
                    const int viewerCount = HorizontalViewerCount();
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

                    auto mainImage = ActiveMainImage();
                    const bool highQuality = mainImage ? mainImage->HighQualitySampling() : true;
                    const std::wstring samplingLabel = SamplingLabel(highQuality, backplate);
                    ModifyMenuW(
                        hPopup,
                        IDM_CTX_TOGGLE_SAMPLING,
                        MF_BYCOMMAND | MF_STRING,
                        IDM_CTX_TOGGLE_SAMPLING,
                        (std::wstring(L"Sampling: ") + samplingLabel + L"\tQ").c_str());

                    const bool thumbRegistered = FICture2App::IsThumbnailProviderRegistered();
                    ModifyMenuW(
                        hPopup,
                        IDM_CTX_REGISTER_THUMBNAIL_PROVIDER,
                        MF_BYCOMMAND | MF_STRING,
                        thumbRegistered ? IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER : IDM_CTX_REGISTER_THUMBNAIL_PROVIDER,
                        thumbRegistered
                            ? L"Unregister &Thumbnail Provider (Admin)..."
                            : L"Register &Thumbnail Provider (Admin)...");

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

        bool HandleMouseWheelMessage(WPARAM wParam, LPARAM lParam)
        {
            if (!m_inputController)
            {
                m_inputController = std::make_unique<ImageBrowserInputController>();
            }

            ImageBrowserInputController::MouseWheelContext ctx {};
            ctx.items = &m_items;
            ctx.selectedIndex = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            if (m_mainImage != nullptr)
            {
                ctx.hasMainRect = true;
                ctx.mainRect = m_mainImage->LayoutRect();
            }
            if (m_thumbScroll != nullptr)
            {
                ctx.hasThumbRect = true;
                ctx.thumbRect = m_thumbScroll->LayoutRect();
            }
            ctx.requestFocus = [this]()
            {
                RequestFocus();
            };
            ctx.selectIndex = [this](size_t idx)
            {
                SelectItemByIndex(idx);
            };

            return m_inputController->HandleMouseWheel(ctx, wParam, lParam);
        }

        bool HandleKeyDownMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (!m_inputController)
            {
                m_inputController = std::make_unique<ImageBrowserInputController>();
            }

            ImageBrowserInputController::KeyContext ctx {};
            ctx.items = &m_items;
            ctx.selectedIndex = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            ctx.selectIndex = [this](size_t idx)
            {
                SelectItemByIndex(idx);
            };

            return m_inputController->HandleKeyDown(ctx, message, wParam, lParam);
        }

        bool HandleKeyUpMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (!m_inputController)
            {
                m_inputController = std::make_unique<ImageBrowserInputController>();
            }

            ImageBrowserInputController::KeyContext ctx {};
            ctx.items = &m_items;
            ctx.selectedIndex = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            ctx.thumbW = m_thumbW;
            ctx.thumbOuterSpacing = m_thumbOuterSpacing;
            if (m_thumbScroll != nullptr)
            {
                ctx.hasThumbRect = true;
                ctx.thumbRect = m_thumbScroll->LayoutRect();
            }
            ctx.selectIndex = [this](size_t idx)
            {
                SelectItemByIndex(idx);
            };
            ctx.activateSelected = [this]()
            {
                ActivateSelected();
            };
            ctx.queueNavigateUp = [this]()
            {
                QueueNavigateUp();
            };
            ctx.queueToggleNavItems = [this]()
            {
                QueueToggleNavItems();
            };
            ctx.toggleAlphaCheckerboard = [this]()
            {
                ToggleAlphaCheckerboard();
            };
            ctx.fitToScreen = [this]()
            {
                FitToScreen();
            };
            ctx.pickBackgroundColor = [this]()
            {
                PickAndApplyBackgroundColor();
            };
            ctx.toggleSamplingQuality = [this]()
            {
                RequestFocus();
                if (m_mainImage)
                {
                    m_mainImage->ToggleSamplingQuality();
                    RefreshInfoPanel();
                }
            };
            ctx.closeHorizontalThisBrowser = [this]()
            {
                QueueCloseHorizontalThisBrowser();
            };
            ctx.openFileReplace = [this]()
                {
                    OpenFileDialog(OpenDialogMode::ReplaceCurrent);
            };
            ctx.openFileSplit = [this]()
            {
                OpenFileDialog(OpenDialogMode::SplitHorizontalNewBrowser);
            };

            return m_inputController->HandleKeyUp(ctx, message, wParam, lParam);
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
            if (m_mainImage == nullptr)
            {
                return false;
            }

            ClearDragOverlay();

            if (!m_dragDrop)
            {
                m_dragDrop = std::make_unique<ImageBrowserDragDrop>();
            }

            ImageBrowserDragDrop::Action action {};
            if (!m_dragDrop->HandleFileDrop(path, clientPt, m_mainImage->LayoutRect(), action))
            {
                return false;
            }

            switch (action.kind)
            {
            case ImageBrowserDragDrop::ActionKind::InsertHorizontal:
                QueueInsertHorizontalWithPathAfterThis(action.path);
                return true;
            case ImageBrowserDragDrop::ActionKind::NavigateToFolder:
                QueueDeferredAction(DeferredActionKind::NavigateToFolder, action.path);
            return true;
            case ImageBrowserDragDrop::ActionKind::NavigateToFile:
                QueueDeferredAction(DeferredActionKind::NavigateToFile, action.path);
                return true;
            default:
                return false;
            }
        }

        bool OnFileDrag(const std::wstring& path, const POINT& clientPt, FD2D::FileDragVisual& outVisual) override
        {
            // Let child panes handle first (for compare mode where this browser hosts other browsers).
            if (Wnd::OnFileDrag(path, clientPt, outVisual))
            {
                return true;
            }

            if (m_mainImage == nullptr)
            {
                outVisual = FD2D::FileDragVisual::None;
                ClearDragOverlay();
                return false;
            }

            if (!m_dragDrop)
            {
                m_dragDrop = std::make_unique<ImageBrowserDragDrop>();
            }

            if (!m_dragDrop->HandleFileDrag(path, clientPt, m_mainImage->LayoutRect(), outVisual, m_dragOverlay))
            {
                outVisual = FD2D::FileDragVisual::None;
                ClearDragOverlay();
                return false;
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

            auto vp = VirtualPath::Parse(incomingFilePath);
            if (vp)
            {
                SplitHorizontalWithFile(*vp);
            }
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

            auto vp = VirtualPath::Parse(filePath);
            if (!vp || !vp->Exists())
            {
                return;
            }

            auto parent = vp->GetParent();
            m_currentFolder = parent;

            RebuildThumbList(*vp);

            // Select/apply exact match if present.
            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].kind == ThumbItemKind::Image && m_items[i].path == *vp)
                {
                    SelectItemByIndex(i);
                    return;
                }
            }

            // Fallback: show first image in folder.
            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].kind == ThumbItemKind::Image)
                {
                    SelectItemByIndex(i);
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
            auto vp = VirtualPath::Parse(folderPath);
            if (vp)
            {
                NavigateToFolder(*vp);
            }
        }

        void AddHorizontalViewerForRestore(const std::wstring& filePath)
        {
            if (filePath.empty())
            {
                return;
            }
            auto vp = VirtualPath::Parse(filePath);
            if (vp)
            {
                SplitHorizontalWithFile(*vp);
            }
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
            auto newWnd = CreateImageBrowser(childName, L"");
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
            (void)ApplySyncedThumbStripHeightIfNeeded(true);
        }

        void OpenAdditionalFileInHorizontalSplit(const VirtualPath& filePath)
        {
            if (VirtualFileSystem::IsDirectory(filePath) || filePath.IsArchiveFile())
            {
                InsertHorizontalWithPathAfterName(L"", filePath);
            }
            else
            {
                SplitHorizontalWithFile(filePath);
            }
        }

        void OpenAdditionalFilesSideBySideAfterName(
            const std::vector<std::wstring>& filePaths,
            const std::wstring& afterName)
        {
            if (filePaths.empty())
            {
                return;
            }

            if (g_rootHorizontalHostBrowser != nullptr && g_rootHorizontalHostBrowser != this)
            {
                g_rootHorizontalHostBrowser->OpenAdditionalFilesSideBySideAfterName(filePaths, afterName);
                return;
            }

            EnsureHorizontalHost();
            if (m_hPanes.empty())
            {
                return;
            }

            auto insertPath = [this, &afterName](const std::wstring& path)
            {
                if (path.empty())
                {
                    return;
                }
                if (static_cast<int>(m_hPanes.size()) >= 4)
                {
                    return;
                }
                InsertHorizontalWithPathAfterName(afterName, VirtualPath::FromFilesystem(path));
            };

            if (afterName.empty())
            {
                for (const auto& path : filePaths)
                {
                    insertPath(path);
                }
            }
            else
            {
                for (auto it = filePaths.rbegin(); it != filePaths.rend(); ++it)
                {
                    insertPath(*it);
                }
            }
        }

    private:
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

        bool EnsureFolderBitmap(ID2D1RenderTarget* target)
        {
            if (target == nullptr)
            {
                return false;
            }

            if (m_folderBitmap && m_folderBitmapTarget == target)
            {
                return true;
            }

            m_folderBitmap.Reset();
            m_folderBitmapTarget = nullptr;

            HMODULE module = GetModuleHandleW(nullptr);
            HRSRC hrsrc = FindResourceW(module, MAKEINTRESOURCEW(IDR_PNG_FOLDER), RT_RCDATA);
            if (!hrsrc)
            {
                return false;
            }

            HGLOBAL hglob = LoadResource(module, hrsrc);
            if (!hglob)
            {
                return false;
            }

            void* data = LockResource(hglob);
            DWORD size = SizeofResource(module, hrsrc);
            if (!data || size == 0)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
            HRESULT hr = CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wic));
            if (FAILED(hr) || !wic)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IWICStream> stream;
            hr = wic->CreateStream(&stream);
            if (FAILED(hr) || !stream)
            {
                return false;
            }

            hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(data), size);
            if (FAILED(hr))
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
            if (FAILED(hr) || !decoder)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            hr = decoder->GetFrame(0, &frame);
            if (FAILED(hr) || !frame)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            hr = wic->CreateFormatConverter(&converter);
            if (FAILED(hr) || !converter)
            {
                return false;
            }

            hr = converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
            if (FAILED(hr))
            {
                return false;
            }

            hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &m_folderBitmap);
            if (FAILED(hr) || !m_folderBitmap)
            {
                return false;
            }

            m_folderBitmapTarget = target;
            return true;
        }

        bool UpdateThumbSizingFromPane()
        {
            if (!m_thumbScroll)
            {
                return false;
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
                return false;
            }

            // Keep thumbnail spacing constant; only scale the thumbnail square with the pane height.
            constexpr float contentPadding = 4.0f; // matches thumbs->SetPadding(4)
            const float availableForThumb = paneH - (contentPadding * 2.0f);

            float newSide = availableForThumb;
            newSide = (std::max)(32.0f, (std::min)(256.0f, newSide));

            // Avoid thrashing while dragging the splitter.
            if (std::abs(newSide - m_thumbW) < 1.0f)
            {
                return false;
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
            return true;
        }

        void BuildUi()
        {
            OutputDebugStringW(L"[ImageBrowser] BuildUi: Starting UI construction\n");
            
            // Root: vertical split (main pane + thumb strip)
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
            
            OutputDebugStringW((L"[ImageBrowser] BuildUi: rootSplit created, ratio=0.85, minH=" + std::to_wstring(kThumbStripMinH) + L", maxH=" + std::to_wstring(kThumbStripMaxH) + L"\n").c_str());

            BuildMainPanes();

            if (!m_thumbStripController)
            {
                m_thumbStripController = std::make_unique<ImageBrowserThumbStripController>();
            }
            auto build = m_thumbStripController->Build(rootSplit);
            m_thumbScroll = build.scroll;
            m_thumbPanel = build.panel;

            OutputDebugStringW(L"[ImageBrowser] BuildUi: thumbScroll created and set as SecondChild\n");

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

                auto bus = m_eventBus.lock();
                if (bus)
                {
                    Ficture2Backplate::ImageBrowserEvent ev {};
                    ev.type = Ficture2Backplate::ImageBrowserEvent::Type::ThumbStripHeightChanged;
                    ev.source = this;
                    ev.thumbStripHeight = g_syncedThumbStripHeight;
                    bus->Publish(ev);
                }

                if (BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestLayout();
                }
            });

            constexpr float thumbW = 128.0f;
            constexpr float thumbH = 128.0f;
            m_thumbW = thumbW;
            m_thumbH = thumbH;
            m_thumbLabelDip = 0.0f;
            m_thumbItemSpacing = 0.0f;
            m_thumbOuterSpacing = 8.0f;

            OutputDebugStringW(L"[ImageBrowser] BuildUi: m_thumbPanel assigned\n");

            if (!m_initialFile.empty())
            {
                OutputDebugStringW((L"[ImageBrowser] BuildUi: Initializing with file: " + m_initialFile + L"\n").c_str());
                auto parsed = VirtualPath::Parse(m_initialFile);
                if (parsed)
                {
                    if (parsed->IsArchiveFile() && !parsed->IsInArchive())
                    {
                        m_currentFolder = *parsed;
                        RebuildThumbList(VirtualPath());
                    }
                    else
                    {
                        m_currentFolder = parsed->GetParent();
                        RebuildThumbList(*parsed);
                    }
                }
            }
            else
            {
                OutputDebugStringW(L"[ImageBrowser] BuildUi: No initial file, starting empty\n");
                // Start empty; a session restore or user navigation will populate.
                m_currentFolder = VirtualPath();
                RebuildThumbList(VirtualPath());
            }

            OutputDebugStringW(L"[ImageBrowser] BuildUi: UI construction complete\n");
        }

        bool ApplySyncedThumbStripHeightIfNeeded(bool force = false)
        {
            if (!g_hasSyncedThumbStripHeight || m_rootSplit == nullptr)
            {
                return false;
            }

            if (m_rootSplit->Orientation() != FD2D::SplitterOrientation::Vertical)
            {
                return false;
            }

            const D2D1_RECT_F rootR = m_rootSplit->LayoutRect();
            const float totalH = (std::max)(0.0f, rootR.bottom - rootR.top);
            if (totalH <= 0.0f)
            {
                return false;
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
                        return false;
                    }
                }
            }

            m_rootSplit->SetSplitRatio((std::max)(0.0f, (std::min)(1.0f, ratio)));

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
            return true;
        }

        void BuildMainPanes()
        {
            if (!m_rootSplit)
            {
                return;
            }

            if (!m_mainPane)
            {
                m_mainPane = std::make_unique<ImageBrowserMainPane>();
            }

            m_mainPane->Build(
                m_rootSplit,
                m_initialFile,
                [this](const FD2D::Image::ViewTransform& vt)
                {
                    OnActiveMainViewChanged(vt);
                },
                [this]()
                {
                    RequestFocus();
                },
                [this](FD2D::MainImage& mainImage)
                {
                    ApplyIniToMainImage(mainImage);
                });

            m_mainImage = m_mainPane->MainImage();

                    if (!m_initialFile.empty())
                    {
                m_mainPath = m_initialFile;
            }

        }

        std::wstring ActiveMainPath() const
        {
            return m_mainPath;
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
            if (!m_mainPane)
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
            if (m_selectedIndex < m_items.size() &&
                (m_items[m_selectedIndex].kind == ThumbItemKind::Folder || m_items[m_selectedIndex].kind == ThumbItemKind::Up))
            {
                if (m_items[m_selectedIndex].kind == ThumbItemKind::Up && !m_currentFolder.empty())
                {
                    displayedFullPath = m_currentFolder.GetDisplayPath();
                }
                else
                {
                    displayedFullPath = m_items[m_selectedIndex].path.GetDisplayPath();
                }
            }

            const std::wstring pathDisp = displayedFullPath.empty() ? L"-" : displayedFullPath;
            const bool isFolderSelected = (m_selectedIndex < m_items.size() &&
                (m_items[m_selectedIndex].kind == ThumbItemKind::Folder || m_items[m_selectedIndex].kind == ThumbItemKind::Up));
            const std::wstring archiveLabel = ArchiveFormatLabelForPath(pathDisp);

            uint32_t w = 0;
            uint32_t h = 0;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            if (isFolderSelected)
            {
                w = 0;
                h = 0;
                fmt = DXGI_FORMAT_UNKNOWN;
            }
            else if (main)
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
            if (!isFolderSelected && w > 0 && h > 0)
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
            if (isFolderSelected)
            {
                text = archiveLabel.empty() ? L"-" : archiveLabel;
            }
            else
            {
                text += dim;
                text += L" | ";
                text += DxgiFormatToString(fmt);
                if (!archiveLabel.empty())
                {
                    text += L" | ";
                    text += archiveLabel;
                }
                if (m_mainImage)
                {
                    text += L" | ";
                    text += SamplingLabel(m_mainImage->HighQualitySampling(), BackplateRef());
                }
            }

            const std::wstring zoomText = std::to_wstring(zoomPct) + L"%";
            m_mainPane->UpdateInfo(pathDisp, text, zoomText);
        }

        void ApplyIniToMainImage(FD2D::MainImage& mainImage)
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

        std::shared_ptr<FD2D::MainImage> ActiveMainImage() const
        {
            return m_mainImage;
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

            mainImage->SetInteractionEnabled(true);
            mainImage->SetLoadingSpinnerEnabled(true);
            const std::wstring p = m_items[index].path.wstring();
            mainImage->SetSourceFile(p);
            m_mainPath = p;
            mainImage->Invalidate();
            RefreshInfoPanel();
        }

        void SelectItemByIndex(size_t index)
        {
            if (!m_thumbStripController)
            {
                m_thumbStripController = std::make_unique<ImageBrowserThumbStripController>();
            }

            m_thumbStripController->SelectItemByIndex(
                m_items,
                m_selectedIndex,
                m_selectedFocus,
                m_thumbScroll,
                m_mainImage,
                m_mainPath,
                m_currentFolder,
                m_mainPane.get(),
                m_syncSuppressBroadcast,
                ImageBrowserCount(),
                [this](size_t idx)
                {
                    ApplyMainFromIndex(idx);
                },
                [this]()
                {
                    RefreshInfoPanel();
                },
                [this](const std::wstring& fileNameLower)
                {
                    auto bus = m_eventBus.lock();
                    if (bus)
                    {
                        Ficture2Backplate::ImageBrowserEvent ev {};
                        ev.type = Ficture2Backplate::ImageBrowserEvent::Type::FileNameSelected;
                        ev.source = this;
                        ev.fileNameLower = fileNameLower;
                        bus->Publish(ev);
                    }
                },
                index);
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
            SelectItemByIndex(match);
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
            if (ImageBrowserCount() < 2)
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

            auto bus = m_eventBus.lock();
            if (bus)
            {
                Ficture2Backplate::ImageBrowserEvent ev {};
                ev.type = Ficture2Backplate::ImageBrowserEvent::Type::ViewTransformChanged;
                ev.source = this;
                ev.fileNameLower = myNameLower;
                ev.viewTransform = vt;
                bus->Publish(ev);
            }
        }

        void ApplySyncedViewTransform(const FD2D::Image::ViewTransform& vt)
        {
            auto main = ActiveMainImage();
            if (!main)
            {
                return;
            }

            // Sync should mirror the *currently displayed* view state from the focused source.
            // Do NOT re-run per-pane spring animation on receivers (it can cause subtle jitter
            // due to different frame timing/layout across panes).
            FD2D::Image::ViewTransform applied = vt;
            applied.targetZoomScale = applied.zoomScale;
            applied.zoomVelocity = 0.0f;

            m_viewSyncSuppressBroadcast = true;
            main->SetViewTransform(applied, false /*notify*/);
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

            ApplyShowNavItems(g_showNavItems);

            auto bus = m_eventBus.lock();
            if (bus)
            {
                Ficture2Backplate::ImageBrowserEvent ev {};
                ev.type = Ficture2Backplate::ImageBrowserEvent::Type::ShowNavItemsChanged;
                ev.source = this;
                ev.boolValue = g_showNavItems;
                bus->Publish(ev);
            }
        }

        void QueueNavigateUp()
        {
            QueueDeferredAction(DeferredActionKind::NavigateUp);
        }

        void NavigateUp()
        {
            if (!m_navigation)
            {
                m_navigation = std::make_unique<ImageBrowserNavigation>();
            }

            (void)m_navigation->NavigateUp(
                m_currentFolder,
                [this](const VirtualPath& folder)
                {
                    NavigateToFolder(folder);
                });
        }

        void ActivateSelectedImpl()
        {
            if (!m_navigation)
            {
                m_navigation = std::make_unique<ImageBrowserNavigation>();
            }

            (void)m_navigation->ActivateSelected(
                m_selectedIndex,
                m_items,
                [this](size_t index)
                {
                    ApplyMainFromIndex(index);
                },
                [this](const VirtualPath& folder)
                {
                    NavigateToFolder(folder);
                });
        }

        void NavigateToFolder(const VirtualPath& folder)
        {
            if (!m_navigation)
            {
                m_navigation = std::make_unique<ImageBrowserNavigation>();
            }

            (void)m_navigation->NavigateToFolder(
                folder,
                [](const VirtualPath& p)
                {
                    return VirtualFileSystem::IsDirectory(p);
                },
                m_currentFolder,
                [this](const VirtualPath& prefer)
                {
                    RebuildThumbList(prefer);
                },
                [this]()
                {
            if (m_thumbScroll)
            {
                m_thumbScroll->SetScrollX(0.0f);
            }
                },
                [this](size_t index)
                {
                    SelectItemByIndex(index);
                },
                m_items);
        }

        void NavigateToFile(const VirtualPath& filePath)
        {
            if (!m_navigation)
            {
                m_navigation = std::make_unique<ImageBrowserNavigation>();
            }

            (void)m_navigation->NavigateToFile(
                filePath,
                [](const VirtualPath& p)
                {
                    return p.Exists();
                },
                [](const std::wstring& p)
                {
                    return ImageCore::DecoderRegistry::Instance().IsSupportedPath(p);
                },
                [](const VirtualPath& p)
                {
                    return VirtualFileSystem::IsDirectory(p);
                },
                m_currentFolder,
                [this](const VirtualPath& prefer)
                {
                    RebuildThumbList(prefer);
                });
        }

        void RebuildThumbList(const VirtualPath& preferSelectPath)
        {
            if (!m_thumbPanel)
            {
                return;
            }

            m_items.clear();
            m_selectedIndex = static_cast<size_t>(-1);
            m_selectedFocus.reset();

            if (!m_thumbStripController)
            {
                m_thumbStripController = std::make_unique<ImageBrowserThumbStripController>();
            }

            auto result = m_thumbStripController->RebuildList(
                m_thumbPanel,
                m_items,
                m_thumbW,
                m_thumbH,
                m_showNavItems,
                m_currentFolder,
                preferSelectPath,
                [this]()
                    {
                        QueueNavigateUp();
                },
                [this](const VirtualPath& dir)
                    {
                        QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                },
                [this](size_t index)
                {
                    RequestFocus();
                    SelectItemByIndex(index);
                },
                [](const VirtualPath& a, const VirtualPath& b)
                {
                    return PathEqualsInsensitive(a, b);
                },
                [this](const wchar_t* prefix, const VirtualPath& p)
                {
                    return MakeStableThumbName(prefix, p);
                },
                [](const VirtualPath& p)
                {
                    return ImageCore::DecoderRegistry::Instance().IsSupportedPath(p.GetDisplayPath());
                });

            // Restore selection without scrolling yet; layout hasn't been updated.
            if (!result.hasItems)
            {
                // No items: clear main image and path
                if (m_mainImage)
                {
                    m_mainImage->ClearSource();
                    m_mainImage->SetInteractionEnabled(false);
                }
                m_mainPath.clear();
                RefreshInfoPanel();
            }
            else
            {
                SelectItemByIndex(result.selectIndex);
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
            if (!m_currentFolder.empty() && m_currentFolder.IsFilesystemPath())
            {
                if (std::filesystem::exists(m_currentFolder.hostPath) && std::filesystem::is_directory(m_currentFolder.hostPath))
                {
                    initialDir = m_currentFolder.hostPath.wstring();
                ofn.lpstrInitialDir = initialDir.c_str();
                }
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
            QueueDeferredAction(mode == OpenDialogMode::SplitHorizontalNewBrowser ? DeferredActionKind::SplitHorizontalWithFile : DeferredActionKind::NavigateToFile, VirtualPath::FromFilesystem(chosen));
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
            if (!m_currentFolder.empty() && m_currentFolder.IsFilesystemPath())
            {
                if (std::filesystem::exists(m_currentFolder.hostPath) && std::filesystem::is_directory(m_currentFolder.hostPath))
                {
                    initialDir = m_currentFolder.hostPath.wstring();
                ofn.lpstrInitialDir = initialDir.c_str();
                }
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
                        QueueInsertHorizontalWithPathAfterThis(VirtualPath::FromFilesystem(chosen));
                    }
                }
                break;
            case IDM_CTX_REGISTER_ASSOCIATIONS:
                {
                    FD2D::Backplate* bp = BackplateRef();
                    HWND hwnd = bp ? bp->Window() : nullptr;
                    FICture2App::RegisterSupportedFileAssociations(hwnd);
                }
                break;
            case IDM_CTX_REGISTER_THUMBNAIL_PROVIDER:
                {
                    FD2D::Backplate* bp = BackplateRef();
                    HWND hwnd = bp ? bp->Window() : nullptr;
                    FICture2App::RegisterThumbnailProvider(hwnd, false);
                }
                break;
            case IDM_CTX_UNREGISTER_THUMBNAIL_PROVIDER:
                {
                    FD2D::Backplate* bp = BackplateRef();
                    HWND hwnd = bp ? bp->Window() : nullptr;
                    FICture2App::RegisterThumbnailProvider(hwnd, true);
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
            case IDM_CTX_TOGGLE_SAMPLING:
                RequestFocus();
                if (m_mainImage)
                {
                    m_mainImage->ToggleSamplingQuality();
                    RefreshInfoPanel();
                }
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
            ApplyBrowserBackgroundColor(next);
            auto bus = m_eventBus.lock();
            if (bus)
            {
                Ficture2Backplate::ImageBrowserEvent ev {};
                ev.type = Ficture2Backplate::ImageBrowserEvent::Type::BackgroundColorChanged;
                ev.source = this;
                ev.color = next;
                bus->Publish(ev);
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

            ApplyFocusedBackgroundColor(g_focusedBrowserBackgroundColor);
            auto bus = m_eventBus.lock();
            if (bus)
            {
                Ficture2Backplate::ImageBrowserEvent ev {};
                ev.type = Ficture2Backplate::ImageBrowserEvent::Type::FocusedBackgroundColorChanged;
                ev.source = this;
                ev.color = g_focusedBrowserBackgroundColor;
                bus->Publish(ev);
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

            ApplyAlphaCheckerboard(checkerEnabled);
            auto bus = m_eventBus.lock();
            if (bus)
            {
                Ficture2Backplate::ImageBrowserEvent ev {};
                ev.type = Ficture2Backplate::ImageBrowserEvent::Type::AlphaCheckerboardChanged;
                ev.source = this;
                ev.boolValue = checkerEnabled;
                bus->Publish(ev);
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

        void QueueDeferredAction(DeferredActionKind kind, const VirtualPath& path = {})
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

        void QueueInsertHorizontalWithPathAfterThis(const VirtualPath& path)
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
            const VirtualPath path = m_deferredPath;
            const std::wstring text = m_deferredText;
            m_deferredKind = DeferredActionKind::None;
            m_deferredPath = VirtualPath();
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

        void InsertHorizontalWithPathAfterName(const std::wstring& afterName, const VirtualPath& path)
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
            auto newWnd = CreateImageBrowser(childName, L"");
            auto newBrowser = std::dynamic_pointer_cast<ImageBrowserImpl>(newWnd);
            if (newBrowser)
            {
                if (VirtualFileSystem::IsDirectory(path))
                {
                    newBrowser->RestoreOpenFolder(path.GetDisplayPath());
                }
                else
                {
                    newBrowser->RestoreOpenFile(path.GetDisplayPath());
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

        void SplitHorizontalWithFile(const VirtualPath& filePath)
        {
            if (filePath.hostPath.empty())
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
            auto newBrowser = CreateImageBrowser(childName, filePath.wstring());
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

        std::shared_ptr<FD2D::SplitPanel> m_rootSplit {};
        std::unique_ptr<ImageBrowserMainPane> m_mainPane {};
        std::shared_ptr<FD2D::MainImage> m_mainImage {};
        std::wstring m_mainPath {};

        std::shared_ptr<FD2D::Wnd> m_selectedFocus {};
        std::unique_ptr<ImageBrowserDragDrop> m_dragDrop {};
        std::unique_ptr<ImageBrowserThumbStripController> m_thumbStripController {};
        std::unique_ptr<ImageBrowserNavigation> m_navigation {};
        std::unique_ptr<ImageBrowserInputController> m_inputController {};
        std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
        std::shared_ptr<FD2D::StackPanel> m_thumbPanel {};
        std::vector<ThumbItem> m_items {};
        size_t m_selectedIndex { static_cast<size_t>(-1) };
        ULONGLONG m_lastKeyNavMs { 0 };
        int m_thumbWheelRemainder { 0 };

        VirtualPath m_currentFolder {};
        bool m_showNavItems { true };
        float m_thumbW { 128.0f };
        float m_thumbH { 128.0f };
        float m_thumbLabelDip { 0.0f };
        float m_thumbItemSpacing { 0.0f };
        float m_thumbOuterSpacing { 8.0f }; // spacing between thumbnail "items" in the horizontal strip

        DeferredActionKind m_deferredKind { DeferredActionKind::None };
        VirtualPath m_deferredPath {};
        std::wstring m_deferredText {};

        std::wstring m_initialFile {};
        std::vector<std::shared_ptr<FD2D::Wnd>> m_hPanes {};
        std::shared_ptr<FD2D::Wnd> m_hHost {};

        // (no focus-background state)
        bool m_syncSuppressBroadcast { false };
        bool m_viewSyncSuppressBroadcast { false };
        std::weak_ptr<Ficture2Backplate::EventBus> m_eventBus {};
        Ficture2Backplate::EventBus::HandlerId m_eventBusToken { 0 };

        void ClearDragOverlay()
        {
            if (m_dragOverlay != ImageBrowserDragOverlay::Kind::None)
            {
                m_dragOverlay = ImageBrowserDragOverlay::Kind::None;
                Invalidate();
            }
        }

        std::unique_ptr<ImageBrowserDragOverlay> m_dragOverlayLayer {};
        ImageBrowserDragOverlay::Kind m_dragOverlay { ImageBrowserDragOverlay::Kind::None };
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_browserBgBrush {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
        ID2D1RenderTarget* m_folderBitmapTarget { nullptr };

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

std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, const std::wstring& initialFile)
{
    return std::make_shared<ImageBrowserImpl>(name, initialFile);
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

void ImageBrowser_OpenAdditionalFilesSideBySide(const std::vector<std::wstring>& filePaths)
{
    if (g_rootHorizontalHostBrowser == nullptr || filePaths.empty())
    {
        return;
    }

    size_t existing = g_rootHorizontalHostBrowser->ImageBrowsersSnapshot().size();
    for (const auto& path : filePaths)
    {
        if (path.empty())
        {
            continue;
        }

        if (existing >= 4)
        {
            break;
        }

        g_rootHorizontalHostBrowser->OpenAdditionalFileInHorizontalSplit(VirtualPath::FromFilesystem(path));
        existing = g_rootHorizontalHostBrowser->ImageBrowsersSnapshot().size();
    }
}

void ImageBrowser_OpenAdditionalFilesSideBySideAfter(
    const std::vector<std::wstring>& filePaths,
    const std::wstring& afterName)
{
    if (g_rootHorizontalHostBrowser == nullptr || filePaths.empty())
    {
        return;
    }

    g_rootHorizontalHostBrowser->OpenAdditionalFilesSideBySideAfterName(filePaths, afterName);
}

void ImageBrowser_SaveSessionToIni(const std::wstring& iniFile)
{
    if (iniFile.empty() || g_rootHorizontalHostBrowser == nullptr)
    {
        return;
    }

    // Save viewer count + per-viewer displayed file.
    const std::vector<ImageBrowserImpl*> browsers = g_rootHorizontalHostBrowser->ImageBrowsersSnapshot();
    const int count = static_cast<int>((std::min)(static_cast<size_t>(4), browsers.size()));
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
        auto* b = browsers[static_cast<size_t>(i)];
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
        const std::vector<ImageBrowserImpl*> browsers = g_rootHorizontalHostBrowser->ImageBrowsersSnapshot();
        for (auto* b : browsers)
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