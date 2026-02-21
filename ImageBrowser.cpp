#include "ImageBrowser.h"
#include "Version.h"

#include "framework.h"
#include "Resource.h"
#include "ThumbNavTile.h"
#include "ThumbImageTile.h"
#include "ImageBrowserMainPane.h"
#include "ImageBrowserThumbnailPane.h"
#include "ImageBrowserAsyncThumbLoader.h"
#include "ImageBrowserSplitCoordinator.h"
#include "ImageBrowserDeferredActions.h"
#include "ImageBrowserDragDrop.h"
#include "ImageBrowserDragOverlay.h"
#include "ImageBrowserThumbTypes.h"
#include "ImageBrowserThumbStripController.h"
#include "IpcCompareRequest.h"
#include "Ficture2Backplate.h"
#include "AppSetup.h"

#include "FD2D/FD2D.h"
#include "FD2D/Core.h"
#include "FD2D/MainImage.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageDecodeDispatcher.h"
#include "ImageCore/ImageCore.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <memory>
#include <thread>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <cwctype>
#include <wrl/client.h>
#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>

namespace
{
    class ImageBrowserImpl;

    constexpr float kThumbStripPadding = 8.0f; // matches thumbs->SetPadding(8)
    constexpr float kThumbMinSide = 32.0f;
    constexpr float kThumbMaxSide = 256.0f;
    constexpr float kThumbStripMinH = (kThumbStripPadding * 2.0f) + kThumbMinSide;
    constexpr float kThumbStripMaxH = (kThumbStripPadding * 2.0f) + kThumbMaxSide;
    constexpr float kSplitPanelDefaultHitThickness = 12.0f; // Splitter::m_hitAreaThickness default


    static unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

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

    static std::wstring NormalizeInnerPathLowerForCompare(const std::wstring& inner)
    {
        std::wstring s = inner;
        for (auto& c : s)
        {
            if (c == L'\\')
            {
                c = L'/';
            }
            c = static_cast<wchar_t>(towlower(c));
        }
        while (!s.empty() && (s.back() == L'/' || s.back() == L'\\'))
        {
            s.pop_back();
        }
        return s;
    }

    static bool PathEqualsInsensitive(const VirtualPath& a, const VirtualPath& b)
    {
        return NormalizePathLowerForCompare(a.hostPath) == NormalizePathLowerForCompare(b.hostPath) &&
            NormalizeInnerPathLowerForCompare(a.archiveInnerPath) == NormalizeInnerPathLowerForCompare(b.archiveInnerPath);
    }

    static std::vector<std::wstring> GetSupportedImageExtensions()
    {
        return ImageCore::ImageDecodeDispatcher::GetSupportedExtensions();
    }

    static std::wstring BuildSupportedImageDialogFilter()
    {
        const std::vector<std::wstring> exts = GetSupportedImageExtensions();
        std::wostringstream patternBuilder {};
        for (size_t i = 0; i < exts.size(); ++i)
        {
            if (i != 0)
            {
                patternBuilder << L";";
            }
            patternBuilder << L"*" << exts[i];
        }
        std::wstring wildcardPattern = patternBuilder.str();
        if (wildcardPattern.empty())
        {
            wildcardPattern = L"*.*";
        }

        std::wstring filter {};
        filter.reserve(256 + wildcardPattern.size() * 2);
        filter += L"Supported images (" + wildcardPattern + L")";
        filter.push_back(L'\0');
        filter += wildcardPattern;
        filter.push_back(L'\0');
        filter += L"All files (*.*)";
        filter.push_back(L'\0');
        filter += L"*.*";
        filter.push_back(L'\0');
        filter.push_back(L'\0');
        return filter;
    }

    static bool TryGetIniFilePath(std::wstring& outIniFile)
    {
        outIniFile = FICture2App::GetIniFilePath();
        return !outIniFile.empty();
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

    static bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt)
    {
        return pt.x >= r.left &&
            pt.x <= r.right &&
            pt.y >= r.top &&
            pt.y <= r.bottom;
    }

    static FD2D::Wnd* ResolveRootBrowserWndFromBus(const std::shared_ptr<Ficture2Backplate::EventBus>& bus)
    {
        if (!bus)
        {
            return nullptr;
        }

        const auto browsers = bus->ImageBrowsersSnapshot();
        if (browsers.empty())
        {
            return nullptr;
        }

        return browsers.front();
    }

    class ImageBrowserImpl : public FD2D::Wnd, public IImageBrowserOps
    {
    public:
        enum class OpenDialogMode;

        explicit ImageBrowserImpl(const std::wstring& name, const std::wstring& initialFile = L"")
            : Wnd(name)
            , m_initialFile(initialFile)
        {
            m_asyncThumbReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (m_asyncThumbReadyEvent != nullptr)
            {
                ImageBrowserAsyncThumbLoader::RegisterBrowser(Name(), m_asyncThumbReadyEvent);
            }
            BuildUi();
        }

        ~ImageBrowserImpl() override
        {
            ImageBrowserAsyncThumbLoader::UnregisterBrowser(Name());
            if (m_asyncThumbReadyEvent != nullptr)
            {
                CloseHandle(m_asyncThumbReadyEvent);
                m_asyncThumbReadyEvent = nullptr;
            }
            UnregisterFromEventBus();
        }

        void OnAttached(FD2D::Backplate& backplate) override
        {
            Wnd::OnAttached(backplate);
            RegisterWithEventBus();
            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                ficBp->EnsureImageBrowserIniInitialized();
                m_showNavItems = ficBp->ShowNavItemsEnabled();
                m_browserFocusedBackgroundColor = ficBp->FocusedBackgroundColor();
                ApplyShowNavItems(m_showNavItems);
                ApplyAlphaCheckerboard(ficBp->AlphaCheckerboardEnabled());
            }
            // Default per-ImageBrowser background follows current global clear color.
            m_browserBackgroundColor = backplate.ClearColor();
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

        Ficture2Backplate* FictureBackplateRef() const
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr)
            {
                return nullptr;
            }

