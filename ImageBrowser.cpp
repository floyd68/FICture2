#include "ImageBrowser.h"
#include "Version.h"
#include "CommonUtil.h"

#include "framework.h"
#include "Resource.h"
#include "ThumbNavTile.h"
#include "ThumbImageTile.h"
#include "ImageBrowserMainPane.h"
#include "ImageBrowserThumbnailPane.h"
#include "ImageBrowserAsyncThumbLoader.h"
#include "ImageBrowserSplitCoordinator.h"
#include "ImageBrowserDeferredActions.h"
#include "ImageBrowserDragController.h"
#include "ImageBrowserThumbTypes.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserInfoPresenter.h"
#include "ImageBrowserKeyboardController.h"
#include "ImageBrowserThumbLayoutCoordinator.h"
#include "ImageBrowserAssets.h"
#include "IpcCompareRequest.h"
#include "Ficture2Backplate.h"
#include "AppLog.h"

#include "FD2D/FD2D.h"
#include "FD2D/Util.h"
#include "ImageBrowserMainImage.h"
#include "ImageViewTypes.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"
#include "VirtualPath.h"
#include "VirtualFileSystem.h"
#include "ImageAwareVfs.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
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

    static std::wstring MakeStableThumbName(const wchar_t* prefix, const std::wstring& path)
    {
        std::wstring s = CommonUtil::ToLower(path);

        return std::wstring(prefix) + L"_" + CommonUtil::Hex64(CommonUtil::Fnv1a64(s)) + L"_tile";
    }

    static std::wstring MakeStableThumbName(const wchar_t* prefix, const std::filesystem::path& p)
    {
		return MakeStableThumbName(prefix, p.wstring());
    }

    static std::wstring MakeStableThumbName(const wchar_t* prefix, const Floar::VirtualPath& vp)
    {
		return MakeStableThumbName(prefix, vp.GetDisplayPath());
    }

    class ImageBrowserImpl : public FD2D::Wnd, public IImageBrowserOps
    {
    public:
        explicit ImageBrowserImpl(const std::wstring& name, const std::wstring& initialFile = L"")
            : Wnd(name)
            , m_initialFile(initialFile)
        {
            FIC2_TIMER_START(t_ctor);
            m_asyncThumbReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            FIC2_LOG_STEP(t_ctor, "ctor: CreateEventW");
            if (m_asyncThumbReadyEvent != nullptr)
            {
                ImageBrowserAsyncThumbLoader::RegisterBrowser(Name(), m_asyncThumbReadyEvent);
                FIC2_LOG_STEP(t_ctor, "ctor: AsyncThumbLoader::RegisterBrowser");
            }
            BuildUi();
            FIC2_LOG_STEP(t_ctor, "ctor: BuildUi total");
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
            FIC2_TIMER_START(t_attach);
            Wnd::OnAttached(backplate);
            FIC2_LOG_STEP(t_attach, "[OnAttached] Wnd::OnAttached (recursive child attach + layout)");

            RegisterWithEventBus();
            FIC2_LOG_STEP(t_attach, "[OnAttached] RegisterWithEventBus");

            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                ficBp->EnsureImageBrowserIniInitialized();
                FIC2_LOG_STEP(t_attach, "[OnAttached] EnsureImageBrowserIniInitialized");

                m_showNavItems = ficBp->ShowNavItemsEnabled();
                m_browserFocusedBackgroundColor = ficBp->FocusedBackgroundColor();
                ApplyShowNavItems(m_showNavItems);
                ApplyAlphaCheckerboard(ficBp->AlphaCheckerboardEnabled());
                if (m_mainImage)
                {
                    m_mainImage->SetZoomStiffness(ficBp->ImageZoomStiffness());
                }
                FIC2_LOG_STEP(t_attach, "[OnAttached] apply INI settings");
            }
            // Default per-ImageBrowser background follows current global clear color.
            m_browserBackgroundColor = backplate.ClearColor();
            if (BackplateRef() != nullptr && BackplateRef()->FocusedWnd() == nullptr)
            {
                RequestFocus();
            }
            FIC2_LOG_STEP(t_attach, "[OnAttached] ClearColor + RequestFocus");
            FIC2_LOG_DEBUG("[OnAttached] complete");
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

            Floar::VirtualPath prefer {};
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
                auto scroll = m_thumbPane->Scroll();
                if (scroll)
                {
                    scroll->EnsureCentered(focusRect, true);
                }
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
                const unsigned long long now = CommonUtil::NowMs();
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
            if (m_dragOverlay != ImageBrowserDragController::OverlayKind::None && m_mainPane != nullptr)
            {
                m_mainPane->RenderOnMainRect([this, target](const D2D1_RECT_F& mainRect)
                {
                    m_dragController.DrawOverlay(target, mainRect, m_dragOverlay);
                });
            }

            // Draw folder / parent-folder icon in main image area if a folder is selected
            if (m_selectedIndex < m_items.size() &&
                (m_items[m_selectedIndex].kind == ThumbItemKind::Folder || m_items[m_selectedIndex].kind == ThumbItemKind::Up) &&
                m_mainPane != nullptr)
            {
                const bool isUp = (m_items[m_selectedIndex].kind == ThumbItemKind::Up);
                const auto iconKind = isUp
                    ? ImageBrowserAssets::FolderIconKind::FolderUp
                    : ImageBrowserAssets::FolderIconKind::Folder;
                if (m_assets.EnsureFolderBitmap(target, iconKind))
                {
                    ID2D1Bitmap* bitmap = isUp ? m_assets.FolderUpBitmap() : m_assets.FolderBitmap();
                    if (bitmap != nullptr)
                    {
                        m_mainPane->RenderCenteredMainOverlayBitmap(target, bitmap, 0.30f);
                    }
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
                FIC2_LOG_INFO("[IPC] UI: HandleIpcCompareMessage received path='{}'",
                    std::filesystem::path(req->path).string());
                req->compareStarted = TryStartCompareWithFileNameMatch(req->path);
                FIC2_LOG_INFO("[IPC] UI: TryStartCompareWithFileNameMatch result={}",
                    req->compareStarted ? "true (compare started)" : "false (no match)");
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

        Floar::VirtualPath GetContextMenuTargetImagePathAtPoint(const POINT& pt) const
        {
            for (const auto& item : m_items)
            {
                if (item.kind != ThumbItemKind::Image || !item.focus)
                {
                    continue;
                }
                if (FD2D::Util::RectContainsPoint(item.focus->LayoutRect(), pt))
                {
                    return item.path;
                }
            }

            if (m_selectedIndex < m_items.size() && m_items[m_selectedIndex].kind == ThumbItemKind::Image)
            {
                return m_items[m_selectedIndex].path;
            }
            return Floar::VirtualPath();
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
            return ImageBrowserKeyboardController::HandleTypeToSelectWithStateStorage(
                event.keyCode,
                event.scanCode,
                event.modifiers.control,
                event.modifiers.alt,
                CommonUtil::NowMs(),
                m_items.size(),
                m_selectedIndex,
                [this](size_t index)
                {
                    return TypeToSelectItemLabel(index);
                },
                [this](size_t index)
                {
                    SelectItemByIndex(index);
                },
                m_typeSelectQuery,
                m_typeSelectLastInputMs);
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
            if (!m_mainPane ||
                !m_mainPane->TryGetMainImageRect(mainRect) ||
                !FD2D::Util::RectContainsPoint(mainRect, clientPt))
            {
                return false;
            }

            ClearDragOverlay();
            ImageBrowserDragController::Action action {};
            if (!m_dragController.HandleFileDrop(path, clientPt, mainRect, action))
            {
                return false;
            }

            switch (action.kind)
            {
            case ImageBrowserDragController::ActionKind::InsertHorizontal:
                QueueDeferredActionCore(DeferredActionKind::InsertHorizontalWithPathAfterName, action.path, Name());
                return true;
            case ImageBrowserDragController::ActionKind::NavigateToFolder:
                QueueDeferredAction(DeferredActionKind::NavigateToFolder, action.path);
                return true;
            case ImageBrowserDragController::ActionKind::NavigateToFile:
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
            if (!m_mainPane ||
                !m_mainPane->TryGetMainImageRect(mainRect) ||
                !FD2D::Util::RectContainsPoint(mainRect, clientPt))
            {
                outVisual = FD2D::FileDragVisual::None;
                ClearDragOverlay();
                return false;
            }

            if (!m_dragController.HandleFileDrag(path, clientPt, mainRect, outVisual, m_dragOverlay))
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
                FIC2_LOG_DEBUG("[IPC] UI: TryStartCompareWithFileNameMatch — incoming path empty, skipping.");
                return false;
            }

            const std::wstring currentPath = ActiveMainPath();
            if (currentPath.empty())
            {
                FIC2_LOG_WARN("[IPC] UI: TryStartCompareWithFileNameMatch — current browser has no file open (m_mainPath empty).");
                return false;
            }

            const auto NormalizeLowerName = [](const std::wstring& p) -> std::wstring
            {
                return CommonUtil::ToLower(std::filesystem::path(p).filename().wstring());
            };

            const std::wstring incomingName = NormalizeLowerName(incomingFilePath);
            const std::wstring currentName = NormalizeLowerName(currentPath);

            FIC2_LOG_DEBUG("[IPC] UI: compare '{}' vs current '{}'", 
                std::filesystem::path(incomingName).string(),
                std::filesystem::path(currentName).string());

            if (incomingName.empty() || currentName.empty())
            {
                return false;
            }

            if (incomingName != currentName)
            {
                FIC2_LOG_INFO("[IPC] UI: filename mismatch ('{}' != '{}') — Ignore.",
                    std::filesystem::path(incomingName).string(),
                    std::filesystem::path(currentName).string());
                return false;
            }

            FIC2_LOG_INFO("[IPC] UI: filename match! Opening incoming path in split pane: {}",
                std::filesystem::path(incomingFilePath).string());
            auto vp = Floar::VirtualPath::Parse(incomingFilePath);
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

            auto vp = Floar::VirtualPath::Parse(filePath);
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
            auto vp = Floar::VirtualPath::Parse(folderPath);
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
            auto vp = Floar::VirtualPath::Parse(filePath);
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

        void OpenAdditionalFileInHorizontalSplit(const Floar::VirtualPath& filePath)
        {
            if (Floar::VirtualFileSystem::IsDirectory(filePath) || filePath.IsArchiveFile())
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

        bool UpdateThumbSizingFromPane()
        {
            float paneH = 0.0f;
            if (m_thumbPane == nullptr)
            {
                // Fall back to backplate-synced height only when local layout is not ready yet.
                (void)TryGetSyncedThumbStripHeight(paneH);
            }
            else
            {
                D2D1_RECT_F stripRect {};
                if (!m_thumbPane->TryGetStripRect(stripRect))
                {
                    (void)TryGetSyncedThumbStripHeight(paneH);
                }
                else
                {
                    paneH = (std::max)(0.0f, stripRect.bottom - stripRect.top);
                }
            }
            const ImageBrowserThumbLayoutCoordinator::ThumbSizeInput sizingInput
            {
                paneH,
                m_thumbH,
                (m_rootSplit != nullptr && m_rootSplit->IsSplitterDragging()),
                CommonUtil::NowMs(),
                m_lastThumbSizingApplyMs,
                kThumbMinSide,
                kThumbMaxSide,
                4.0f
            };
            const auto sizing = ImageBrowserThumbLayoutCoordinator::EvaluateThumbSize(sizingInput);
            if (!sizing.shouldApply)
            {
                return false;
            }
            const float newHeight = sizing.newThumbSide;

            m_thumbW = newHeight; // Keep this for navigation tiles
            m_thumbH = newHeight;

            if (m_thumbPane)
            {
                auto scroll = m_thumbPane->Scroll();
                if (scroll)
                {
                    scroll->SetScrollStep((std::max)(48.0f, newHeight * 0.75f));
                }
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
            m_lastThumbSizingApplyMs = sizing.updatedApplyMs;
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
            OpenAdditionalFileInHorizontalSplit(Floar::VirtualPath::FromFilesystem(filePath));
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

        void BrowserApplyViewTransformForSync(const ImageViewTransform& vt) override
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

        void BrowserCmdRotateLeft() override
        {
            if (auto m = ActiveMainImage())
            {
                m->RotateCCW();
            }
        }

        void BrowserCmdRotateRight() override
        {
            if (auto m = ActiveMainImage())
            {
                m->RotateCW();
            }
        }

        void BrowserCmdRotate180() override
        {
            auto m = ActiveMainImage();
            if (!m)
            {
                return;
            }
            auto vt = m->GetViewTransform();
            vt.rotationQuarters = (vt.rotationQuarters + 2) % 4;
            m->SetViewTransform(vt, true /*notify → sync*/);
        }

        void BrowserCmdRotateReset() override
        {
            auto m = ActiveMainImage();
            if (!m)
            {
                return;
            }
            auto vt = m->GetViewTransform();
            vt.rotationQuarters = 0;
            m->SetViewTransform(vt, true /*notify → sync*/);
        }

        bool BrowserContextMenuPrepareForDisplay(const POINT& ptClient) override
        {
            D2D1_RECT_F mainRect {};
            if (m_mainPane == nullptr ||
                !m_mainPane->TryGetMainImageRect(mainRect) ||
                !FD2D::Util::RectContainsPoint(mainRect, ptClient))
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

        std::wstring BrowserGetExplorerTargetPathAtPoint(const POINT& ptClient) const override
        {
            return GetContextMenuTargetImagePathAtPoint(ptClient).wstring();
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
            FIC2_TIMER_START(t_build);
            FIC2_LOG_DEBUG("[BuildUi] start (name='{}')", std::string(Name().begin(), Name().end()));

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
            FIC2_LOG_STEP(t_build, "[BuildUi] rootSplit created");

            BuildMainPanes();
            FIC2_LOG_STEP(t_build, "[BuildUi] BuildMainPanes (ImageBrowserMainPane + ImageBrowserMainImage)");

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
            FIC2_LOG_STEP(t_build, "[BuildUi] ImageBrowserThumbnailPane::Build");

            rootSplit->OnSplitChanged([this](float)
            {
                float h = 0.0f;
                if (m_thumbPane == nullptr)
                {
                    return;
                }
                D2D1_RECT_F stripRect {};
                if (!m_thumbPane->TryGetStripRect(stripRect))
                {
                    return;
                }
                h = (std::max)(0.0f, stripRect.bottom - stripRect.top);

                const float clampedHeight = ImageBrowserThumbLayoutCoordinator::ClampThumbStripHeight(
                    h,
                    kThumbStripMinH,
                    kThumbStripMaxH);
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

            if (!m_initialFile.empty())
            {
                FIC2_LOG_DEBUG("[BuildUi] initial file: '{}'",
                    std::filesystem::path(m_initialFile).string());
                auto parsed = Floar::VirtualPath::Parse(m_initialFile);
                if (parsed)
                {
                    if (parsed->IsArchiveFile() && !parsed->IsInArchive())
                    {
                        m_currentFolder = *parsed;
                        RebuildThumbList(Floar::VirtualPath());
                    }
                    else
                    {
                        m_currentFolder = parsed->GetParent();
                        RebuildThumbList(*parsed);
                    }
                }
                FIC2_LOG_STEP(t_build, "[BuildUi] RebuildThumbList (initial file)");
            }
            else
            {
                FIC2_LOG_DEBUG("[BuildUi] no initial file — starting empty");
                // Start empty; a session restore or user navigation will populate.
                m_currentFolder = Floar::VirtualPath();
                RebuildThumbList(Floar::VirtualPath());
                FIC2_LOG_STEP(t_build, "[BuildUi] RebuildThumbList (empty)");
            }

            FIC2_LOG_DEBUG("[BuildUi] complete");
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
            const auto ratioResult = ImageBrowserThumbLayoutCoordinator::ComputeSyncedSplitRatio(
                {
                    totalH,
                    syncedHeight,
                    kThumbStripMinH,
                    kThumbStripMaxH,
                    kSplitPanelDefaultHitThickness
                });
            if (!ratioResult.valid)
            {
                return false;
            }

            if (!force)
            {
                // If we're already close, avoid churning layouts.
                float cur = 0.0f;
                if (m_thumbPane != nullptr)
                {
                    D2D1_RECT_F stripRect {};
                    if (m_thumbPane->TryGetStripRect(stripRect))
                    {
                        cur = (std::max)(0.0f, stripRect.bottom - stripRect.top);
                        if (std::abs(cur - ratioResult.desiredSecondHeight) < 1.0f)
                        {
                            return false;
                        }
                    }
                }
            }

            m_rootSplit->SetSplitRatio(ratioResult.splitRatio);

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
                [this](const ImageViewTransform& vt)
                {
                    OnActiveMainViewChanged(vt);
                },
                [this]()
                {
                    RequestFocus();
                },
                [this](ImageBrowserMainImage& mainImage)
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
            return CommonUtil::ToLower(std::filesystem::path(p).filename().wstring());
        }

        void RefreshInfoPanel()
        {
            if (!m_mainPane)
            {
                return;
            }

            auto main = ActiveMainImage();
            int zoomPct = 100;
            if (main)
            {
                zoomPct = static_cast<int>(std::round(main->ZoomScale() * 100.0f));
            }

            ImageBrowserInfoPresenter::Input input {};
            input.activePath = ActiveMainPath();
            input.hasSelection = (m_selectedIndex < m_items.size());
            if (input.hasSelection)
            {
                input.selectedKind = m_items[m_selectedIndex].kind;
                input.selectedPath = m_items[m_selectedIndex].path;
            }
            input.currentFolder = m_currentFolder;
            input.hasLoadedInfo = (main != nullptr);
            if (main)
            {
                input.loadedInfo = main->GetLoadedInfo();
            }
            input.zoomPercent = zoomPct;
            input.rotationQuarters = main ? main->GetViewTransform().rotationQuarters : 0;
            input.hasSamplingState = (m_mainImage != nullptr);
            if (m_mainImage != nullptr)
            {
                input.highQualitySampling = m_mainImage->HighQualitySampling();
                auto* ficBp = FictureBackplateRef();
                input.useD3DRenderer = (ficBp != nullptr && ficBp->D3DDevice() != nullptr);
            }

            const auto output = ImageBrowserInfoPresenter::Build(input);
            m_mainPane->UpdateInfo(output.pathText, output.infoText, output.zoomText);
        }

        void ApplyIniToMainImage(ImageBrowserMainImage& mainImage)
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp != nullptr)
            {
                mainImage.SetZoomStiffness(ficBp->ImageZoomStiffness());
            }
        }

        std::shared_ptr<ImageBrowserMainImage> ActiveMainImage() const
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
                    auto scroll = m_thumbPane->Scroll();
                    if (scroll)
                    {
                        scroll->EnsureCentered(focusRect);
                    }
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
                const std::wstring curLower = CommonUtil::ToLower(m_items[m_selectedIndex].path.filename().wstring());
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

                const std::wstring nameLower = CommonUtil::ToLower(m_items[i].path.filename().wstring());
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

        void OnActiveMainViewChanged(const ImageViewTransform& vt)
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

        void ApplySyncedViewTransform(const ImageViewTransform& vt)
        {
            auto main = ActiveMainImage();
            if (!main)
            {
                return;
            }

            // Sync should mirror the *currently displayed* view state from the focused source.
            // Do NOT re-run per-pane spring animation on receivers (it can cause subtle jitter
            // due to different frame timing/layout across panes).
            ImageViewTransform applied = vt;
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

            const Floar::VirtualPath parent = m_currentFolder.GetParent();
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

        void NavigateToFolder(const Floar::VirtualPath& folder)
        {
            if (!Floar::VirtualFileSystem::IsDirectory(folder))
            {
                return;
            }

            const Floar::VirtualPath previousFolder = m_currentFolder;
            m_currentFolder = folder;
            RebuildThumbList(previousFolder);
            if (m_thumbPane)
            {
                auto scroll = m_thumbPane->Scroll();
                if (scroll)
                {
                    scroll->SetScrollX(0.0f);
                }
            }
        }

        void NavigateToFile(const Floar::VirtualPath& filePath)
        {
            if (!filePath.Exists())
            {
                return;
            }

            if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(filePath.GetDisplayPath()))
            {
                return;
            }

            const Floar::VirtualPath folder = filePath.GetParent();
            if (!Floar::VirtualFileSystem::IsDirectory(folder))
            {
                return;
            }

            m_currentFolder = folder;
            RebuildThumbList(filePath);
        }

        void RebuildThumbList(const Floar::VirtualPath& preferSelectPath)
        {
            FD2D::Backplate* bp = BackplateRef();
            if (bp == nullptr)
            {
                RebuildThumbListImmediate(preferSelectPath, nullptr);
                return;
            }

            StartThumbListLoadAsync(preferSelectPath);
        }

        void StartThumbListLoadAsync(const Floar::VirtualPath& preferSelectPath)
        {
            if (!m_thumbPane || !m_thumbPane->Panel())
            {
                return;
            }

            const unsigned long long requestId = ++m_thumbListRequestId;
            const std::wstring browserName = Name();
            const Floar::VirtualPath folder = m_currentFolder;
            const bool showNavItems = m_showNavItems;

            m_thumbListLoading = true;
            m_progressiveStartMs = CommonUtil::NowMs();
            FIC2_LOG_INFO("[ThumbLoad] Started loading folder '{}' (browser={})",
                std::filesystem::path(folder.wstring()).string(),
                std::filesystem::path(browserName).string());
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
                [requestId, browserName, folder, preferSelectPath, showNavItems](std::vector<Floar::VirtualFileEntry>&& batch, bool completed)
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

            const unsigned long long now = CommonUtil::NowMs();
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
            // Unconditional (not threshold-gated): this runs both from DrainAsyncThumbChunks
            // (folder enumeration chunks arriving) and from OnRender's ~50ms poll while a
            // list is still loading — the latter has no other timing coverage, and it runs
            // on the UI thread inside every affected Render() call, which is a prime suspect
            // for "sluggish panning/low FPS for a few seconds after opening a large folder".
            FIC2_TIMER_START(t_apply);
            RebuildThumbListImmediate(
                m_progressivePreferSelectPath,
                &m_progressiveListedEntries,
                applySelection);
            const auto applyMs = FIC2_ELAPSED_MS(t_apply);
            FIC2_LOG_INFO(
                "[ThumbLoad] ApplyProgressiveThumbUpdate: {} items in {}ms (finalize={}, entriesAccumulated={})",
                m_items.size(), applyMs, m_progressiveLoadCompleted, m_progressiveListedEntries.size());

            m_progressiveUiDirty = false;
            m_progressiveLastApplyMs = CommonUtil::NowMs();
            if (m_progressiveLoadCompleted)
            {
                m_thumbListLoading = false;
                const unsigned long long totalMs = CommonUtil::NowMs() - m_progressiveStartMs;
                FIC2_LOG_INFO("[ThumbLoad] Progressive load complete: {} items in {}ms total",
                    m_items.size(), totalMs);
            }
        }

        void RebuildThumbListImmediate(
            const Floar::VirtualPath& preferSelectPath,
            const std::vector<Floar::VirtualFileEntry>* preloadedEntries,
            bool applySelection = true)
        {
            if (!m_thumbPane || !m_thumbPane->Panel())
            {
                return;
            }

            FIC2_TIMER_START(t_rebuild);

            m_items.clear();
            m_typeSelectQuery.clear();
            m_selectedIndex = static_cast<size_t>(-1);
            m_selectedFocus.reset();

            auto context = MakeThumbRebuildContext(preferSelectPath, preloadedEntries);
            FIC2_LOG_STEP(t_rebuild, "[RebuildImmediate] MakeThumbRebuildContext");

            auto result = m_thumbStripController.RebuildList(context);
            FIC2_LOG_STEP(t_rebuild, "[RebuildImmediate] ThumbStripController::RebuildList");

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
            const Floar::VirtualPath& preferSelectPath,
            const std::vector<Floar::VirtualFileEntry>* preloadedEntries)
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
            context.pathEquals = [](const Floar::VirtualPath& a, const Floar::VirtualPath& b)
            {
                return CommonUtil::PathEqualsInsensitive(
                    a.hostPath,
                    a.archiveInnerPath,
                    b.hostPath,
                    b.archiveInnerPath);
            };
            context.makeStableName = [this](const wchar_t* prefix, const Floar::VirtualPath& p)
            {
                return MakeStableThumbName(prefix, p);
            };
            context.isSupportedImage = [](const Floar::VirtualPath& p)
            {
                return ImageAwareVfs::IsImageFile(p);
            };
            context.preloadedEntries = preloadedEntries;
            return context;
        }

        int HorizontalViewerCount() const
        {
            auto* rootHost = RootHostFromEventBus();
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

        void ToggleSamplingQualityFromContextMenu()
        {
            RequestFocus();
            if (m_mainImage)
            {
                m_mainImage->ToggleSamplingQuality();
                RefreshInfoPanel();
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

        void QueueDeferredAction(DeferredActionKind kind, const Floar::VirtualPath& path = {})
        {
            // Ensure any deferred action runs on the ImageBrowser that originated it.
            // (Pointer input is often handled by child tiles, so the parent ImageBrowser may not receive it directly.)
            QueueDeferredActionCore(kind, path);
        }

        void QueueDeferredActionCore(
            DeferredActionKind kind,
            const Floar::VirtualPath& path = {},
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
                [this](int rawKind, const Floar::VirtualPath& path)
                {
                    return DispatchDeferredPathOnly(static_cast<DeferredActionKind>(rawKind), path);
                },
                [this](int rawKind, const Floar::VirtualPath& path, const std::wstring& text)
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
        bool DispatchDeferredPathOnly(DeferredActionKind kind, const Floar::VirtualPath& path)
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
        bool DispatchDeferredPathAndText(DeferredActionKind kind, const Floar::VirtualPath& path, const std::wstring& text)
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
            return RootHostFromEventBus() == this;
        }

        ImageBrowserImpl* DelegatedRootHostBrowser() const
        {
            auto* rootHost = RootHostFromEventBus();
            if (rootHost == nullptr || rootHost == this)
            {
                return nullptr;
            }
            return rootHost;
        }

        ImageBrowserImpl* RootHostFromEventBus() const
        {
            const auto bus = m_eventBus.lock();
            if (!bus)
            {
                return nullptr;
            }

            const auto browsers = bus->ImageBrowsersSnapshot();
            if (browsers.empty())
            {
                return nullptr;
            }

            return static_cast<ImageBrowserImpl*>(browsers.front());
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

            InsertHorizontalWithPathAfterName(afterName, Floar::VirtualPath::FromFilesystem(path));
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

        void InsertHorizontalWithPathAfterName(const std::wstring& afterName, const Floar::VirtualPath& path)
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
                if (Floar::VirtualFileSystem::IsDirectory(path))
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

        void SplitHorizontalWithFile(const Floar::VirtualPath& filePath)
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
        std::shared_ptr<ImageBrowserMainImage> m_mainImage {};
        std::wstring m_mainPath {};

        std::shared_ptr<FD2D::Wnd> m_selectedFocus {};
        ImageBrowserThumbStripController m_thumbStripController {};
        std::shared_ptr<ImageBrowserThumbnailPane> m_thumbPane {};
        std::vector<ThumbItem> m_items {};
        size_t m_selectedIndex { static_cast<size_t>(-1) };
        std::wstring m_typeSelectQuery {};
        unsigned long long m_typeSelectLastInputMs { 0 };
        ULONGLONG m_lastKeyNavMs { 0 };

        Floar::VirtualPath m_currentFolder {};
        bool m_showNavItems { true };
        std::atomic<unsigned long long> m_thumbListRequestId { 0 };
        bool m_thumbListLoading { false };
        Floar::VirtualPath m_progressivePreferSelectPath {};
        std::vector<Floar::VirtualFileEntry> m_progressiveListedEntries {};
        bool m_progressiveUiDirty { false };
        bool m_progressiveLoadCompleted { false };
        unsigned long long m_progressiveLastApplyMs { 0 };
        unsigned long long m_progressiveStartMs { 0 };
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
        Floar::VirtualPath m_deferredPath {};
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
            if (m_dragOverlay != ImageBrowserDragController::OverlayKind::None)
            {
                m_dragOverlay = ImageBrowserDragController::OverlayKind::None;
                Invalidate();
            }
        }

        ImageBrowserDragController m_dragController {};
        ImageBrowserDragController::OverlayKind m_dragOverlay { ImageBrowserDragController::OverlayKind::None };
        ImageBrowserAssets m_assets {};

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

