#include "ImageBrowser.h"
#include "Version.h"
#include "CommonUtil.h"

#include "framework.h"
#include "Resource.h"
#include "ThumbNavTile.h"
#include "ThumbImageTile.h"
#include "ImageBrowserMainPane.h"
#include "ImageBrowserThumbnailPane.h"
#include "ImageBrowserThumbLayout.h"
#include "ImageBrowserThumbViewport.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageBrowserAsyncThumbLoader.h"
#include "FolderEnumeration.h"
#include "ImageBrowserSplitCoordinator.h"
#include "ImageBrowserDeferredActions.h"
#include "ImageBrowserDragController.h"
#include "ImageBrowserThumbTypes.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserInfoPresenter.h"
#include "ImageBrowserPathState.h"
#include "ImageBrowserKeyboardController.h"
#include "ImageBrowserThumbLayoutCoordinator.h"
#include "ImageBrowserAssets.h"
#include "IpcOpenRequest.h"
#include "Ficture2Backplate.h"
#include "AppLog.h"
#include "AppSetup.h"
#include "ArchiveEntryHandoff.h"
#include "DeferredRenderScope.h"

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

    constexpr float kThumbMinSide = 32.0f;
    constexpr float kThumbMaxSide = 256.0f;
    // A strip fits a row of kThumbMinSide..kThumbMaxSide plus its chrome
    // (selection room and scrollbar gutter).
    constexpr float kThumbStripMinH = kThumbMinSide + ImageBrowserThumbnailPane::kStripChrome;
    constexpr float kThumbStripMaxH = kThumbMaxSide + ImageBrowserThumbnailPane::kStripChrome;
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
            m_folderLane = FolderEnumerationService::Shared().CreateLane();
            BuildUi();
            FIC2_LOG_STEP(t_ctor, "ctor: BuildUi total");
        }

        ~ImageBrowserImpl() override
        {
            // Detaches the folder enumeration without waiting for its worker.
            m_folderLane.Close();
            UnregisterFromRegistry();
            DeferredWakeupTargets().erase(m_deferredWakeupId);
        }

        FolderLoadLane& FolderLane() { return m_folderLane; }
        ImageBrowserThumbnailPane* ThumbPane() const { return m_thumbPane.get(); }
        size_t QueuedDeferredActions() const { return m_deferredActions.Size(); }

        void OnAttached(FD2D::Backplate& backplate) override
        {
            FIC2_TIMER_START(t_attach);
            Wnd::OnAttached(backplate);
            // Folder listing chunks wake this window (payload-free message).
            if (auto* folderBp = dynamic_cast<Ficture2Backplate*>(&backplate))
            {
                m_folderLane.SetWakeup(folderBp->FolderListingWakeupTarget());
            }
            FIC2_LOG_STEP(t_attach, "[OnAttached] Wnd::OnAttached (recursive child attach + layout)");

            RegisterWithRegistry();
            FIC2_LOG_STEP(t_attach, "[OnAttached] RegisterWithRegistry");

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
            UnregisterFromRegistry();
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

        void NotifyIpcOpenSnapshotChanged()
        {
            if (auto* ficBp = FictureBackplateRef())
            {
                ficBp->RefreshIpcOpenSnapshot();
            }
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

        std::shared_ptr<BrowserRegistry> RegistryRef() const
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp == nullptr)
            {
                return nullptr;
            }

            return ficBp->RegistryPtr();
        }

        void RegisterWithRegistry()
        {
            if (!m_registry.expired())
            {
                return;
            }

            auto registry = RegistryRef();
            if (!registry)
            {
                return;
            }

            m_registry = registry;
            registry->RegisterImageBrowser(this);
        }

        void UnregisterFromRegistry()
        {
            auto registry = m_registry.lock();
            if (!registry)
            {
                return;
            }

            registry->UnregisterImageBrowser(this);

            m_registry.reset();
        }

        size_t ImageBrowserCount() const
        {
            auto registry = m_registry.lock();
            if (!registry)
            {
                return 0;
            }

            return registry->ImageBrowserCount();
        }

        std::vector<ImageBrowserImpl*> ImageBrowsersSnapshot() const
        {
            std::vector<ImageBrowserImpl*> out;
            auto registry = m_registry.lock();
            if (!registry)
            {
                return out;
            }

            const auto browsers = registry->ImageBrowsersSnapshot();
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
            if (const ThumbModelEntry* selected = SelectedThumbEntry())
            {
                prefer = selected->path;
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
            ApplyThumbScrollAfterArrange();
        }

        // ------------------------------------------------------------------
        // Strip geometry (model extents, not tile windows), the scroll
        // adjustment after a layout pass and the item ranges for the viewport
        // are ThumbStripViewport's. This class reads the viewport from the
        // scroll view and applies the results; a shift keeps a smooth scroll
        // in progress running (ScrollView::ShiftScrollX).
        // ------------------------------------------------------------------

        // Tiles have no margin: their extent is their content width.
        static constexpr float kThumbTileMargin = 0.0f;

        // Items in the strip (the committed model).
        size_t ThumbItemCount() const
        {
            return m_thumbModel.DisplayCount();
        }

        const ThumbModelEntry* SelectedThumbEntry() const
        {
            return (m_selectedIndex < ThumbItemCount()) ? &m_thumbModel.At(m_selectedIndex) : nullptr;
        }

        bool ThumbLayoutReady() const
        {
            return m_thumbViewport.Ready(m_thumbModel);
        }

        // The model indices that have tiles (ThumbStripViewport::Ranges).
        ThumbRanges MaterializedThumbRange(size_t selectedIndex, ThumbViewportBasis basis) const
        {
            return m_thumbViewport.Ranges(m_thumbModel, ThumbViewport(), selectedIndex, basis);
        }

        // Binds tiles for MaterializedThumbRange(), highlighting `selectedIndex`.
        void MaterializeThumbTiles(size_t selectedIndex, ThumbViewportBasis basis = ThumbViewportBasis::Expected)
        {
            auto context = MakeThumbMaterializeContext(selectedIndex, basis);
            m_thumbStripController.MaterializeTiles(context);
        }

        // Runs as the strip panel starts rendering: the scroll view has
        // advanced any smooth scroll, so the viewport is the one this frame
        // presents, even when an animated jump moves farther than the
        // prefetch margin in one frame.
        void OnBeforeRenderStripTiles()
        {
            UpdateMaterializedThumbTiles();
            UpdateThumbLoadPriorities();
        }

        // Between rebuilds the viewport moves (scrolling, or extents and the
        // row height changing): rebind the tiles when the range it needs
        // changed. Runs during Render, before the tiles render; a tile
        // setter's Invalidate() only requests the next frame (FD2D renders
        // coalesced, never re-entrantly), so no frame is rendered per tile.
        void UpdateMaterializedThumbTiles()
        {
            if (!m_thumbPane)
            {
                return;
            }
            // The frame's scroll transform is already set: follow the offset it
            // presents (SettleThumbStripGeometry applied pending adjustments).
            if (MaterializedThumbRange(m_selectedIndex, ThumbViewportBasis::Presented).materialized == m_thumbTiles.Range())
            {
                return;
            }
            MaterializeThumbTiles(m_selectedIndex, ThumbViewportBasis::Presented);
            // This frame's layout pass already ran: place the tiles now (by
            // their model indices), so the frame shows them where they belong.
            if (auto strip = m_thumbPane->StripPanel())
            {
                strip->ArrangeTiles();
            }
        }

        // Extents changed during this render (row height from the splitter, a
        // progressive list update) wait for the next layout pass to re-arrange
        // the strip and apply the scroll adjustment. Apply both now, before the
        // scroll view captures this frame's transform, so the frame draws the
        // tiles at the extents and offset it presents.
        void SettleThumbStripGeometry()
        {
            const auto scroll = m_thumbPane ? m_thumbPane->Scroll() : nullptr;
            if (!scroll || !m_thumbViewport.SettlePending())
            {
                return;
            }
            const D2D1_RECT_F rect = scroll->LayoutRect();
            if (rect.right - rect.left <= 1.0f || rect.bottom - rect.top <= 0.0f)
            {
                return;
            }
            scroll->Arrange({ rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top });
            ApplyThumbScrollAfterArrange();
        }

        // The strip viewport in panel coordinates; none before the first
        // layout pass.
        std::optional<ThumbViewportSpan> ThumbViewport() const
        {
            const auto scroll = m_thumbPane ? m_thumbPane->Scroll() : nullptr;
            const auto panel = m_thumbPane ? m_thumbPane->Panel() : nullptr;
            if (!scroll || !panel)
            {
                return std::nullopt;
            }
            const D2D1_RECT_F view = scroll->LayoutRect();
            const ThumbViewportSpan span {
                view.left + scroll->ScrollX() - panel->LayoutRect().left,
                view.right - view.left };
            if (span.width <= 1.0f)
            {
                return std::nullopt;
            }
            return span;
        }

        // Client rectangle of item `index`, as the panel arranges it.
        D2D1_RECT_F ThumbItemRect(size_t index) const
        {
            const D2D1_RECT_F panel = m_thumbPane->Panel()->LayoutRect();
            return D2D1::RectF(
                panel.left + m_thumbViewport.Layout().Start(index), panel.top,
                panel.left + m_thumbViewport.Layout().End(index), panel.bottom);
        }

        std::wstring ThumbItemKey(size_t index) const
        {
            return (index < ThumbItemCount()) ? m_thumbModel.At(index).lookupKey : std::wstring();
        }

        float ThumbItemExtent(const ThumbModelEntry& item) const
        {
            if (item.kind == ThumbItemKind::Image)
            {
                const float aspect = m_thumbModel.AspectOf(item.lookupKey);
                return ThumbStripLayout::ImageExtent(m_thumbH, aspect, kThumbTileMargin);
            }
            return m_thumbW + 2.0f * kThumbTileMargin;   // nav tiles are square
        }

        // Extents of every item (after a rebuild or a row-height change): the
        // per-update work that is linear in the folder.
        void RebuildThumbLayout()
        {
            const size_t count = ThumbItemCount();
            std::vector<float> extents;
            extents.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                extents.push_back(ThumbItemExtent(m_thumbModel.At(i)));
            }
            m_thumbViewport.Rebuild(std::move(extents),
                ImageBrowserThumbnailPane::kItemSpacing, ImageBrowserThumbnailPane::kPanelPadding);
        }

        // Remembers the anchor before extents change (once per layout pass).
        void CaptureThumbAnchor()
        {
            m_thumbViewport.CaptureAnchor(m_thumbModel, ThumbViewport(), m_selectedIndex);
        }

        void OnThumbImageAspect(const std::wstring& key, float aspect)
        {
            if (!m_thumbModel.SetAspect(key, aspect))
            {
                return;
            }
            const auto index = m_thumbModel.IndexOf(key);
            if (!index || !m_thumbViewport.SetExtent(m_thumbModel, *index,
                    ThumbStripLayout::ImageExtent(m_thumbH, aspect, kThumbTileMargin), ThumbViewport(), m_selectedIndex))
            {
                return;
            }
            if (BackplateRef() != nullptr)
            {
                DeferredRenderScope::RequestLayout(BackplateRef());
            }
        }

        void ApplyThumbScrollAfterArrange()
        {
            const auto scroll = m_thumbPane ? m_thumbPane->Scroll() : nullptr;
            const ThumbStripScroll::Adjustment adjustment =
                m_thumbViewport.TakeAfterArrange(m_thumbModel, scroll ? ThumbViewport() : std::nullopt);
            if (adjustment.kind == ThumbStripScroll::Adjustment::Kind::CenterSelection)
            {
                if (m_selectedIndex < ThumbItemCount())
                {
                    scroll->EnsureCentered(ThumbItemRect(m_selectedIndex), true);
                }
            }
            else if (adjustment.kind == ThumbStripScroll::Adjustment::Kind::Shift)
            {
                scroll->ShiftScrollX(adjustment.shift);
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
            UpdateThumbSizeUpgradeDeferral();
            // Folder listing chunks arrive through CMD_FIC2_ASYNC_THUMB_READY (posted
            // by the loader); this only picks up chunks if a frame happens to render
            // first, and never requests frames itself.
            if (m_listing.Loading() && m_folderLane.HasChunks())
            {
                DrainAsyncThumbChunks();
            }
            // Throttle progressive UI list updates so large folders don't monopolize the UI thread.
            ApplyThrottledProgressiveUpdate();
            SettleThumbStripGeometry();
            // Decodes completed since the last frame are cached and accounted
            // now, also for prefetch tiles that will not render (offscreen).
            ImageBrowserThumbStripController::AdmitCompletedPayloads(m_thumbTiles);
            // Tile materialization and load priorities run when the strip
            // panel renders (OnBeforeRenderStripTiles), after the scroll view
            // advanced this frame's smooth scroll.
            if (m_pendingThumbStripBroadcast && m_rootSplit != nullptr && !m_rootSplit->IsSplitterDragging())
            {
                m_pendingThumbStripBroadcast = false;
                NotifyThumbStripHeightChanged(m_pendingThumbStripHeight);
            }
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
                    if (RefreshTargetResourceStamp(m_browserBackgroundBrushStamp))
                    {
                        m_browserBackgroundBrush.Reset();
                    }
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
        }

        // Fixed-band overlays (FD2D OverlayLayer): paint chrome after the content
        // tree so drag tint / folder icons stay above siblings without a z-index.
        void OnRenderOverlay(
            ID2D1RenderTarget* target,
            FD2D::OverlayLayer layer) override
        {
            if (target == nullptr || layer != FD2D::OverlayLayer::Chrome)
            {
                return;
            }

            // Drag&drop overlay (main image area only).
            if (m_dragOverlay != ImageBrowserDragController::OverlayKind::None &&
                m_mainPane != nullptr)
            {
                m_mainPane->RenderOnMainRect([this, target](const D2D1_RECT_F& mainRect)
                {
                    m_dragController.DrawOverlay(target, mainRect, m_dragOverlay, CurrentTargetStamp());
                });
            }

            // Folder / parent-folder icon when a folder tile is selected.
            const ThumbModelEntry* selected = SelectedThumbEntry();
            if (selected != nullptr &&
                (selected->kind == ThumbItemKind::Folder || selected->kind == ThumbItemKind::Up) &&
                m_mainPane != nullptr)
            {
                const bool isUp = (selected->kind == ThumbItemKind::Up);
                const auto iconKind = isUp
                    ? ImageBrowserAssets::FolderIconKind::FolderUp
                    : ImageBrowserAssets::FolderIconKind::Folder;
                if (m_assets.EnsureFolderBitmap(target, CurrentTargetStamp(), iconKind))
                {
                    ID2D1Bitmap* bitmap = isUp ? m_assets.FolderUpBitmap() : m_assets.FolderBitmap();
                    if (bitmap != nullptr)
                    {
                        m_mainPane->RenderCenteredMainOverlayBitmap(target, bitmap, 0.30f);
                    }
                }
            }
        }

        // Decorative chrome only — do not claim the Chrome input band (keeps
        // hover tooltips and normal hit-testing intact while drag tint paints).
        bool IsOverlayActive(FD2D::OverlayLayer layer) const override
        {
            UNREFERENCED_PARAMETER(layer);
            return false;
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
            // Folder-listing readiness is broadcast to the window, but every
            // browser - including side-by-side browsers nested inside this one -
            // drains only its own queue: drain here, then pass it down the subtree.
            if (event.id == CMD_FIC2_ASYNC_THUMB_READY)
            {
                DrainAsyncThumbChunks();
                (void)Wnd::OnCommandEvent(event);
                return true;
            }

            if (TryHandleCommandEvent(event))
            {
                return true;
            }

            return Wnd::OnCommandEvent(event);
        }

        bool TryHandleCommandEvent(const FD2D::CommandEvent& event)
        {
            switch (event.id)
            {
            case CMD_FIC2_IPC_OPEN:
                return HandleIpcOpenMessage();
            case CMD_FIC2_ASYNC_THUMB_READY:
                return HandleAsyncThumbReadyMessage();
            default:
                return false;
            }
        }

        bool HandleIpcOpenMessage()
        {
            auto* ficBp = FictureBackplateRef();
            if (ficBp == nullptr)
            {
                return false;
            }

            FIC2_LOG_INFO("[IPC] UI: draining IPC open queue.");
            ficBp->DrainIpcOpenQueue();
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
            // Only materialized tiles can be under the point.
            for (const ThumbTile& tile : m_thumbTiles.Tiles())
            {
                if (tile.kind != ThumbItemKind::Image || !tile.focus || tile.index >= ThumbItemCount())
                {
                    continue;
                }
                if (FD2D::Util::RectContainsPoint(tile.focus->LayoutRect(), pt))
                {
                    return m_thumbModel.At(tile.index).path;
                }
            }

            const ThumbModelEntry* selected = SelectedThumbEntry();
            if (selected != nullptr && selected->kind == ThumbItemKind::Image)
            {
                return selected->path;
            }
            return Floar::VirtualPath();
        }

        size_t CurrentThumbSelectionIndex() const
        {
            return (m_selectedIndex < ThumbItemCount()) ? m_selectedIndex : 0;
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
                    if ((next + 1) >= ThumbItemCount())
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

            // Image-view letter keys (NIFDiff parity). Handled before type-to-select.
            if (!event.modifiers.control &&
                !event.modifiers.alt &&
                !event.modifiers.shift)
            {
                switch (event.keyCode)
                {
                case 'R': BrowserCmdSetChannelMode(1); return true;
                case 'G': BrowserCmdSetChannelMode(2); return true;
                case 'B': BrowserCmdSetChannelMode(3); return true;
                case 'A': BrowserCmdSetChannelMode(4); return true;
                case 'N': BrowserCmdSetChannelMode(0); return true;
                case 'I': BrowserCmdShowImageInformation(); return true;
                default: break;
                }
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
                ThumbItemCount(),
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
            if (index >= ThumbItemCount())
            {
                return L"";
            }

            const ThumbModelEntry& item = m_thumbModel.At(index);
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

        // Thumbnail decode priority follows the strip's viewport: tiles that
        // overlap it load at Visible priority, all others as Prefetch, so
        // ImageCore serves on-screen thumbnails first in both its I/O and decode
        // stages (and promotes queued ones as they scroll into view). Tiles keep
        // their content coordinates; the scroll view translates while rendering.
        // Runs every frame over the materialized tiles only.
        void UpdateThumbLoadPriorities()
        {
            const auto visible = m_thumbViewport.VisibleRange(m_thumbModel, ThumbViewport());
            if (!visible)
            {
                return;
            }
            for (const ThumbTile& tile : m_thumbTiles.Tiles())
            {
                if (tile.image)
                {
                    tile.image->SetLoadPriority(visible->Contains(tile.index) ? ImageCore::RequestPriority::Visible : ImageCore::RequestPriority::Prefetch);
                }
            }
        }

        // Items fully inside the viewport (at least 1).
        size_t PagingStepFromThumbViewport() const
        {
            if (const auto full = m_thumbViewport.FullyVisibleCount(m_thumbModel, ThumbViewport()))
            {
                return *full;
            }
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
                FIC2_LOG_WARN("[IPC] UI: TryStartCompareWithFileNameMatch — current browser has no requested image.");
                return false;
            }

            const auto NormalizeLowerName = [](const std::wstring& p) -> std::wstring
            {
                return CommonUtil::ToLower(std::filesystem::path(p).filename().wstring());
            };

            const std::wstring incomingName = NormalizeLowerName(incomingFilePath);
            const std::wstring currentName = NormalizeLowerName(currentPath);

            FIC2_LOG_DEBUG("[IPC] UI: compare '{}' vs current '{}'", 
                AppLog::ToUtf8(incomingName),
                AppLog::ToUtf8(currentName));

            if (incomingName.empty() || currentName.empty())
            {
                return false;
            }

            if (incomingName != currentName)
            {
                FIC2_LOG_INFO("[IPC] UI: filename mismatch ('{}' != '{}') — Ignore.",
                    AppLog::ToUtf8(incomingName),
                    AppLog::ToUtf8(currentName));
                return false;
            }

            FIC2_LOG_INFO("[IPC] UI: filename match! Opening incoming path in split pane: {}",
                AppLog::ToUtf8(incomingFilePath));
            auto vp = Floar::VirtualPath::Parse(incomingFilePath);
            if (vp)
            {
                SplitHorizontalWithFile(*vp);
            }
            return true;
        }

        std::wstring GetRequestedFilePath() const
        {
            return m_paths.requested;
        }

        std::wstring GetDisplayedFilePath() const
        {
            return m_paths.displayed;
        }

        std::wstring GetCurrentFolderPath() const
        {
            return m_currentFolder.wstring();
        }

        // Captures host-tree split ratios (preorder), including the two-row
        // vertical split when present. Stops at compare pane roots so per-browser
        // main/thumb vertical splits are not persisted.
        std::vector<float> CaptureHorizontalSplitRatios() const
        {
            return m_splitPanes.CaptureRatios();
        }

        void ApplyHorizontalSplitRatios(const std::vector<float>& ratios)
        {
            m_splitPanes.ApplyRatios(ratios);
            if (BackplateRef() != nullptr)
            {
                DeferredRenderScope::RequestLayout(BackplateRef());
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
            if (!vp || !Floar::VirtualFileSystem::Exists(*vp))
            {
                return;
            }

            auto parent = vp->GetParent();
            m_currentFolder = parent;

            RebuildThumbList(*vp);

            // Select/apply exact match if present.
            for (size_t i = 0; i < ThumbItemCount(); ++i)
            {
                const ThumbModelEntry& item = m_thumbModel.At(i);
                if (item.kind == ThumbItemKind::Image && item.path == *vp)
                {
                    SelectItemByIndex(i);
                    return;
                }
            }

            // Fallback: show first image in folder.
            for (size_t i = 0; i < ThumbItemCount(); ++i)
            {
                if (m_thumbModel.At(i).kind == ThumbItemKind::Image)
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
            AdoptThumbStripSplit(newWnd);
            m_splitPanes.Append(newWnd);

            RefreshHorizontalHostLayout();
        }

        // A pane added beside this root host starts from the root's thumbnail
        // split ratio instead of the default one. Side-by-side panes share one
        // height, so their strips line up from the first layout, also after the
        // height was changed by a splitter drag (the synced height is pushed to
        // the panes that exist then, not to later ones). The requested ratio,
        // not the effective one: while the strip is clamped to its min/max
        // height they differ, and only the requested one keeps the panes
        // aligned once a resize releases the clamp.
        void AdoptThumbStripSplit(const std::shared_ptr<FD2D::Wnd>& pane) const
        {
            const auto browser = std::dynamic_pointer_cast<ImageBrowserImpl>(pane);
            if (browser != nullptr && browser->m_rootSplit != nullptr && m_rootSplit != nullptr)
            {
                browser->m_rootSplit->SetSplitRatio(m_rootSplit->RequestedSplitRatio());
            }
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
        static constexpr UINT CMD_FIC2_IPC_OPEN = WM_APP + 0x7A12;
        static constexpr UINT CMD_FIC2_ASYNC_THUMB_READY = kFolderListingReadyMessage;

        enum class DeferredActionKind
        {
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
            // The row gets the strip height without its chrome: the scrollbar
            // gutter (always budgeted, see kStripChrome) and the selection room
            // above and below.
            const ImageBrowserThumbLayoutCoordinator::ThumbSizeInput sizingInput
            {
                paneH - FD2D::ScrollView::kScrollBarGutter,
                m_thumbH,
                (m_rootSplit != nullptr && m_rootSplit->IsSplitterDragging()),
                CommonUtil::NowMs(),
                m_lastThumbSizingApplyMs,
                kThumbMinSide,
                kThumbMaxSide,
                ImageBrowserThumbnailPane::kSelectionRoom
            };
            const auto sizing = ImageBrowserThumbLayoutCoordinator::EvaluateThumbSize(sizingInput);
            if (!sizing.shouldApply)
            {
                return false;
            }
            const float newHeight = sizing.newThumbSide;

            // Resizing the attached tiles renders one frame (none inside a render).
            const DeferredRenderScope batchRender(BackplateRef());
            CaptureThumbAnchor();   // the visible anchor stays put while widths scale
            m_thumbW = newHeight; // Keep this for navigation tiles
            m_thumbH = newHeight;
            ApplyThumbRowHeightToStrip();

            if (m_thumbPane)
            {
                auto scroll = m_thumbPane->Scroll();
                if (scroll)
                {
                    scroll->SetScrollStep((std::max)(48.0f, newHeight * 0.75f));
                }
            }

            // Tiles that are bound later get the new size from the materialize context.
            for (const ThumbTile& tile : m_thumbTiles.Tiles())
            {
                if (tile.imageTile)
                {
                    // Use variable width based on aspect ratio
                    tile.imageTile->SetFixedHeight(m_thumbH);
                }
                if (tile.navTile)
                {
                    // Navigation tiles remain square (invalidates on a change)
                    tile.navTile->SetFixedSize({ m_thumbW, m_thumbH });
                }
            }
            RebuildThumbLayout();

            if (BackplateRef() != nullptr)
            {
                DeferredRenderScope::RequestLayout(BackplateRef());
            }
            m_lastThumbSizingApplyMs = sizing.updatedApplyMs;
            return true;
        }

        // While the splitter is dragged the row height keeps moving: bound
        // image tiles keep their thumbnails (scaled) and upgrade undersized
        // ones once the drag ends, instead of decoding at every bucket the
        // drag crosses (ThumbSizePolicy). Runs before the tiles render.
        void UpdateThumbSizeUpgradeDeferral()
        {
            const bool deferred = (m_rootSplit != nullptr && m_rootSplit->IsSplitterDragging());
            if (deferred == m_thumbSizeUpgradesDeferred)
            {
                return;
            }
            m_thumbSizeUpgradesDeferred = deferred;
            for (const ThumbTile& tile : m_thumbTiles.Tiles())
            {
                if (tile.imageTile)
                {
                    tile.imageTile->SetSizeUpgradesDeferred(deferred);
                }
            }
        }

        // The strip panel's height is the row height, not its tiles' (the
        // materialized range can be partial or empty).
        void ApplyThumbRowHeightToStrip()
        {
            if (auto strip = m_thumbPane ? m_thumbPane->StripPanel() : nullptr)
            {
                strip->SetRowHeight(m_thumbH);
            }
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

        std::wstring BrowserGetRequestedFilePath() const override
        {
            return GetRequestedFilePath();
        }

        std::wstring BrowserGetDisplayedFilePath() const override
        {
            return GetDisplayedFilePath();
        }

        std::wstring BrowserGetCurrentFolderPath() const override
        {
            return GetCurrentFolderPath();
        }

        bool BrowserTryGetMainImageClientRect(D2D1_RECT_F& outRect) const override
        {
            if (m_mainPane == nullptr)
            {
                return false;
            }
            return m_mainPane->TryGetMainImageRect(outRect);
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

        std::wstring BrowserGetRequestedFileNameLower() const override
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
            if (m_selectedIndex < ThumbItemCount())
            {
                ActivateSelected();
            }
        }

        void BrowserCmdSelectPrevious() override
        {
            const size_t cur = CurrentThumbSelectionIndex();
            const size_t next = (cur == 0) ? 0 : (cur - 1);
            SelectItemByIndex(next);
        }

        void BrowserCmdSelectNext() override
        {
            const size_t cur = CurrentThumbSelectionIndex();
            SelectItemByIndex(cur + 1);
        }

        void BrowserCmdSelectFirst() override
        {
            SelectItemByIndex(0);
        }

        void BrowserCmdSelectLast() override
        {
            if (ThumbItemCount() > 0)
            {
                SelectItemByIndex(ThumbItemCount() - 1);
            }
        }

        void BrowserCmdPagePrevious() override
        {
            const size_t step = PagingStepFromThumbViewport();
            const size_t cur = CurrentThumbSelectionIndex();
            const size_t next = (cur > step) ? (cur - step) : 0;
            SelectItemByIndex(next);
        }

        void BrowserCmdPageNext() override
        {
            const size_t step = PagingStepFromThumbViewport();
            const size_t cur = CurrentThumbSelectionIndex();
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

        void BrowserCmdMipPrevious() override
        {
            auto m = ActiveMainImage();
            if (!m)
            {
                return;
            }
            const uint32_t mip = m->MipLevel();
            if (mip > 0)
            {
                m->SelectMip(mip - 1);
            }
        }

        void BrowserCmdMipNext() override
        {
            auto m = ActiveMainImage();
            if (!m)
            {
                return;
            }
            const uint32_t mip = m->MipLevel();
            if (mip + 1 < m->MipLevels())
            {
                m->SelectMip(mip + 1);
            }
        }

        void BrowserCmdSelectMip(uint32_t mipLevel) override
        {
            if (auto m = ActiveMainImage())
            {
                m->SelectMip(mipLevel);
            }
        }

        void BrowserCmdSetChannelMode(int mode) override
        {
            if (auto m = ActiveMainImage())
            {
                m->SetChannelMode(mode);
            }
        }

        void BrowserCmdSetAlphaUsageOverride(ImageCore::AlphaUsage usage) override
        {
            if (auto m = ActiveMainImage())
            {
                m->SetAlphaUsageOverride(usage);
            }
        }

        void BrowserCmdShowImageInformation() override
        {
            auto m = ActiveMainImage();
            auto* ficBp = FictureBackplateRef();
            if (!m || ficBp == nullptr || ficBp->Window() == nullptr)
            {
                return;
            }
            MessageBoxW(
                ficBp->Window(),
                m->InformationText().c_str(),
                L"Image Information",
                MB_ICONINFORMATION | MB_OK);
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
            if (mainImage)
            {
                const auto info = mainImage->GetLoadedInfo();
                snapshot.hasImage = !info.sourcePath.empty() && info.width > 0 && info.height > 0;
                snapshot.alphaUsageOverride = mainImage->AlphaUsageOverride();
                snapshot.mipLevels = mainImage->MipLevels();
                snapshot.mipLevel = mainImage->MipLevel();
                snapshot.sourceWidth = info.sourceWidth ? info.sourceWidth : info.width;
                snapshot.sourceHeight = info.sourceHeight ? info.sourceHeight : info.height;
            }
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

        std::wstring BrowserRequestedImageFileNameForTitle() const override
        {
            return RequestedImageFileNameForTitle();
        }

        void BuildUi()
        {
            FIC2_TIMER_START(t_build);
            FIC2_LOG_DEBUG("[BuildUi] start (name='{}')", AppLog::ToUtf8(Name()));

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
            if (auto strip = m_thumbPane->StripPanel())
            {
                strip->SetLayout(&m_thumbViewport.Layout());
                strip->SetTiles(&m_thumbTiles);
                strip->SetOnBeforeRenderTiles([this]()
                {
                    OnBeforeRenderStripTiles();
                });
            }

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
            ApplyThumbRowHeightToStrip();
            m_thumbLabelDip = 0.0f;
            m_thumbItemSpacing = 0.0f;
            m_thumbOuterSpacing = 8.0f;

            if (!m_initialFile.empty())
            {
                FIC2_LOG_DEBUG("[BuildUi] initial file: '{}'",
                    AppLog::ToUtf8(m_initialFile));
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
                DeferredRenderScope::RequestLayout(BackplateRef());
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
            if (m_mainImage)
            {
                m_mainImage->SetOnPresentedPathChanged([this](const std::wstring& path)
                {
                    OnMainImagePresentedPathChanged(path);
                });
                // Image information follows the main image's own change
                // notification instead of being rebuilt on every frame.
                m_mainImage->SetOnInfoChanged([this]()
                {
                    RefreshInfoPanel();
                });
            }

                    if (!m_initialFile.empty())
                    {
                m_paths.SelectImage(m_initialFile);
                // Adopt anything the main image presented before the callback
                // was installed (normally nothing yet at this point).
                if (m_mainImage)
                {
                    const std::wstring presented = m_mainImage->GetLoadedInfo().sourcePath;
                    if (!presented.empty())
                    {
                        m_paths.OnPresented(presented);
                    }
                }
                NotifyIpcOpenSnapshotChanged();
            }

        }

        // Requested image of the main pane (see ImageBrowserPathState.h).
        std::wstring ActiveMainPath() const
        {
            return m_paths.requested;
        }

        // Main-image presentation boundary: the displayed path follows only the
        // current request, and the IPC snapshot is refreshed.
        void OnMainImagePresentedPathChanged(const std::wstring& path)
        {
            if (path.empty())
            {
                m_paths.OnPresentationCleared();
            }
            else if (!m_paths.OnPresented(path))
            {
                FIC2_LOG_WARN("[PathState] Ignoring presentation of a path that is not the current request: {}",
                    AppLog::ToUtf8(path));
            }
            NotifyIpcOpenSnapshotChanged();
        }

    public:
        std::wstring RequestedImageFileNameForTitle() const
        {
            return m_paths.RequestedFileName();
        }

    private:
        // Requested image name, not the image still on screen: after a new
        // selection, sync and IPC matching must use the new name immediately.
        std::wstring ActiveMainFileNameLower() const
        {
            return m_paths.RequestedFileNameLower();
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
            const ThumbModelEntry* selected = SelectedThumbEntry();
            input.hasSelection = (selected != nullptr);
            if (selected != nullptr)
            {
                input.selectedKind = selected->kind;
                input.selectedPath = selected->path;
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
            if (!mainImage || index >= ThumbItemCount())
            {
                return;
            }

            const ThumbModelEntry& item = m_thumbModel.At(index);
            if (item.kind != ThumbItemKind::Image)
            {
                return;
            }

            mainImage->SetInteractionEnabled(true);
            mainImage->SetLoadingSpinnerEnabled(true);
            const std::wstring p = item.path.wstring();
            // The request is recorded before the load starts: a GPU cache hit
            // presents synchronously inside SetSourceFile and must match it.
            m_paths.RequestImage(p, [&]()
            {
                mainImage->SetSourceFile(p);
            });
            if (auto* ficBp = FictureBackplateRef())
            {
                ficBp->AddRecentFile(p);
            }
            NotifyIpcOpenSnapshotChanged();
            mainImage->Invalidate();
            RefreshInfoPanel();
        }

        ImageBrowserThumbStripController::SelectItemContext MakeThumbSelectContext()
        {
            ImageBrowserThumbStripController::SelectItemContext context {};
            context.model = &m_thumbModel;
            context.tiles = &m_thumbTiles;
            context.selectedIndex = &m_selectedIndex;
            context.ensureSelectionVisible = [this]()
            {
                if (m_thumbPane == nullptr || m_selectedIndex >= ThumbItemCount() || m_thumbKeepScrollOnSelect)
                {
                    return;
                }

                // Before the next layout pass (list or extents just rebuilt) the
                // panel does not match the layout yet: center when it does.
                D2D1_RECT_F stripRect {};
                if (m_thumbViewport.ArrangePending() || !ThumbLayoutReady() || !m_thumbPane->TryGetStripRect(stripRect))
                {
                    m_thumbViewport.RequestCenter();
                    return;
                }
                if (auto scroll = m_thumbPane->Scroll())
                {
                    scroll->EnsureCentered(ThumbItemRect(m_selectedIndex));
                }
            };
            context.syncSuppressBroadcast = m_syncSuppressBroadcast;
            context.imageBrowserCount = ImageBrowserCount();
            context.applyMainFromIndex = [this](size_t idx)
            {
                ApplyMainFromIndex(idx);
            };
            context.applyNonImageSelection = [this](const ThumbModelEntry& selectedItem)
            {
                if (m_mainImage)
                {
                    m_mainImage->ClearSource();
                    m_mainImage->SetInteractionEnabled(false);
                    m_mainImage->Invalidate();
                }

                if (selectedItem.kind == ThumbItemKind::Up && !m_currentFolder.empty())
                {
                    m_paths.SelectNonImage(m_currentFolder.GetDisplayPath());
                }
                else
                {
                    m_paths.SelectNonImage(selectedItem.path.GetDisplayPath());
                }
                NotifyIpcOpenSnapshotChanged();

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
            const ThumbModelEntry* selected = SelectedThumbEntry();
            if (selected != nullptr && selected->kind == ThumbItemKind::Image)
            {
                const std::wstring curLower = CommonUtil::ToLower(selected->path.filename().wstring());
                if (!curLower.empty() && curLower == fileNameLower)
                {
                    return;
                }
            }

            // Find same filename among image items in current directory.
            size_t match = static_cast<size_t>(-1);
            for (size_t i = 0; i < ThumbItemCount(); ++i)
            {
                const ThumbModelEntry& item = m_thumbModel.At(i);
                if (item.kind != ThumbItemKind::Image)
                {
                    continue;
                }

                const std::wstring nameLower = CommonUtil::ToLower(item.path.filename().wstring());
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
            // This browser and the browsers it synchronizes render once.
            const DeferredRenderScope batchRender(BackplateRef());
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
            const ThumbModelEntry* item = SelectedThumbEntry();
            if (item == nullptr)
            {
                return;
            }

            if (item->kind == ThumbItemKind::Image)
            {
                ApplyMainFromIndex(m_selectedIndex);
                return;
            }

            if (item->kind == ThumbItemKind::Up || item->kind == ThumbItemKind::Folder)
            {
                const Floar::VirtualPath folder = item->path;   // the rebuild replaces the model's items
                NavigateToFolder(folder);
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
                RebuildThumbListImmediate(preferSelectPath, true);
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

            const unsigned long long requestId = m_listing.Start(preferSelectPath, CommonUtil::NowMs());
            const std::wstring browserName = Name();
            const Floar::VirtualPath folder = m_currentFolder;
            const bool showNavItems = m_showNavItems;
            NoteListedFolder(folder);

            FIC2_LOG_INFO("[ThumbLoad] Started loading folder '{}' (browser={})",
                AppLog::ToUtf8(folder.wstring()),
                AppLog::ToUtf8(browserName));
            // The strip keeps showing the committed items until the first
            // progressive update commits the new listing.
            m_thumbModel.StageReset(folder, showNavItems, ImageFilterForThumbModel());
            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAnimationFrame();
            }

            // Supersedes the previous load of this browser (latest request wins).
            m_folderLane.Start(FolderLoadRequest { requestId, folder, preferSelectPath, showNavItems });
        }

        // A listing of `folder` starts: leaving an archive closes the idle
        // forward cursors its thumbnails left (TrimArchiveCursorsOnLeave).
        void NoteListedFolder(const Floar::VirtualPath& folder)
        {
            (void)TrimArchiveCursorsOnLeave(m_listedFolder, folder);
            m_listedFolder = folder;
        }

        bool HandleAsyncThumbReadyMessage()
        {
            DrainAsyncThumbChunks();
            return true;
        }

        void DrainAsyncThumbChunks()
        {
            const std::deque<AsyncThumbListChunkPayload> chunks = m_folderLane.Dequeue();
            if (chunks.empty())
            {
                return;
            }

            if (!m_listing.Merge(chunks, m_currentFolder, m_showNavItems, m_thumbModel))
            {
                return;
            }

            if (m_listing.ShouldApplyNow(CommonUtil::NowMs()))
            {
                ApplyProgressiveThumbUpdate(m_listing.Completed());
            }
            else
            {
                ApplyThrottledProgressiveUpdate();
            }
        }

        // Applies a pending progressive list update once the throttle interval
        // has passed. While one is pending (and only then) it keeps requesting
        // frames, so a throttled update is not left waiting for an unrelated
        // render; it stops when the update is applied or the folder completes.
        void ApplyThrottledProgressiveUpdate()
        {
            switch (m_listing.ThrottledUpdate(CommonUtil::NowMs()))
            {
            case ThumbListingSession::Throttle::Apply:
                ApplyProgressiveThumbUpdate(false);
                break;
            case ThumbListingSession::Throttle::Wait:
                if (BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestAnimationFrame();
                }
                break;
            case ThumbListingSession::Throttle::None:
                break;
            }
        }

        void ApplyProgressiveThumbUpdate(bool finalizeSelection)
        {
            if (!m_listing.UpdatePending())
            {
                return;
            }

            const bool applySelection = finalizeSelection && m_listing.Completed();
            // Unconditional (not threshold-gated): this runs both from DrainAsyncThumbChunks
            // (folder enumeration chunks arriving) and from OnRender's ~50ms poll while a
            // list is still loading — the latter has no other timing coverage, and it runs
            // on the UI thread inside every affected Render() call, which is a prime suspect
            // for "sluggish panning/low FPS for a few seconds after opening a large folder".
            FIC2_TIMER_START(t_apply);
            RebuildThumbListImmediate(
                m_listing.PreferSelectPath(),
                false,
                applySelection);
            const auto applyMs = FIC2_ELAPSED_MS(t_apply);
            FIC2_LOG_INFO(
                "[ThumbLoad] ApplyProgressiveThumbUpdate: {} items in {}ms (finalize={}, entriesAccumulated={})",
                ThumbItemCount(), applyMs, m_listing.Completed(), m_thumbModel.ListedCount());

            if (m_listing.Applied(CommonUtil::NowMs()))
            {
                const unsigned long long totalMs = CommonUtil::NowMs() - m_listing.StartMs();
                FIC2_LOG_INFO("[ThumbLoad] Progressive load complete: {} items in {}ms total",
                    ThumbItemCount(), totalMs);
            }
        }

        static ThumbItemModel::ImageFilter ImageFilterForThumbModel()
        {
            return [](const Floar::VirtualPath& p)
            {
                return ImageAwareVfs::IsImageFile(p);
            };
        }

        // Rebuilds the strip from m_thumbModel. With `listSynchronously` the
        // current folder is listed into it first; otherwise the progressively
        // staged listing is committed.
        void RebuildThumbListImmediate(
            const Floar::VirtualPath& preferSelectPath,
            bool listSynchronously,
            bool applySelection = true)
        {
            if (!m_thumbPane || !m_thumbPane->Panel())
            {
                return;
            }

            // Applying a listing renders one frame when it is done: the tile
            // setters, the selection and the main image only request it (FD2D
            // coalesces invalidations into one render per loop iteration).
            const DeferredRenderScope batchRender(BackplateRef());

            // The anchor and the previous selection refer to the items shown
            // so far, before the model's display order changes.
            CaptureThumbAnchor();
            const std::wstring previousSelectedKey = ThumbItemKey(m_selectedIndex);
            if (listSynchronously)
            {
                NoteListedFolder(m_currentFolder);
                m_thumbModel.Reset(m_currentFolder, m_showNavItems, ImageFilterForThumbModel());
                m_listing.ResetPreferKey();
                if (!m_currentFolder.hostPath.empty())
                {
                    // The same listing (and identity capture) as the
                    // progressive path, run on this thread without a stop.
                    std::vector<Floar::VirtualFileEntry> listing {};
                    std::vector<ImageSourceIdentity> identities {};
                    FilesystemFolderEnumerationBackend().Enumerate(m_currentFolder, std::stop_token {},
                        [&](std::vector<Floar::VirtualFileEntry>&& batch,
                            std::vector<ImageSourceIdentity>&& batchIdentities,
                            bool)
                        {
                            identities.resize(listing.size());
                            listing.insert(listing.end(),
                                std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
                            identities.insert(identities.end(), batchIdentities.begin(), batchIdentities.end());
                        });
                    m_listing.ResolvePreferKey(preferSelectPath, listing);
                    (void)m_thumbModel.Merge(listing, identities);
                }
            }
            else
            {
                (void)m_thumbModel.Commit();
            }

            FIC2_TIMER_START(t_rebuild);

            m_typeSelectQuery.clear();
            m_selectedIndex = static_cast<size_t>(-1);

            // The layout model first (extents and key index), then the tiles of
            // the materialized range with the resolved selection highlighted.
            RebuildThumbLayout();
            const bool hasItems = ThumbItemCount() > 0;
            const size_t selectIndex = m_listing.PreferredIndex(m_thumbModel, preferSelectPath);
            FIC2_LOG_STEP(t_rebuild, "[RebuildImmediate] RebuildThumbLayout");
            // The anchor survives a progressive merge by key; a different (or
            // newly resolved) selection is centered instead. Decided before
            // materializing: the range follows the viewport that results.
            const bool centerSelection = ThumbStripLayout::CenterAfterRebuild(
                previousSelectedKey, ThumbItemKey(selectIndex), hasItems);
            if (centerSelection)
            {
                m_thumbViewport.RequestCenter();
            }
            MaterializeThumbTiles(selectIndex);
            FIC2_LOG_STEP(t_rebuild, "[RebuildImmediate] MaterializeThumbTiles");

            // Restore selection without scrolling yet; layout hasn't been updated.
            if (!hasItems)
            {
                if (applySelection)
                {
                    // No items: clear main image and path
                    if (m_mainImage)
                    {
                        m_mainImage->ClearSource();
                        m_mainImage->SetInteractionEnabled(false);
                    }
                    m_paths.ClearSelection();
                    NotifyIpcOpenSnapshotChanged();
                    RefreshInfoPanel();
                }
            }
            else if (applySelection)
            {
                // Re-applying the same selection (the final progressive update)
                // must not re-center: the user may have scrolled away.
                m_thumbKeepScrollOnSelect = !centerSelection;
                SelectItemByIndex(selectIndex);
                m_thumbKeepScrollOnSelect = false;
            }
            else
            {
                // During progressive async updates, keep a stable selected index so keyboard
                // navigation does not reset to the first item between chunks (its tile was
                // highlighted when bound).
                m_selectedIndex = selectIndex;
            }

            if (BackplateRef() != nullptr)
            {
                DeferredRenderScope::RequestLayout(BackplateRef());
                Invalidate();
            }
        }

        ImageBrowserThumbStripController::MaterializeContext MakeThumbMaterializeContext(
            size_t selectedIndex,
            ThumbViewportBasis basis)
        {
            ImageBrowserThumbStripController::MaterializeContext context {};
            context.panel = (m_thumbPane != nullptr) ? m_thumbPane->Panel() : nullptr;
            context.backplate = BackplateRef();
            context.tiles = &m_thumbTiles;
            context.thumbW = m_thumbW;
            context.thumbH = m_thumbH;
            context.model = &m_thumbModel;
            const ThumbRanges ranges = MaterializedThumbRange(selectedIndex, basis);
            context.range = ranges.materialized;
            context.visible = ranges.visible;
            context.selectedIndex = selectedIndex;
            context.payloads = m_thumbPayloads;
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
            context.onImageAspect = [this](const std::wstring& key, float aspect)
            {
                OnThumbImageAspect(key, aspect);
            };
            context.onImageSourceLimited = [this](const std::wstring& key, uint32_t height)
            {
                (void)m_thumbModel.SetSourceLimited(key, height);
            };
            context.deferSizeUpgrades = m_thumbSizeUpgradesDeferred;
            context.makeStableName = [this](const wchar_t* prefix, const Floar::VirtualPath& p)
            {
                return MakeStableThumbName(prefix, p);
            };
            return context;
        }

        int HorizontalViewerCount() const
        {
            auto* rootHost = RootHostFromRegistry();
            if (rootHost == nullptr)
            {
                return 1;
            }

            if (rootHost->m_splitPanes.Empty())
            {
                return 1;
            }

            return static_cast<int>(rootHost->m_splitPanes.Count());
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
            if (!m_deferredActions.Push({ static_cast<int>(kind), path, text }))
            {
                FIC2_LOG_WARN("[DeferredAction] Queue full ({} actions); dropping action kind={}.",
                    ImageBrowserDeferredActions::DeferredQueue::kCapacity, static_cast<int>(kind));
                return;
            }
            PostDeferredActionWakeup();
        }

        // One wakeup per queued action, addressed to this browser, so every
        // queued action runs here, in order, whichever browser has focus.
        // Wakeups go to a message-only window of their own instead of the
        // backplate: an action never runs inside a Wnd-tree traversal, where
        // inserting or closing a pane would rebuild the split tree and destroy
        // Wnds whose OnCommandEvent is still running.
        void PostDeferredActionWakeup()
        {
            if (BackplateRef() == nullptr)
            {
                return;
            }
            const HWND window = DeferredWakeupWindow();
            if (window == nullptr || !PostMessageW(window, CMD_FIC2_DEFERRED_ACTION, m_deferredWakeupId, 0))
            {
                FIC2_LOG_WARN("[DeferredAction] Could not post a wakeup (err={}).", GetLastError());
            }
        }

        static WPARAM NextDeferredWakeupId()
        {
            static std::atomic<WPARAM> next { 1 };
            return next.fetch_add(1);
        }

        // Browsers by wakeup id. Never destroyed, so a browser outliving
        // static destruction can still unregister.
        static std::unordered_map<WPARAM, std::weak_ptr<ImageBrowserImpl>>& DeferredWakeupTargets()
        {
            static auto* targets = new std::unordered_map<WPARAM, std::weak_ptr<ImageBrowserImpl>>();
            return *targets;
        }

        static LRESULT CALLBACK DeferredWakeupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message != CMD_FIC2_DEFERRED_ACTION)
            {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            const auto& targets = DeferredWakeupTargets();
            const auto it = targets.find(wParam);
            // Retained while its action runs: the action may close its own pane.
            const std::shared_ptr<ImageBrowserImpl> browser = (it != targets.end()) ? it->second.lock() : nullptr;
            if (browser != nullptr)
            {
                (void)browser->RunDeferredAction();
            }
            return 0;
        }

        // Created by the UI thread on its first wakeup; lives as long as the process.
        static HWND DeferredWakeupWindow()
        {
            static const HWND window = []
            {
                WNDCLASSEXW wc {};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = &DeferredWakeupProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"FICture2.DeferredBrowserActions";
                (void)RegisterClassExW(&wc);
                return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                    wc.hInstance, nullptr);
            }();
            return window;
        }

    public:
        static void RegisterDeferredWakeupTarget(const std::shared_ptr<ImageBrowserImpl>& browser)
        {
            DeferredWakeupTargets()[browser->m_deferredWakeupId] = browser;
        }

    private:

        // Runs the oldest queued action; false when none is queued. The action
        // leaves the queue before it runs: it may queue more or close this pane.
        bool RunDeferredAction()
        {
            const auto action = m_deferredActions.Pop();
            if (!action)
            {
                return false;
            }

            (void)ImageBrowserDeferredActions::Dispatch(
                *action,
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
            return true;
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
            return RootHostFromRegistry() == this;
        }

        ImageBrowserImpl* DelegatedRootHostBrowser() const
        {
            auto* rootHost = RootHostFromRegistry();
            if (rootHost == nullptr || rootHost == this)
            {
                return nullptr;
            }
            return rootHost;
        }

        ImageBrowserImpl* RootHostFromRegistry() const
        {
            const auto registry = m_registry.lock();
            if (!registry)
            {
                return nullptr;
            }

            const auto browsers = registry->ImageBrowsersSnapshot();
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
                DeferredRenderScope::RequestLayout(BackplateRef());
            }
        }

        bool CanAddHorizontalViewer() const
        {
            return m_splitPanes.CanAdd();
        }

        bool EnsureHorizontalHostReady(bool enforceCapacity = false, bool showLimitMessage = false)
        {
            EnsureHorizontalHost();
            if (m_splitPanes.Empty())
            {
                return false;
            }

            if (enforceCapacity && !CanAddHorizontalViewer())
            {
                if (showLimitMessage && BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
                {
                    MessageBoxW(
                        BackplateRef()->Window(),
                        L"Maximum 8 viewers are supported.",
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
            if (m_splitPanes.RemoveAdded(name))
            {
                RefreshHorizontalHostLayout();
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

            AdoptThumbStripSplit(newWnd);
            m_splitPanes.InsertAfter(afterName, newWnd);
            RefreshHorizontalHostLayout();
        }

        void EnsureHorizontalHost()
        {
            // Pane 0 is this browser's existing UI root.
            m_splitPanes.EnsureRoot(m_rootSplit);
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
            AdoptThumbStripSplit(newBrowser);
            m_splitPanes.Append(newBrowser);

            RefreshHorizontalHostLayout();
        }

        void RebuildHorizontalHost()
        {
            // Rebuild the full host so all panes become equal width.
            // (Nested splits with 0.5 ratios produce 50/25/25 otherwise.)
            const auto host = m_splitPanes.RebuildHost();
            if (!host)
            {
                return;
            }

            // Replace this Wnd's children with the new host tree.
            ClearChildren();
            AddChild(host);
        }

        std::shared_ptr<FD2D::SplitPanel> m_rootSplit {};
        std::shared_ptr<ImageBrowserMainPane> m_mainPane {};
        std::shared_ptr<ImageBrowserMainImage> m_mainImage {};
        // Selected / requested / displayed paths (single owner; see ImageBrowserPathState.h).
        ImageBrowserPathState m_paths {};

        ImageBrowserThumbStripController m_thumbStripController {};
        std::shared_ptr<ImageBrowserThumbnailPane> m_thumbPane {};
        ThumbTileSet m_thumbTiles {};   // tiles of MaterializedThumbRange()
        // Decoded thumbnails, bounded and shared by every browser; a previous
        // folder's entries age out instead of being dropped on navigation.
        std::shared_ptr<ThumbPayloadCache> m_thumbPayloads { ThumbPayloadCache::Shared() };
        size_t m_selectedIndex { static_cast<size_t>(-1) };   // model index; its tile (if any) is highlighted
        std::wstring m_typeSelectQuery {};
        unsigned long long m_typeSelectLastInputMs { 0 };
        ULONGLONG m_lastKeyNavMs { 0 };

        Floar::VirtualPath m_currentFolder {};
        Floar::VirtualPath m_listedFolder {};   // of the last listing started (NoteListedFolder)
        bool m_showNavItems { true };
        ThumbListingSession m_listing {};   // the progressive listing of m_currentFolder
        ThumbItemModel m_thumbModel {};   // the strip's items (committed); a progressive listing is staged in it
        ThumbStripViewport m_thumbViewport {};  // geometry of m_thumbModel's items and the next layout pass's scroll adjustment
        bool m_thumbKeepScrollOnSelect { false };   // see RebuildThumbListImmediate
        FolderLoadLane m_folderLane {};
        float m_thumbW { 128.0f };
        float m_thumbH { 128.0f };
        unsigned long long m_lastThumbSizingApplyMs { 0 };
        bool m_thumbSizeUpgradesDeferred { false };   // splitter drag in progress
        bool m_pendingThumbStripBroadcast { false };
        float m_pendingThumbStripHeight { 0.0f };
        float m_thumbLabelDip { 0.0f };
        float m_thumbItemSpacing { 0.0f };
        float m_thumbOuterSpacing { 8.0f }; // spacing between thumbnail "items" in the horizontal strip

        ImageBrowserDeferredActions::DeferredQueue m_deferredActions {};
        // Never reused, so a wakeup that outlives its browser matches no one.
        const WPARAM m_deferredWakeupId { NextDeferredWakeupId() };

        std::wstring m_initialFile {};
        ImageBrowserSplitPanes m_splitPanes {};   // root host only: side-by-side panes and their host tree

        // (no focus-background state)
        bool m_syncSuppressBroadcast { false };
        bool m_viewSyncSuppressBroadcast { false };
        std::weak_ptr<BrowserRegistry> m_registry {};

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
        FD2D::GraphicsResourceStamp m_browserBackgroundBrushStamp {};
    };
}

std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, const std::wstring& initialFile)
{
    auto browser = std::make_shared<ImageBrowserImpl>(name, initialFile);
    ImageBrowserImpl::RegisterDeferredWakeupTarget(browser);
    return browser;
}

FolderLoadLane* ImageBrowserFolderLoadLane(FD2D::Wnd& browser)
{
    auto* impl = dynamic_cast<ImageBrowserImpl*>(&browser);
    return impl ? &impl->FolderLane() : nullptr;
}

ImageBrowserThumbnailPane* ImageBrowserThumbPane(FD2D::Wnd& browser)
{
    auto* impl = dynamic_cast<ImageBrowserImpl*>(&browser);
    return impl ? impl->ThumbPane() : nullptr;
}

size_t ImageBrowserQueuedDeferredActions(FD2D::Wnd& browser)
{
    auto* impl = dynamic_cast<ImageBrowserImpl*>(&browser);
    return impl ? impl->QueuedDeferredActions() : 0;
}