            return dynamic_cast<Ficture2Backplate*>(bp);
        }

        bool TryGetSyncedThumbStripHeight(float& outHeight) const
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp == nullptr)
            {
                return false;
            }

            return ficBp->TryGetSyncedThumbStripHeight(outHeight);
        }

        void SetSyncedThumbStripHeight(float height)
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp == nullptr)
            {
                return;
            }

            ficBp->SetSyncedThumbStripHeight(height);
        }

        std::shared_ptr<Ficture2Backplate::EventBus> EventBusRef() const
        {
            auto* ficBp = FictureBackplateRef();
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
        }

        void UnregisterFromEventBus()
        {
            auto bus = m_eventBus.lock();
            if (!bus)
            {
                return;
            }

            bus->UnregisterImageBrowser(this);

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

        static bool ContainsDescendantWnd(const FD2D::Wnd* root, const FD2D::Wnd* target)
        {
            if (root == nullptr || target == nullptr)
            {
                return false;
            }

            for (const auto& child : root->ChildrenInOrder())
            {
                if (!child)
                {
                    continue;
                }
                if (child.get() == target || ContainsDescendantWnd(child.get(), target))
                {
                    return true;
                }
            }
            return false;
        }

        bool HasFocusWithinBrowser() const
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr)
            {
                return false;
            }

            FD2D::Wnd* focused = bp->FocusedWnd();
            if (focused == nullptr)
            {
                return false;
            }

            if (focused == this)
            {
                return true;
            }

            // Focus highlighting should follow this browser's main pane subtree only.
            // Using the whole ImageBrowser subtree can over-match when this browser hosts
            // other browsers (split host), causing unrelated panes to appear focused.
            return (m_mainPane != nullptr) && ContainsDescendantWnd(m_mainPane.get(), focused);
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            m_desired = available;
            return m_desired;
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            Wnd::Arrange(finalRect);

            if (m_selectedFocus && m_thumbPane)
            {
                const D2D1_RECT_F focusRect = m_selectedFocus->LayoutRect();
                m_thumbPane->EnsureCentered(focusRect, true);
            }

        }

        void OnRenderD3D(ID3D11DeviceContext* context) override
        {
            // Per-ImageBrowser background (stationary, never pans with the image).
            // Draw in the D3D pass so it stays behind GPU-rendered images.
            FD2D::Backplate* bp = BackplateRef();
            const bool paneFocused = HasFocusWithinBrowser();
            if (bp != nullptr && bp->D3DDevice() != nullptr)
            {
                const D2D1_COLOR_F bg = paneFocused ? m_browserFocusedBackgroundColor : m_browserBackgroundColor;
                (void)bp->ClearRectD3D(LayoutRect(), bg);
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
            if (m_thumbListLoading && m_asyncThumbReadyEvent != nullptr)
            {
                if (WaitForSingleObject(m_asyncThumbReadyEvent, 0) == WAIT_OBJECT_0)
                {
                    DrainAsyncThumbChunks();
                }
                if (BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestAnimationFrame();
                }
            }
            // Throttle progressive UI list updates so large folders don't monopolize the UI thread.
            if (m_progressiveUiDirty && !m_progressiveLoadCompleted)
            {
                const unsigned long long now = NowMs();
                if (now - m_progressiveLastApplyMs >= 50)
                {
                    ApplyProgressiveThumbUpdate(false);
                }
            }
            if (m_pendingThumbStripBroadcast && m_rootSplit != nullptr && !m_rootSplit->IsSplitterDragging())
            {
                m_pendingThumbStripBroadcast = false;
                NotifyThumbStripHeightChanged(m_pendingThumbStripHeight);
            }
            RefreshInfoPanel();

            // D2D-only backend: fill per-ImageBrowser background before drawing children.
            // (On the D3D swapchain backend, D2D runs after the GPU image pass, so we must NOT fill here.)
            if (target != nullptr)
            {
                FD2D::Backplate* bp = BackplateRef();
                const bool d3dActive = (bp != nullptr && bp->D3DDevice() != nullptr);
                const bool paneFocused = HasFocusWithinBrowser();
                if (!d3dActive)
                {
                    const D2D1_COLOR_F bg = paneFocused ? m_browserFocusedBackgroundColor : m_browserBackgroundColor;
                    if (!m_browserBackgroundBrush)
                    {
                        (void)target->CreateSolidColorBrush(bg, m_browserBackgroundBrush.ReleaseAndGetAddressOf());
                    }
                    if (m_browserBackgroundBrush)
                    {
                        m_browserBackgroundBrush->SetColor(bg);
                        target->FillRectangle(LayoutRect(), m_browserBackgroundBrush.Get());
                    }
                }
            }

            Wnd::OnRender(target);

            if (target == nullptr)
            {
                return;
            }

            // Drag&drop overlay (drawn on top of the main image area only).
            if (m_dragOverlay != ImageBrowserDragOverlay::Kind::None && m_mainPane != nullptr)
            {
                if (!m_dragOverlayLayer)
                {
                    m_dragOverlayLayer = std::make_unique<ImageBrowserDragOverlay>(target);
                }
                m_mainPane->RenderOnMainRect([this, target](const D2D1_RECT_F& mainRect)
                {
                    m_dragOverlayLayer->Draw(target, mainRect, m_dragOverlay);
                });
            }

            // Draw folder icon in main image area if a folder is selected
            if (m_selectedIndex < m_items.size() &&
                (m_items[m_selectedIndex].kind == ThumbItemKind::Folder || m_items[m_selectedIndex].kind == ThumbItemKind::Up) &&
                m_mainPane != nullptr)
            {
                if (EnsureFolderBitmap(target))
                {
                    m_mainPane->RenderCenteredMainOverlayBitmap(target, m_folderBitmap.Get(), 0.30f);
                }
            }
        }

        bool OnInputEvent(const FD2D::InputEvent& event) override
        {
            if (TryHandleInputEvent(event))
            {
                return true;
            }

            return Wnd::OnInputEvent(event);
        }

        bool TryHandleInputEvent(const FD2D::InputEvent& event)
        {
            return HandleInputType(event.type, event);
        }

        bool HandleInputType(FD2D::InputEventType type, const FD2D::InputEvent& event)
        {
            switch (type)
            {
            case FD2D::InputEventType::KeyDown:
                return HandleKeyDownMessage(event);
            case FD2D::InputEventType::KeyUp:
                return HandleKeyUpMessage(event);
            default:
                return false;
            }
        }

        bool OnCommandEvent(const FD2D::CommandEvent& event) override
        {
            if (TryHandleCommandEvent(event))
            {
                return true;
            }

            return Wnd::OnCommandEvent(event);
        }

        bool TryHandleCommandEvent(const FD2D::CommandEvent& event)
        {
            return HandleCommandId(event.id, event.lParam);
        }

        bool HandleCommandId(UINT id, LPARAM lParam)
        {
            switch (id)
            {
            case CMD_FIC2_IPC_COMPARE:
                return HandleIpcCompareMessage(lParam);
            case CMD_FIC2_DEFERRED_ACTION:
                return HandleDeferredActionMessage();
            case CMD_FIC2_ASYNC_THUMB_READY:
                return HandleAsyncThumbReadyMessage();
            default:
                return false;
            }
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

        bool HandleContextMenuMessage(const POINT& pt)
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp == nullptr)
            {
                return false;
            }

            return ficBp->ShowImageBrowserContextMenu(this, pt);
        }

        VirtualPath GetContextMenuTargetImagePathAtPoint(const POINT& pt) const
        {
            for (const auto& item : m_items)
            {
                if (item.kind != ThumbItemKind::Image || !item.focus)
                {
                    continue;
                }
                if (RectContainsPoint(item.focus->LayoutRect(), pt))
                {
                    return item.path;
                }
            }

            if (m_selectedIndex < m_items.size() && m_items[m_selectedIndex].kind == ThumbItemKind::Image)
            {
                return m_items[m_selectedIndex].path;
            }
            return VirtualPath();
        }

        void ShowImagePathInExplorer(const VirtualPath& target)
        {
            if (target.empty())
            {
                return;
            }

            const std::filesystem::path nativePath = target.hostPath;
            if (nativePath.empty())
            {
                return;
            }

            const std::wstring params = L"/select,\"" + nativePath.wstring() + L"\"";
            const HINSTANCE result = ShellExecuteW(
                BackplateRef() ? BackplateRef()->Window() : nullptr,
                L"open",
                L"explorer.exe",
                params.c_str(),
                nullptr,
                SW_SHOWNORMAL);
            if (reinterpret_cast<intptr_t>(result) <= 32)
            {
                MessageBoxW(
                    BackplateRef() ? BackplateRef()->Window() : nullptr,
                    L"Failed to open Windows Explorer for the selected image.",
                    L"FICture2",
                    MB_OK | MB_ICONWARNING);
            }
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

        size_t CurrentThumbSelectionIndex() const
        {
            return (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
        }

        void ApplyThumbWheelStep(int steps)
        {
            if (steps == 0)
            {
                return;
            }

            const int dir = (steps > 0) ? -1 : 1;
            const int count = std::abs(steps);
            const size_t cur = CurrentThumbSelectionIndex();
            size_t next = cur;
            for (int s = 0; s < count; ++s)
            {
                if (dir < 0)
                {
                    if (next == 0)
                    {
                        break;
                    }
                    next--;
                }
                else
                {
                    if ((next + 1) >= m_items.size())
                    {
                        break;
                    }
                    next++;
                }
            }

            if (next != cur)
            {
                SelectItemByIndex(next);
            }
        }

        bool HandleKeyDownMessage(const FD2D::InputEvent& event)
        {
            auto* ficBp = FictureBackplateRef();
            bool handledByBackplate = false;
            if (ficBp != nullptr &&
                ficBp->HandleImageBrowserKeyDownCommand(
                    this,
                    event.keyCode,
                    event.modifiers.control,
                    event.modifiers.shift,
                    event.modifiers.alt,
                    handledByBackplate))
            {
                return true;
            }
            if (handledByBackplate)
            {
                return false;
            }

            return HandleTypeToSelectKeyDown(event);
        }

        bool HandleTypeToSelectKeyDown(const FD2D::InputEvent& event)
        {
            if (m_items.empty())
            {
                return false;
            }

            if (event.modifiers.control || event.modifiers.alt)
            {
                return false;
            }

            const unsigned long long now = NowMs();
            if (now - m_typeSelectLastInputMs > 1200)
            {
                m_typeSelectQuery.clear();
            }

            // Let navigation/edit/system keys flow to normal key handlers.
            if (IsNavigationOrSystemKey(event.keyCode))
            {
                return false;
            }

            wchar_t ch = 0;
            if (!TryGetPrintableKey(event, ch))
            {
                return false;
            }

            std::wstring nextQuery = m_typeSelectQuery;
            nextQuery.push_back(ch);

            if (!TrySelectByTypeToSelectQuery(nextQuery) && nextQuery.size() > 1)
            {
                nextQuery.assign(1, ch);
                (void)TrySelectByTypeToSelectQuery(nextQuery);
            }

            m_typeSelectQuery = nextQuery;
            m_typeSelectLastInputMs = now;
            return true;
        }

        bool TrySelectByTypeToSelectQuery(const std::wstring& query)
        {
            if (query.empty() || m_items.empty())
            {
                return false;
            }

            const size_t count = m_items.size();
            size_t start = 0;
            if (m_selectedIndex < count)
            {
                start = (m_selectedIndex + 1) % count;
            }

            for (size_t offset = 0; offset < count; ++offset)
            {
                const size_t idx = (start + offset) % count;
                if (StartsWithInsensitive(TypeToSelectItemLabel(idx), query))
                {
                    SelectItemByIndex(idx);
                    return true;
                }
            }

            return false;
        }

        bool IsNavigationOrSystemKey(UINT keyCode) const
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

        bool TryGetPrintableKey(const FD2D::InputEvent& event, wchar_t& outChar) const
        {
            wchar_t chars[4] {};
            BYTE keyState[256] {};
            if (!GetKeyboardState(keyState))
            {
                return false;
            }

            const UINT scanCode = event.scanCode;
            int converted = ToUnicode(event.keyCode, scanCode, keyState, chars, 4, 0);
            if (converted < 0)
            {
                wchar_t clearBuf[4] {};
                (void)ToUnicode(event.keyCode, scanCode, keyState, clearBuf, 4, 0);
                return false;
            }
            if (converted <= 0)
            {
                return false;
            }

            wchar_t ch = chars[0];
            if (!iswprint(ch))
            {
                return false;
            }
            outChar = static_cast<wchar_t>(towlower(ch));
            return true;
        }

        static bool StartsWithInsensitive(const std::wstring& text, const std::wstring& prefix)
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

        std::wstring TypeToSelectItemLabel(size_t index) const
        {
            if (index >= m_items.size())
            {
                return L"";
            }

            const ThumbItem& item = m_items[index];
            if (item.kind == ThumbItemKind::Up)
            {
                return L"..";
            }
            return item.path.GetFilename();
        }

        bool HandleKeyUpMessage(const FD2D::InputEvent& event)
        {
            const auto& modifiers = event.modifiers;
            auto* ficBp = FictureBackplateRef();
            bool handledByBackplate = false;
            if (ficBp != nullptr &&
                ficBp->HandleImageBrowserKeyUpCommand(
                    this,
                    event.keyCode,
                    modifiers.control,
                    modifiers.shift,
                    modifiers.alt,
                    handledByBackplate))
            {
                return true;
            }
            if (handledByBackplate)
            {
                return false;
            }
            return false;
        }

        size_t PagingStepFromThumbViewport() const
        {
            if (m_thumbPane == nullptr)
            {
                return 1;
            }
            const float itemExtent = (std::max)(1.0f, m_thumbW + m_thumbOuterSpacing);
            return m_thumbPane->PagingStep(itemExtent);
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
            D2D1_RECT_F mainRect {};
            if (!m_mainPane || !m_mainPane->TryGetMainRectForPoint(clientPt, mainRect))
            {
                return false;
            }

            ClearDragOverlay();
            const ImageBrowserDragDrop dragDrop {};
            ImageBrowserDragDrop::Action action {};
            if (!dragDrop.HandleFileDrop(path, clientPt, mainRect, action))
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

            D2D1_RECT_F mainRect {};
            if (!m_mainPane || !m_mainPane->TryGetMainRectForPoint(clientPt, mainRect))
            {
                outVisual = FD2D::FileDragVisual::None;
                ClearDragOverlay();
                return false;
            }

            const ImageBrowserDragDrop dragDrop {};
            if (!dragDrop.HandleFileDrag(path, clientPt, mainRect, outVisual, m_dragOverlay))
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
            if (auto* rootHost = DelegatedRootHostBrowser())
            {
                rootHost->AddHorizontalViewerForRestoreFolder(folderPath);
                return;
            }

            if (!EnsureHorizontalHostReady(true))
            {
                return;
            }

            const std::wstring childName = ImageBrowserSplitCoordinator::NextSplitBrowserName();
            auto newWnd = CreateImageBrowser(childName, L"");
            auto newBrowser = std::dynamic_pointer_cast<ImageBrowserImpl>(newWnd);
            if (newBrowser != nullptr)
            {
                newBrowser->RestoreOpenFolder(folderPath);
            }
            m_hPanes.push_back(newWnd);

            RefreshHorizontalHostLayout();
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

            if (auto* rootHost = DelegatedRootHostBrowser())
            {
                rootHost->OpenAdditionalFilesSideBySideAfterName(filePaths, afterName);
                return;
            }

            if (!EnsureHorizontalHostReady())
            {
                return;
            }

            if (afterName.empty())
            {
                for (const auto& path : filePaths)
                {
                    if (!TryInsertHorizontalPathAfterName(afterName, path))
                    {
                        break;
                    }
                }
            }
            else
            {
                for (auto it = filePaths.rbegin(); it != filePaths.rend(); ++it)
                {
                    if (!TryInsertHorizontalPathAfterName(afterName, *it))
                    {
                        break;
                    }
                }
            }
        }

    private:
        static constexpr ULONGLONG kKeyRepeatMinIntervalMs = 60;
        static constexpr UINT CMD_FIC2_DEFERRED_ACTION = WM_APP + 0x7A11;
        static constexpr UINT CMD_FIC2_IPC_COMPARE = WM_APP + 0x7A12;
        static constexpr UINT CMD_FIC2_ASYNC_THUMB_READY = WM_APP + 0x7A13;

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
            float paneH = 0.0f;
            if (m_thumbPane == nullptr || !m_thumbPane->TryGetStripHeight(paneH))
            {
                // Fall back to backplate-synced height only when local layout is not ready yet.
                (void)TryGetSyncedThumbStripHeight(paneH);
            }
            if (paneH <= 1.0f)
            {
                return false;
            }

            // Keep thumbnail spacing constant; only scale the thumbnail height with the pane height.
            // Controller uses thumbs->SetPadding(4), so keep this value aligned.
            constexpr float contentPadding = 4.0f;
            const float availableForThumb = paneH - (contentPadding * 2.0f);

            // Use continuous sizing so splitter drag is reflected immediately.
            // Coarse snap steps (e.g. 96/128/192) make the strip look "stuck" at one size.
            float newHeight = (std::max)(kThumbMinSide, (std::min)(kThumbMaxSide, availableForThumb));

            // Quantize lightly to reduce jitter from fractional layout values.
            newHeight = std::round(newHeight);

            const bool splitterDragging = (m_rootSplit != nullptr && m_rootSplit->IsSplitterDragging());
            const float minDelta = splitterDragging ? 1.5f : 0.5f;

            // While dragging splitter, limit expensive thumbnail retargeting frequency.
            if (splitterDragging)
            {
                const unsigned long long now = NowMs();
                if ((now - m_lastThumbSizingApplyMs) < 33ULL && std::abs(newHeight - m_thumbH) < 6.0f)
                {
                    return false;
                }
            }

            // Ignore tiny sub-pixel changes only.
            if (std::abs(newHeight - m_thumbH) < minDelta)
            {
                return false;
            }

            m_thumbW = newHeight; // Keep this for navigation tiles
            m_thumbH = newHeight;

            if (m_thumbPane)
            {
                m_thumbPane->SetScrollStep((std::max)(48.0f, newHeight * 0.75f));
            }

            for (auto& item : m_items)
            {
                if (item.imageTile)
                {
                    // Use variable width based on aspect ratio
                    item.imageTile->SetFixedHeight(m_thumbH);
                }
                if (item.navTile)
                {
                    // Navigation tiles remain square
                    item.navTile->SetFixedSize({ m_thumbW, m_thumbH });
                    item.navTile->Invalidate();
                }
            }

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
            m_lastThumbSizingApplyMs = NowMs();
            return true;
        }

        void NotifyThumbStripHeightChanged(float height)
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp == nullptr)
            {
                return;
            }

            ficBp->SynchronizeThumbStripHeight(this, height);
        }

        bool BrowserTryStartCompareWithFileNameMatch(const std::wstring& incomingFilePath) override
        {
            return TryStartCompareWithFileNameMatch(incomingFilePath);
        }

        void BrowserRestoreOpenFile(const std::wstring& filePath) override
        {
            RestoreOpenFile(filePath);
        }

        void BrowserOpenAdditionalFileInHorizontalSplit(const std::wstring& filePath) override
        {
            OpenAdditionalFileInHorizontalSplit(VirtualPath::FromFilesystem(filePath));
        }

        void BrowserOpenAdditionalFilesSideBySideAfterName(
            const std::vector<std::wstring>& filePaths,
            const std::wstring& afterName) override
        {
            OpenAdditionalFilesSideBySideAfterName(filePaths, afterName);
        }

        std::wstring BrowserGetDisplayedFilePath() const override
        {
            return GetDisplayedFilePath();
        }

        std::wstring BrowserGetCurrentFolderPath() const override
        {
            return GetCurrentFolderPath();
        }

        std::vector<float> BrowserCaptureHorizontalSplitRatios() const override
        {
            return CaptureHorizontalSplitRatios();
        }

        void BrowserRestoreOpenFolder(const std::wstring& folderPath) override
        {
            RestoreOpenFolder(folderPath);
        }

        void BrowserAddHorizontalViewerForRestore(const std::wstring& filePath) override
        {
            AddHorizontalViewerForRestore(filePath);
        }

        void BrowserAddHorizontalViewerForRestoreFolder(const std::wstring& folderPath) override
        {
            AddHorizontalViewerForRestoreFolder(folderPath);
        }

        void BrowserForceApplySyncedThumbStripHeight() override
        {
            ForceApplySyncedThumbStripHeight();
        }

        void BrowserApplyHorizontalSplitRatios(const std::vector<float>& ratios) override
        {
            ApplyHorizontalSplitRatios(ratios);
        }

        void BrowserSelectFileNameForSync(const std::wstring& fileNameLower) override
        {
            OnSyncedFileNameSelected(fileNameLower);
        }

        std::wstring BrowserGetActiveFileNameLower() const override
        {
            return ActiveMainFileNameLower();
        }

        void BrowserApplyViewTransformForSync(const FD2D::Image::ViewTransform& vt) override
        {
            ApplySyncedViewTransform(vt);
        }

        void BrowserApplyShowNavItemsForSync(bool showNavItems) override
        {
            ApplyShowNavItems(showNavItems);
        }

        void BrowserApplyBackgroundColorForSync(const D2D1_COLOR_F& color) override
        {
            ApplyBrowserBackgroundColor(color);
        }

        void BrowserApplyFocusedBackgroundColorForSync(const D2D1_COLOR_F& color) override
        {
            ApplyFocusedBackgroundColor(color);
        }

        void BrowserApplyAlphaCheckerboardForSync(bool checkerEnabled) override
        {
            ApplyAlphaCheckerboard(checkerEnabled);
        }

        // Command domain: file/viewer lifecycle.
        void BrowserCmdOpenImage() override
        {
            OpenFileDialog(OpenDialogMode::ReplaceCurrent);
        }

        void BrowserCmdOpenImageSplitNew() override
        {
            OpenFileDialog(OpenDialogMode::SplitHorizontalNewBrowser);
        }

        void BrowserCmdOpenNewImage() override
        {
            HandleOpenNewImageCommand();
        }

        // Command domain: selection/navigation.
        void BrowserCmdActivateSelected() override
        {
            if (!m_items.empty() && m_selectedIndex < m_items.size())
            {
                ActivateSelected();
            }
        }

        void BrowserCmdSelectPrevious() override
        {
            const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            const size_t next = (cur == 0) ? 0 : (cur - 1);
            SelectItemByIndex(next);
        }

        void BrowserCmdSelectNext() override
        {
            const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            SelectItemByIndex(cur + 1);
        }

        void BrowserCmdSelectFirst() override
        {
            SelectItemByIndex(0);
        }

        void BrowserCmdSelectLast() override
        {
            if (!m_items.empty())
            {
                SelectItemByIndex(m_items.size() - 1);
            }
        }

        void BrowserCmdPagePrevious() override
        {
            const size_t step = PagingStepFromThumbViewport();
            const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            const size_t next = (cur > step) ? (cur - step) : 0;
            SelectItemByIndex(next);
        }

        void BrowserCmdPageNext() override
        {
            const size_t step = PagingStepFromThumbViewport();
            const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
            const size_t next = cur + step;
            SelectItemByIndex(next);
        }

        void BrowserCmdNavigateUp() override
        {
            QueueNavigateUp();
        }

        // Command domain: file/viewer lifecycle.
        void BrowserCmdClose() override
        {
            QueueCloseHorizontalThisBrowser();
        }

        // Command domain: view/appearance toggles.
        void BrowserCmdBackgroundColor() override
        {
            PickAndApplyBackgroundColor();
        }

        void BrowserCmdFocusedBackgroundColor() override
        {
            PickAndApplyFocusedBackgroundColor();
        }

        void BrowserCmdFitToScreen() override
        {
            FitToScreen();
        }

        void BrowserCmdToggleDirectories() override
        {
            QueueToggleNavItems();
        }

        void BrowserCmdToggleAlpha() override
        {
            ToggleAlphaCheckerboard();
        }

        void BrowserCmdToggleSampling() override
        {
            ToggleSamplingQualityFromContextMenu();
        }

        // Command domain: integration/system actions.
        void BrowserCmdShowInExplorerAtPoint(const POINT& ptClient) override
        {
            ShowImagePathInExplorer(GetContextMenuTargetImagePathAtPoint(ptClient));
        }

        void BrowserCmdRegisterAssociations() override
        {
#if FICTURE2_ENABLE_REGISTRATION_MENU
            FICture2App::RegisterSupportedFileAssociations(ContextMenuOwnerWindow());
#endif
        }

        void BrowserCmdRegisterThumbnailProvider() override
        {
#if FICTURE2_ENABLE_REGISTRATION_MENU
            FICture2App::RegisterThumbnailProvider(ContextMenuOwnerWindow(), false);
#endif
        }

        void BrowserCmdUnregisterThumbnailProvider() override
        {
#if FICTURE2_ENABLE_REGISTRATION_MENU
            FICture2App::RegisterThumbnailProvider(ContextMenuOwnerWindow(), true);
#endif
        }

        bool BrowserContextMenuPrepareForDisplay(const POINT& ptClient) override
        {
            if (m_mainPane == nullptr || !m_mainPane->ContainsMainPoint(ptClient))
            {
                return false;
            }

            m_mainPane->PauseMainImageViewAnimation();
            return true;
        }

        ContextMenuSnapshot BrowserContextMenuSnapshotAtPoint(const POINT& ptClient) const override
        {
            ContextMenuSnapshot snapshot {};
            snapshot.showNavItems = m_showNavItems;
            auto mainImage = ActiveMainImage();
            snapshot.highQualitySampling = mainImage ? mainImage->HighQualitySampling() : true;
            snapshot.hasExplorerTarget = !GetContextMenuTargetImagePathAtPoint(ptClient).empty();
            return snapshot;
        }

        bool BrowserHasFocusForTitle() const override
        {
            return HasFocus();
        }

        std::wstring BrowserSelectedImageFileNameForTitle() const override
        {
            return SelectedImageFileNameForTitle();
        }

        void BuildUi()
        {
#if defined(_DEBUG)
            OutputDebugStringW(L"[ImageBrowser] BuildUi: Starting UI construction\n");
#endif

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

#if defined(_DEBUG)
            OutputDebugStringW((L"[ImageBrowser] BuildUi: rootSplit created, ratio=0.85, minH=" + std::to_wstring(kThumbStripMinH) + L", maxH=" + std::to_wstring(kThumbStripMaxH) + L"\n").c_str());
#endif

            BuildMainPanes();

            if (!m_thumbPane)
            {
                m_thumbPane = std::make_shared<ImageBrowserThumbnailPane>();
            }
            m_thumbPane->Build(
                rootSplit,
                [this](int steps)
                {
                    ApplyThumbWheelStep(steps);
                },
                [this]()
                {
                    RequestFocus();
                });

#if defined(_DEBUG)
            OutputDebugStringW(L"[ImageBrowser] BuildUi: thumbScroll created and set as SecondChild\n");
#endif

            rootSplit->OnSplitChanged([this](float)
            {
                float h = 0.0f;
                if (m_thumbPane == nullptr || !m_thumbPane->TryGetStripHeight(h))
                {
                    return;
                }

                const float clampedHeight = (std::max)(kThumbStripMinH, (std::min)(kThumbStripMaxH, h));
                SetSyncedThumbStripHeight(clampedHeight);
                const bool splitterDragging = (m_rootSplit != nullptr && m_rootSplit->IsSplitterDragging());
                if (splitterDragging)
                {
                    m_pendingThumbStripBroadcast = true;
                    m_pendingThumbStripHeight = clampedHeight;
                    return;
                }

                NotifyThumbStripHeightChanged(clampedHeight);
            });

            constexpr float thumbW = 128.0f;
            constexpr float thumbH = 128.0f;
            m_thumbW = thumbW;
            m_thumbH = thumbH;
            m_thumbLabelDip = 0.0f;
            m_thumbItemSpacing = 0.0f;
            m_thumbOuterSpacing = 8.0f;

#if defined(_DEBUG)
            OutputDebugStringW(L"[ImageBrowser] BuildUi: thumbnail pane assigned\n");
#endif

            if (!m_initialFile.empty())
            {
#if defined(_DEBUG)
                OutputDebugStringW((L"[ImageBrowser] BuildUi: Initializing with file: " + m_initialFile + L"\n").c_str());
#endif
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
#if defined(_DEBUG)
                OutputDebugStringW(L"[ImageBrowser] BuildUi: No initial file, starting empty\n");
#endif
                // Start empty; a session restore or user navigation will populate.
                m_currentFolder = VirtualPath();
                RebuildThumbList(VirtualPath());
            }

#if defined(_DEBUG)
            OutputDebugStringW(L"[ImageBrowser] BuildUi: UI construction complete\n");
#endif
        }

        bool ApplySyncedThumbStripHeightIfNeeded(bool force = false)
        {
            float syncedHeight = 0.0f;
            if (!TryGetSyncedThumbStripHeight(syncedHeight) || m_rootSplit == nullptr)
            {
                return false;
            }

            if (m_rootSplit->Orientation() != FD2D::SplitterOrientation::Vertical)
            {
                return false;
            }

            // Don't update layout during rendering to prevent recursion
            FD2D::Backplate* bp = BackplateRef();
            if (!force && bp && bp->IsRendering())
            {
                return false;
            }

            const D2D1_RECT_F rootR = m_rootSplit->LayoutRect();
            const float totalH = (std::max)(0.0f, rootR.bottom - rootR.top);
            if (totalH <= 0.0f)
            {
                return false;
            }

            const float desiredSecond = (std::max)(kThumbStripMinH, (std::min)(kThumbStripMaxH, syncedHeight));

            // Estimate available height like SplitPanel::Arrange (childArea minus splitter thickness).
            const float availableH = (std::max)(1.0f, totalH - kSplitPanelDefaultHitThickness);
            const float ratio = 1.0f - (desiredSecond / availableH);

            if (!force)
            {
                // If we're already close, avoid churning layouts.
                float cur = 0.0f;
                if (m_thumbPane != nullptr && m_thumbPane->TryGetStripHeight(cur))
                {
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
                m_mainPane = std::make_shared<ImageBrowserMainPane>();
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
                },
                [this](const POINT& pt)
                {
                    return HandleContextMenuMessage(pt);
                },
                [this]()
                {
                    RequestFocus();
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

    public:
        std::wstring SelectedImageFileNameForTitle() const
        {
            if (m_selectedIndex < m_items.size() && m_items[m_selectedIndex].kind == ThumbItemKind::Image)
            {
                return m_items[m_selectedIndex].path.filename().wstring();
            }
            return L"";
        }

    private:
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
                    if (auto* ficBp = FictureBackplateRef())
                    {
                        text += ficBp->SamplingLabelForRenderer(m_mainImage->HighQualitySampling());
                    }
                    else
                    {
                        const FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
                        text += (m_mainImage->HighQualitySampling()
                            ? ((d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D HQ Cubic" : L"D2D Linear")
                            : ((d2dVersion >= FD2D::D2DVersion::D2D1_1) ? L"D2D Nearest" : L"D2D Linear"));
                    }
                }
            }

            const std::wstring zoomText = std::to_wstring(zoomPct) + L"%";
            m_mainPane->UpdateInfo(pathDisp, text, zoomText);
        }

        void ApplyIniToMainImage(FD2D::MainImage& mainImage)
        {
            std::wstring iniFile {};
            if (TryGetIniFilePath(iniFile))
            {
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

        ImageBrowserThumbStripController::SelectItemContext MakeThumbSelectContext()
        {
            ImageBrowserThumbStripController::SelectItemContext context {};
            context.items = &m_items;
            context.selectedIndex = &m_selectedIndex;
            context.selectedFocus = &m_selectedFocus;
            context.ensureSelectionVisible = [this]()
            {
                if (m_thumbPane == nullptr || !m_selectedFocus)
                {
                    return;
                }

                D2D1_RECT_F stripRect {};
                const D2D1_RECT_F focusRect = m_selectedFocus->LayoutRect();
                const bool layoutReady =
                    m_thumbPane->TryGetStripRect(stripRect) &&
                    (focusRect.right > focusRect.left) &&
                    (focusRect.bottom > focusRect.top);
                if (layoutReady)
                {
                    m_thumbPane->EnsureCentered(focusRect);
                }
            };
            context.syncSuppressBroadcast = m_syncSuppressBroadcast;
            context.imageBrowserCount = ImageBrowserCount();
            context.applyMainFromIndex = [this](size_t idx)
            {
                ApplyMainFromIndex(idx);
            };
            context.applyNonImageSelection = [this](const ThumbItem& selectedItem)
            {
                if (m_mainImage)
                {
                    m_mainImage->ClearSource();
                    m_mainImage->SetInteractionEnabled(false);
                    m_mainImage->Invalidate();
                }

                if (selectedItem.kind == ThumbItemKind::Up && !m_currentFolder.empty())
                {
                    m_mainPath = m_currentFolder.GetDisplayPath();
                }
                else
                {
                    m_mainPath = selectedItem.path.GetDisplayPath();
                }

                if (m_mainPane)
                {
                    m_mainPane->ResetInfoCache();
                }

                RefreshInfoPanel();
            };
            context.publishFileName = [this](const std::wstring& fileNameLower)
            {
                auto* ficBp = FictureBackplateRef();
                if (ficBp != nullptr)
                {
                    ficBp->SynchronizeFileSelection(this, fileNameLower);
                }
            };
            return context;
        }

        void SelectItemByIndex(size_t index)
        {
            auto context = MakeThumbSelectContext();
            m_thumbStripController.SelectItemByIndex(context, index);

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->UpdateTitleBarInfo();
            }
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
            RefreshInfoPanel();

            if (m_viewSyncSuppressBroadcast)
            {
                return;
            }

            const std::wstring myNameLower = ActiveMainFileNameLower();
            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                ficBp->SynchronizeViewTransform(this, myNameLower, vt);
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
            m_showNavItems = !m_showNavItems;
            ApplyShowNavItems(m_showNavItems);

            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                ficBp->SynchronizeShowNavItems(this, m_showNavItems);
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

            const VirtualPath parent = m_currentFolder.GetParent();
            if (parent == m_currentFolder)
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

            const ThumbItem& item = m_items[m_selectedIndex];
            if (item.kind == ThumbItemKind::Image)
            {
                ApplyMainFromIndex(m_selectedIndex);
                return;
            }

            if (item.kind == ThumbItemKind::Up || item.kind == ThumbItemKind::Folder)
            {
                NavigateToFolder(item.path);
            }
        }

        void NavigateToFolder(const VirtualPath& folder)
        {
            if (!VirtualFileSystem::IsDirectory(folder))
            {
                return;
            }

            const VirtualPath previousFolder = m_currentFolder;
            m_currentFolder = folder;
            RebuildThumbList(previousFolder);
            if (m_thumbPane)
            {
                m_thumbPane->SetScrollX(0.0f);
            }
        }

        void NavigateToFile(const VirtualPath& filePath)
        {
            if (!filePath.Exists())
            {
                return;
            }

            if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(filePath.GetDisplayPath()))
            {
                return;
            }

            const VirtualPath folder = filePath.GetParent();
            if (!VirtualFileSystem::IsDirectory(folder))
            {
                return;
            }

            m_currentFolder = folder;
            RebuildThumbList(filePath);
        }

        void RebuildThumbList(const VirtualPath& preferSelectPath)
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr)
            {
                RebuildThumbListImmediate(preferSelectPath, nullptr);
                return;
            }

            StartThumbListLoadAsync(preferSelectPath);
        }

        void StartThumbListLoadAsync(const VirtualPath& preferSelectPath)
        {
            if (!m_thumbPane || !m_thumbPane->Panel())
            {
                return;
            }

            const unsigned long long requestId = ++m_thumbListRequestId;
            const std::wstring browserName = Name();
            const VirtualPath folder = m_currentFolder;
            const bool showNavItems = m_showNavItems;

            m_thumbListLoading = true;
            m_progressiveListedEntries.clear();
            m_progressivePreferSelectPath = preferSelectPath;
            m_progressiveUiDirty = false;
            m_progressiveLoadCompleted = false;
            m_progressiveLastApplyMs = 0;
            if (m_asyncThumbReadyEvent != nullptr)
            {
                ResetEvent(m_asyncThumbReadyEvent);
            }
            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAnimationFrame();
            }

            ImageBrowserAsyncThumbLoader::StartEnumerate(
                folder,
                [requestId, browserName, folder, preferSelectPath, showNavItems](std::vector<VirtualFileEntry>&& batch, bool completed)
            {
                AsyncThumbListChunkPayload payload {};
                payload.requestId = requestId;
                payload.folder = folder;
                payload.preferSelectPath = preferSelectPath;
                payload.showNavItems = showNavItems;
                payload.completed = completed;
                payload.batch = std::move(batch);
                ImageBrowserAsyncThumbLoader::EnqueueChunk(browserName, std::move(payload));
            });
        }

        bool HandleAsyncThumbReadyMessage()
        {
            DrainAsyncThumbChunks();
            return true;
        }

        std::deque<AsyncThumbListChunkPayload> DequeueAsyncThumbChunks()
        {
            return ImageBrowserAsyncThumbLoader::DequeueChunks(Name(), m_asyncThumbReadyEvent);
        }

        bool MergeAcceptedAsyncThumbChunks(const std::deque<AsyncThumbListChunkPayload>& chunks)
        {
            bool anyAccepted = false;
            for (const auto& chunk : chunks)
            {
                if (!ImageBrowserAsyncThumbLoader::AcceptChunk(
                        chunk,
                        m_thumbListRequestId.load(),
                        m_currentFolder,
                        m_showNavItems))
                {
                    continue;
                }

                anyAccepted = true;
                (void)ImageBrowserAsyncThumbLoader::ApplyChunkToProgressive(
                    chunk,
                    m_progressiveListedEntries,
                    m_progressiveUiDirty,
                    m_progressiveLoadCompleted);
            }
            return anyAccepted;
        }

        void DrainAsyncThumbChunks()
        {
            const std::deque<AsyncThumbListChunkPayload> chunks = DequeueAsyncThumbChunks();
            if (chunks.empty())
            {
                return;
            }

            if (!MergeAcceptedAsyncThumbChunks(chunks))
            {
                return;
            }

            const unsigned long long now = NowMs();
            const bool shouldApplyNow = ImageBrowserAsyncThumbLoader::ShouldApplyNow(
                m_progressiveLoadCompleted,
                now,
                m_progressiveLastApplyMs);
            if (shouldApplyNow)
            {
                ApplyProgressiveThumbUpdate(m_progressiveLoadCompleted);
            }
        }

        void ApplyProgressiveThumbUpdate(bool finalizeSelection)
        {
            if (!m_progressiveUiDirty)
            {
                return;
            }

            const bool applySelection = finalizeSelection && m_progressiveLoadCompleted;
            RebuildThumbListImmediate(
                m_progressivePreferSelectPath,
                &m_progressiveListedEntries,
                applySelection);

            m_progressiveUiDirty = false;
            m_progressiveLastApplyMs = NowMs();
            if (m_progressiveLoadCompleted)
            {
                m_thumbListLoading = false;
            }
        }

        void RebuildThumbListImmediate(
            const VirtualPath& preferSelectPath,
            const std::vector<VirtualFileEntry>* preloadedEntries,
            bool applySelection = true)
        {
            if (!m_thumbPane || !m_thumbPane->Panel())
            {
                return;
            }

            m_items.clear();
            m_typeSelectQuery.clear();
            m_selectedIndex = static_cast<size_t>(-1);
            m_selectedFocus.reset();

            auto context = MakeThumbRebuildContext(preferSelectPath, preloadedEntries);

            auto result = m_thumbStripController.RebuildList(context);

            // Restore selection without scrolling yet; layout hasn't been updated.
            if (!result.hasItems)
            {
                if (applySelection)
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
            }
            else if (applySelection)
            {
                SelectItemByIndex(result.selectIndex);
            }
            else
            {
                // During progressive async updates, keep a stable selected index/focus so keyboard
                // navigation does not reset to the first item between chunks.
                m_selectedIndex = result.selectIndex;
                if (m_selectedIndex < m_items.size())
                {
                    m_selectedFocus = m_items[m_selectedIndex].focus;
                    if (m_items[m_selectedIndex].image)
                    {
                        m_items[m_selectedIndex].image->SetSelected(true);
                    }
                    if (m_items[m_selectedIndex].navTile)
                    {
                        m_items[m_selectedIndex].navTile->SetSelected(true);
                    }
                }
                else
                {
                    m_selectedFocus.reset();
                }
            }

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
                Invalidate();
            }
        }

        ImageBrowserThumbStripController::RebuildListContext MakeThumbRebuildContext(
            const VirtualPath& preferSelectPath,
            const std::vector<VirtualFileEntry>* preloadedEntries)
        {
            ImageBrowserThumbStripController::RebuildListContext context {};
            context.panel = (m_thumbPane != nullptr) ? m_thumbPane->Panel() : nullptr;
            context.items = &m_items;
            context.thumbW = m_thumbW;
            context.thumbH = m_thumbH;
            context.showNavItems = m_showNavItems;
            context.currentFolder = m_currentFolder;
            context.preferSelectPath = preferSelectPath;
            context.onSelectIndex = [this](size_t index)
            {
                RequestFocus();
                SelectItemByIndex(index);
            };
            context.onActivateIndex = [this](size_t index)
            {
                RequestFocus();
                SelectItemByIndex(index);
                QueueDeferredAction(DeferredActionKind::ActivateSelected);
            };
            context.pathEquals = [](const VirtualPath& a, const VirtualPath& b)
            {
                return PathEqualsInsensitive(a, b);
            };
            context.makeStableName = [this](const wchar_t* prefix, const VirtualPath& p)
            {
                return MakeStableThumbName(prefix, p);
            };
            context.isSupportedImage = [](const VirtualPath& p)
            {
                return VirtualFileSystem::IsImageFile(p);
            };
            context.preloadedEntries = preloadedEntries;
            return context;
        }

        void OpenFileDialog(OpenDialogMode mode)
        {
            wchar_t fileName[MAX_PATH] {};

            const std::wstring filter = BuildSupportedImageDialogFilter();

            OPENFILENAMEW ofn {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = BackplateRef() ? BackplateRef()->Window() : nullptr;
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
            ofn.lpstrFilter = filter.c_str();
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

            // Defer UI tree mutation to avoid reentrancy issues during input dispatch.
            QueueDeferredAction(mode == OpenDialogMode::SplitHorizontalNewBrowser ? DeferredActionKind::SplitHorizontalWithFile : DeferredActionKind::NavigateToFile, VirtualPath::FromFilesystem(chosen));
        }

        int HorizontalViewerCount() const
        {
            const auto bus = m_eventBus.lock();
            auto* rootHost = static_cast<ImageBrowserImpl*>(ResolveRootBrowserWndFromBus(bus));
            if (rootHost == nullptr)
            {
                return 1;
            }

            if (rootHost->m_hPanes.empty())
            {
                return 1;
            }

            return static_cast<int>(rootHost->m_hPanes.size());
        }

        bool TryPickImageFile(std::filesystem::path& outPath, const wchar_t* title)
        {
            wchar_t fileName[MAX_PATH] {};

            const std::wstring filter = BuildSupportedImageDialogFilter();

            OPENFILENAMEW ofn {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = BackplateRef() ? BackplateRef()->Window() : nullptr;
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
            ofn.lpstrFilter = filter.c_str();
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

        void HandleOpenNewImageCommand()
        {
            if (HorizontalViewerCount() > 3)
            {
                return;
            }

            std::filesystem::path chosen {};
            if (TryPickImageFile(chosen, L"Open New Image"))
            {
                QueueInsertHorizontalWithPathAfterThis(VirtualPath::FromFilesystem(chosen));
            }
        }

        void ToggleSamplingQualityFromContextMenu()
        {
            RequestFocus();
            if (m_mainImage)
            {
                m_mainImage->ToggleSamplingQuality();
                RefreshInfoPanel();
            }
        }

        HWND ContextMenuOwnerWindow() const
        {
            FD2D::Backplate* bp = BackplateRef();
            return bp ? bp->Window() : nullptr;
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
            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                ficBp->SynchronizeBackgroundColor(this, next);
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
            const D2D1_COLOR_F currentFocused = m_browserFocusedBackgroundColor;
            cc.rgbResult = RGB(
                toByte(currentFocused.r),
                toByte(currentFocused.g),
                toByte(currentFocused.b));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (!ChooseColorW(&cc))
            {
                return; // cancelled
            }

            const BYTE r = GetRValue(cc.rgbResult);
            const BYTE g = GetGValue(cc.rgbResult);
            const BYTE b = GetBValue(cc.rgbResult);

            const D2D1_COLOR_F nextFocused = D2D1::ColorF(
                static_cast<float>(r) / 255.0f,
                static_cast<float>(g) / 255.0f,
                static_cast<float>(b) / 255.0f,
                1.0f);

            ApplyFocusedBackgroundColor(nextFocused);
            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                ficBp->SynchronizeFocusedBackgroundColor(this, nextFocused);
            }

            if (bp->Window() != nullptr)
            {
                bp->Render();
            }
        }

        void ToggleAlphaCheckerboard()
        {
            bool checkerEnabled = false;
            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                checkerEnabled = !ficBp->AlphaCheckerboardEnabled();
            }
            else
            {
                checkerEnabled = true;
            }

            ApplyAlphaCheckerboard(checkerEnabled);
            if (ficBp != nullptr)
            {
                ficBp->SynchronizeAlphaCheckerboard(this, checkerEnabled);
            }
        }

        void QueueCloseHorizontalThisBrowser()
        {
            // Do not allow closing the root host browser.
            if (IsRootHorizontalHostBrowser())
            {
                return;
            }

            // Require at least 2 viewers.
            if (HorizontalViewerCount() < 2)
            {
                return;
            }

            QueueDeferredActionCore(DeferredActionKind::CloseHorizontalByName, {}, Name());
        }

        void QueueDeferredAction(DeferredActionKind kind, const VirtualPath& path = {})
        {
            // Ensure any deferred action runs on the ImageBrowser that originated it.
            // (Pointer input is often handled by child tiles, so the parent ImageBrowser may not receive it directly.)
            QueueDeferredActionCore(kind, path);
        }

        void QueueInsertHorizontalWithPathAfterThis(const VirtualPath& path)
        {
            QueueDeferredActionCore(DeferredActionKind::InsertHorizontalWithPathAfterName, path, Name());
        }

        void QueueDeferredActionCore(
            DeferredActionKind kind,
            const VirtualPath& path = {},
            const std::wstring& text = L"")
        {
            RequestFocus();
            auto state = ReadDeferredActionState();
            ImageBrowserDeferredActions::Queue(
                state,
                static_cast<int>(kind),
                path,
                text);
            WriteDeferredActionState(state);
            PostDeferredActionMessage();
        }

        void PostDeferredActionMessage()
        {
            if (BackplateRef() != nullptr)
            {
                PostMessageW(BackplateRef()->Window(), CMD_FIC2_DEFERRED_ACTION, 0, 0);
            }
        }

        ImageBrowserDeferredActions::DeferredState ReadDeferredActionState() const
        {
            ImageBrowserDeferredActions::DeferredState state {};
            state.kind = static_cast<int>(m_deferredKind);
            state.path = m_deferredPath;
            state.text = m_deferredText;
            return state;
        }

        void WriteDeferredActionState(const ImageBrowserDeferredActions::DeferredState& state)
        {
            m_deferredKind = static_cast<DeferredActionKind>(state.kind);
            m_deferredPath = state.path;
            m_deferredText = state.text;
        }

        void RunDeferredAction()
        {
            auto state = ReadDeferredActionState();
            const auto snapshot = ImageBrowserDeferredActions::TakeSnapshotAndClear(
                state,
                static_cast<int>(DeferredActionKind::None));
            WriteDeferredActionState(state);

            (void)ImageBrowserDeferredActions::Dispatch(
                snapshot,
                [this](int rawKind)
                {
                    return DispatchDeferredNoPayload(static_cast<DeferredActionKind>(rawKind));
                },
                [this](int rawKind, const VirtualPath& path)
                {
                    return DispatchDeferredPathOnly(static_cast<DeferredActionKind>(rawKind), path);
                },
                [this](int rawKind, const VirtualPath& path, const std::wstring& text)
                {
                    return DispatchDeferredPathAndText(static_cast<DeferredActionKind>(rawKind), path, text);
                });
        }

        // Deferred actions that do not consume path/text payload.
        bool DispatchDeferredNoPayload(DeferredActionKind kind)
        {
            switch (kind)
            {
            case DeferredActionKind::ToggleNavItems:
                ToggleNavItems();
                return true;
            case DeferredActionKind::NavigateUp:
                NavigateUp();
                return true;
            case DeferredActionKind::ActivateSelected:
                ActivateSelectedImpl();
                return true;
            default:
                return false;
            }
        }

        // Deferred actions that consume only path payload.
        bool DispatchDeferredPathOnly(DeferredActionKind kind, const VirtualPath& path)
        {
            switch (kind)
            {
            case DeferredActionKind::NavigateToFolder:
                NavigateToFolder(path);
                return true;
            case DeferredActionKind::NavigateToFile:
                NavigateToFile(path);
                return true;
            case DeferredActionKind::SplitHorizontalWithFile:
                SplitHorizontalWithFile(path);
                return true;
            default:
                return false;
            }
        }

        // Deferred actions that consume both text and path payload.
        bool DispatchDeferredPathAndText(DeferredActionKind kind, const VirtualPath& path, const std::wstring& text)
        {
            switch (kind)
            {
            case DeferredActionKind::InsertHorizontalWithPathAfterName:
                InsertHorizontalWithPathAfterName(text, path);
                return true;
            case DeferredActionKind::CloseHorizontalByName:
                CloseHorizontalByName(text);
                return true;
            default:
                return false;
            }
        }

        bool IsRootHorizontalHostBrowser() const
        {
            const auto bus = m_eventBus.lock();
            return static_cast<ImageBrowserImpl*>(ResolveRootBrowserWndFromBus(bus)) == this;
        }

        ImageBrowserImpl* DelegatedRootHostBrowser() const
        {
            const auto bus = m_eventBus.lock();
            auto* rootHost = static_cast<ImageBrowserImpl*>(ResolveRootBrowserWndFromBus(bus));
            if (rootHost == nullptr || rootHost == this)
            {
                return nullptr;
            }
            return rootHost;
        }

        void RequestLayoutIfAvailable()
        {
            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestLayout();
            }
        }

        bool CanAddHorizontalViewer() const
        {
            return ImageBrowserSplitCoordinator::CanAddViewer(m_hPanes.size());
        }

        bool EnsureHorizontalHostReady(bool enforceCapacity = false, bool showLimitMessage = false)
        {
            EnsureHorizontalHost();
            if (m_hPanes.empty())
            {
                return false;
            }

            if (enforceCapacity && !CanAddHorizontalViewer())
            {
                if (showLimitMessage && BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
                {
                    MessageBoxW(
                        BackplateRef()->Window(),
                        L"Maximum 4 viewers are supported.",
                        L"FICture2",
                        MB_OK | MB_ICONINFORMATION);
                }
                return false;
            }

            return true;
        }

        bool TryInsertHorizontalPathAfterName(const std::wstring& afterName, const std::wstring& path)
        {
            if (path.empty())
            {
                return true;
            }

            if (!CanAddHorizontalViewer())
            {
                return false;
            }

            InsertHorizontalWithPathAfterName(afterName, VirtualPath::FromFilesystem(path));
            return true;
        }

        void RefreshHorizontalHostLayout()
        {
            RebuildHorizontalHost();
            RequestLayoutIfAvailable();
        }

        void CloseHorizontalByName(const std::wstring& name)
        {
            if (name.empty())
            {
                return;
            }

            // Always apply closing at the root host browser.
            if (auto* rootHost = DelegatedRootHostBrowser())
            {
                rootHost->CloseHorizontalByName(name);
                return;
            }

            if (!EnsureHorizontalHostReady())
            {
                return;
            }
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
                    RefreshHorizontalHostLayout();
                    return;
                }
            }
        }

        void InsertHorizontalWithPathAfterName(const std::wstring& afterName, const VirtualPath& path)
        {
            if (auto* rootHost = DelegatedRootHostBrowser())
            {
                rootHost->InsertHorizontalWithPathAfterName(afterName, path);
                return;
            }

            if (path.empty())
            {
                return;
            }

            if (!EnsureHorizontalHostReady(true))
            {
                return;
            }

            const size_t insertIndex = ImageBrowserSplitCoordinator::ResolveInsertIndex(m_hPanes, afterName);
            const std::wstring childName = ImageBrowserSplitCoordinator::NextInsertBrowserName();
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

            m_hPanes.insert(m_hPanes.begin() + static_cast<std::ptrdiff_t>(insertIndex), newWnd);
            RefreshHorizontalHostLayout();
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
            if (auto* rootHost = DelegatedRootHostBrowser())
            {
                rootHost->SplitHorizontalWithFile(filePath);
                return;
            }

            if (!EnsureHorizontalHostReady(true, true))
            {
                return;
            }

            const std::wstring childName = ImageBrowserSplitCoordinator::NextSplitBrowserName();
            auto newBrowser = CreateImageBrowser(childName, filePath.wstring());
            m_hPanes.push_back(newBrowser);

            RefreshHorizontalHostLayout();
        }

        void RebuildHorizontalHost()
        {
            // Rebuild the full host so all panes become equal width.
            // (Nested splits with 0.5 ratios produce 50/25/25 otherwise.)
            const auto host = ImageBrowserSplitCoordinator::BuildEqualWidthHostTree(m_hPanes);
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
        std::shared_ptr<ImageBrowserMainPane> m_mainPane {};
        std::shared_ptr<FD2D::MainImage> m_mainImage {};
        std::wstring m_mainPath {};

        std::shared_ptr<FD2D::Wnd> m_selectedFocus {};
        ImageBrowserThumbStripController m_thumbStripController {};
        std::shared_ptr<ImageBrowserThumbnailPane> m_thumbPane {};
        std::vector<ThumbItem> m_items {};
        size_t m_selectedIndex { static_cast<size_t>(-1) };
        std::wstring m_typeSelectQuery {};
        unsigned long long m_typeSelectLastInputMs { 0 };
        ULONGLONG m_lastKeyNavMs { 0 };

        VirtualPath m_currentFolder {};
        bool m_showNavItems { true };
        std::atomic<unsigned long long> m_thumbListRequestId { 0 };
        bool m_thumbListLoading { false };
        VirtualPath m_progressivePreferSelectPath {};
        std::vector<VirtualFileEntry> m_progressiveListedEntries {};
        bool m_progressiveUiDirty { false };
        bool m_progressiveLoadCompleted { false };
        unsigned long long m_progressiveLastApplyMs { 0 };
        HANDLE m_asyncThumbReadyEvent { nullptr };
        float m_thumbW { 128.0f };
        float m_thumbH { 128.0f };
        unsigned long long m_lastThumbSizingApplyMs { 0 };
        bool m_pendingThumbStripBroadcast { false };
        float m_pendingThumbStripHeight { 0.0f };
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
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
        ID2D1RenderTarget* m_folderBitmapTarget { nullptr };

        // ImageBrowser background colors (used for focus indication).
        // NOTE: base defaults match the global clear; focused defaults to a dark yellow accent.
        D2D1_COLOR_F m_browserBackgroundColor { 0.09f, 0.09f, 0.10f, 1.0f };
        D2D1_COLOR_F m_browserFocusedBackgroundColor { 0.18f, 0.16f, 0.03f, 1.0f };
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_browserBackgroundBrush {};

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

