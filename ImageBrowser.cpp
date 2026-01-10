#include "ImageBrowser.h"

#include "framework.h"
#include "ThumbNavTile.h"
#include "ThumbImageTile.h"

#include "FD2D/FD2D.h"
#include "FD2D/MainImage.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>
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
            g_allBrowsers.push_back(this);
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
        }

        void OnAttached(FD2D::Backplate& backplate) override
        {
            Wnd::OnAttached(backplate);
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

        void OnRender(ID2D1RenderTarget* target) override
        {
            // Splitter dragging re-arranges the SplitPanel subtree directly, but may not trigger
            // a full root re-Arrange pass. Drive responsive thumbnail sizing here so it updates
            // live while dragging.
            ApplySyncedThumbStripHeightIfNeeded();
            UpdateThumbSizingFromPane();
            Wnd::OnRender(target);
        }

        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override
        {
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN)
            {
                const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (RectContainsPoint(LayoutRect(), pt))
                {
                    RequestFocus();
                }
            }

            if (message == WM_FIC2_DEFERRED_ACTION)
            {
                RunDeferredAction();
                return true;
            }

            if (message == WM_TIMER)
            {
                if (wParam == kThumbApplyTimerId)
                {
                    if (BackplateRef() != nullptr)
                    {
                        KillTimer(BackplateRef()->Window(), kThumbApplyTimerId);
                    }

                    if (m_hasPendingApply && m_pendingApplyIndex < m_items.size())
                    {
                        ApplyMainFromIndex(m_pendingApplyIndex);
                    }
                    m_hasPendingApply = false;
                    return true;
                }
            }

            // Keyboard navigation:
            if (message == WM_KEYDOWN)
            {
                const bool isRepeat = ((lParam & (1LL << 30)) != 0);

                if (wParam == 'N')
                {
                    if (!isRepeat)
                    {
                        QueueToggleNavItems();
                    }
                    return true;
                }

                if (wParam == VK_BACK)
                {
                    if (!isRepeat)
                    {
                        QueueNavigateUp();
                    }
                    return true;
                }

                if (wParam == VK_RETURN)
                {
                    if (!isRepeat && !m_items.empty() && m_selectedIndex < m_items.size())
                    {
                        ActivateSelected();
                    }
                    return true;
                }

                if ((wParam == 'O' || wParam == 'o') && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000))
                {
                    if (!isRepeat)
                    {
                        OpenFileDialog(OpenDialogMode::SplitHorizontalNewBrowser);
                    }
                    return true;
                }

                if ((wParam == 'O' || wParam == 'o') && (GetKeyState(VK_CONTROL) & 0x8000))
                {
                    if (!isRepeat)
                    {
                        OpenFileDialog(OpenDialogMode::ReplaceCurrent);
                    }
                    return true;
                }

                // Active pane selection: 1..4
                if (!isRepeat && (wParam == '1' || wParam == '2' || wParam == '3' || wParam == '4'))
                {
                    const int idx = static_cast<int>(wParam - '1');
                    SetActivePane(static_cast<size_t>(idx));
                    return true;
                }

                // Arrow key selection (throttled)
                const ULONGLONG now = GetTickCount64();
                if (now - m_lastKeyNavMs < kKeyRepeatMinIntervalMs && isRepeat)
                {
                    return true;
                }

                auto pageStep = [this]() -> size_t
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

                    // Approximate "items per page" based on thumbnail width + constant inter-item spacing.
                    // (Each item is a vertical StackPanel: [tile] + [label]. Horizontal spacing is on the parent panel.)
                    const float itemExtent = (std::max)(1.0f, m_thumbW + m_thumbOuterSpacing);
                    const int count = static_cast<int>(std::floor(w / itemExtent));
                    return static_cast<size_t>((std::max)(1, count));
                };

                if (wParam == VK_HOME)
                {
                    m_lastKeyNavMs = now;
                    if (!m_items.empty())
                    {
                        SelectItemByIndex(0, MainApplyMode::Debounced);
                    }
                    return true;
                }
                if (wParam == VK_END)
                {
                    m_lastKeyNavMs = now;
                    if (!m_items.empty())
                    {
                        SelectItemByIndex(m_items.size() - 1, MainApplyMode::Debounced);
                    }
                    return true;
                }
                if (wParam == VK_PRIOR) // Page Up
                {
                    m_lastKeyNavMs = now;
                    if (!m_items.empty())
                    {
                        const size_t step = pageStep();
                        const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                        const size_t next = (cur > step) ? (cur - step) : 0;
                        SelectItemByIndex(next, MainApplyMode::Debounced);
                    }
                    return true;
                }
                if (wParam == VK_NEXT) // Page Down
                {
                    m_lastKeyNavMs = now;
                    if (!m_items.empty())
                    {
                        const size_t step = pageStep();
                        const size_t cur = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;
                        size_t next = cur + step;
                        if (next >= m_items.size())
                        {
                            next = m_items.size() - 1;
                        }
                        SelectItemByIndex(next, MainApplyMode::Debounced);
                    }
                    return true;
                }

                if (wParam == VK_LEFT)
                {
                    m_lastKeyNavMs = now;
                    if (!m_items.empty())
                    {
                        size_t next = (m_selectedIndex == 0) ? 0 : (m_selectedIndex - 1);
                        SelectItemByIndex(next, MainApplyMode::Debounced);
                    }
                    return true;
                }
                if (wParam == VK_RIGHT)
                {
                    m_lastKeyNavMs = now;
                    if (!m_items.empty())
                    {
                        size_t next = (m_selectedIndex + 1 >= m_items.size()) ? (m_items.size() - 1) : (m_selectedIndex + 1);
                        SelectItemByIndex(next, MainApplyMode::Debounced);
                    }
                    return true;
                }
            }

            return Wnd::OnMessage(message, wParam, lParam);
        }

        bool OnFileDrop(const std::wstring& path, const POINT& clientPt) override
        {
            if (path.empty())
            {
                return false;
            }

            // Only accept drops onto the main image region (not the thumbnail strip).
            if (m_mainPaneHost == nullptr || !RectContainsPoint(m_mainPaneHost->LayoutRect(), clientPt))
            {
                return false;
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

            const std::filesystem::path p(path);
            if (std::filesystem::exists(p) && std::filesystem::is_directory(p))
            {
                QueueDeferredAction(DeferredActionKind::NavigateToFolder, p);
                return true;
            }

            QueueDeferredAction(DeferredActionKind::NavigateToFile, p);
            return true;
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

        static constexpr UINT_PTR kThumbApplyTimerId = 0x4D21;
        static constexpr ULONGLONG kKeyRepeatMinIntervalMs = 60;
        static constexpr UINT WM_FIC2_DEFERRED_ACTION = WM_APP + 0x7A11;

        enum class DeferredActionKind
        {
            None,
            ToggleNavItems,
            NavigateToFolder,
            NavigateToFile,
            SplitHorizontalWithFile,
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

            const D2D1_RECT_F scrollRect = m_thumbScroll->LayoutRect();
            const float paneH = scrollRect.bottom - scrollRect.top;
            if (paneH <= 1.0f)
            {
                return;
            }

            // Keep thumbnail spacing constant; only scale the thumbnail square with the pane height.
            constexpr float contentPadding = 8.0f; // matches thumbs->SetPadding(8)
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
            rootSplit->SetSplitRatio(0.80f);

            // Thumbnail strip sizing:
            // The splitter's min/max should match the thumbnail min/max size so resizing stays consistent.
            rootSplit->SetSecondPaneMinExtent(kThumbStripMinH);
            rootSplit->SetSecondPaneMaxExtent(kThumbStripMaxH);
            rootSplit->SetConstraintPropagation(FD2D::ConstraintPropagation::Minimum);

            AddChild(rootSplit);
            m_rootSplit = rootSplit;

            BuildMainPanes();

            auto thumbs = std::make_shared<FD2D::StackPanel>(L"thumbs", FD2D::Orientation::Horizontal);
            thumbs->SetSpacing(8.0f);
            thumbs->SetPadding(8.0f);
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

            std::wstring mainPath = !m_initialFile.empty() ? m_initialFile : L"D:/Works/FICture2/landscape/cavebaseground01.dds";
            m_currentFolder = std::filesystem::path(mainPath).parent_path();
            RebuildThumbList(mainPath);

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

            std::wstring mainPath = !m_initialFile.empty() ? m_initialFile : L"D:/Works/FICture2/landscape/cavebaseground01.dds";

            if (m_paneCount == 1)
            {
                auto mainImage = std::make_shared<FD2D::MainImage>(L"mainImage0");
                mainImage->SetSourceFile(mainPath);
                mainImage->SetOnClick([this]()
                {
                    SetActivePane(0);
                });
                ApplyIniToMainImage(*mainImage);
                m_mainImages.push_back(mainImage);
                m_mainPaneHost = mainImage;
                m_rootSplit->SetFirstChild(mainImage);
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
                    img->SetSourceFile(mainPath);
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
                m_rootSplit->SetFirstChild(grid);
            }

            ApplyActiveSelectionStyle();
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
                KillTimer(BackplateRef()->Window(), kThumbApplyTimerId);
            }

            mainImage->SetLoadingSpinnerEnabled(true);
            mainImage->SetSourceFile(m_items[index].path.wstring());
            mainImage->Invalidate();
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
                KillTimer(hwnd, kThumbApplyTimerId);
                SetTimer(hwnd, kThumbApplyTimerId, 150, nullptr);
            }
        }

        void SelectItemByIndex(size_t index, MainApplyMode mode)
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
                m_thumbScroll->EnsureCentered(m_selectedFocus->LayoutRect());
            }

            if (m_items[m_selectedIndex].kind == ThumbItemKind::Image && mode == MainApplyMode::Immediate)
            {
                ApplyMainFromIndex(m_selectedIndex);
            }
            else if (m_items[m_selectedIndex].kind == ThumbItemKind::Image && mode == MainApplyMode::Debounced)
            {
                ScheduleApply(m_selectedIndex);
            }

            m_initialThumbEnsured = true;
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

            std::filesystem::path prefer {};
            if (m_selectedIndex < m_items.size())
            {
                prefer = m_items[m_selectedIndex].path;
            }

            RebuildThumbList(prefer);
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

            m_thumbPanel->ClearChildren();
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

            if (m_showNavItems)
            {
                std::filesystem::path parent = m_currentFolder.parent_path();
                if (!parent.empty() && parent != m_currentFolder)
                {
                    std::wstring base = L"nav_up_" + std::to_wstring(tileId++);
                    auto tile = std::make_shared<ThumbNavTile>(base + L"_tile");
                    tile->SetFixedSize({ m_thumbW, m_thumbH });
                    tile->SetText(L"..");
                    tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
                    tile->SetIcon(ThumbNavTile::IconKind::Up);
                    tile->SetOnClick([this]()
                    {
                        QueueNavigateUp();
                    });

                    m_thumbPanel->AddChild(tile);
                    m_items.push_back({ ThumbItemKind::Up, parent, tile, nullptr, tile, nullptr });
                }

                for (const auto& dir : folders)
                {
                    std::wstring base = L"nav_folder_" + std::to_wstring(tileId++);
                    auto tile = std::make_shared<ThumbNavTile>(base + L"_tile");
                    tile->SetFixedSize({ m_thumbW, m_thumbH });
                    tile->SetText(dir.filename().wstring());
                    tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
                    tile->SetIcon(ThumbNavTile::IconKind::Folder);
                    tile->SetOnClick([this, dir]()
                    {
                        QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                    });

                    m_thumbPanel->AddChild(tile);
                    m_items.push_back({ ThumbItemKind::Folder, dir, tile, nullptr, tile, nullptr });
                }
            }

            for (const auto& p : files)
            {
                std::wstring thumbName = L"thumb_" + std::to_wstring(tileId++);
                auto thumbTile = std::make_shared<ThumbImageTile>(thumbName + L"_tile");
                thumbTile->SetFixedSize({ m_thumbW, m_thumbH });
                thumbTile->SetSourceFile(p.wstring());
                thumbTile->SetCaption(p.filename().wstring());

                const size_t index = m_items.size();
                thumbTile->SetOnClick([this, index]()
                {
                    SelectItemByIndex(index, MainApplyMode::Immediate);
                });
                m_thumbPanel->AddChild(thumbTile);

                m_items.push_back({ ThumbItemKind::Image, p, thumbTile, thumbTile->ImageWnd(), nullptr, thumbTile });
            }

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
                HWND hwnd = BackplateRef()->Window();
                if (hwnd != nullptr)
                {
                    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                }
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

        void QueueDeferredAction(DeferredActionKind kind, const std::filesystem::path& path = {})
        {
            m_deferredKind = kind;
            m_deferredPath = path;
            if (BackplateRef() != nullptr)
            {
                PostMessageW(BackplateRef()->Window(), WM_FIC2_DEFERRED_ACTION, 0, 0);
            }
        }

        void RunDeferredAction()
        {
            const DeferredActionKind kind = m_deferredKind;
            const std::filesystem::path path = m_deferredPath;
            m_deferredKind = DeferredActionKind::None;
            m_deferredPath.clear();

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
        std::vector<std::shared_ptr<FD2D::Image>> m_mainImages {};

        std::shared_ptr<FD2D::Wnd> m_selectedFocus {};
        std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
        std::shared_ptr<FD2D::StackPanel> m_thumbPanel {};
        std::vector<ThumbItem> m_items {};
        size_t m_selectedIndex { static_cast<size_t>(-1) };
        bool m_initialThumbEnsured { false };
        ULONGLONG m_lastKeyNavMs { 0 };
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

        std::wstring m_initialFile {};
        std::vector<std::shared_ptr<FD2D::Wnd>> m_hPanes {};
        std::shared_ptr<FD2D::Wnd> m_hHost {};
    };
}

std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, int paneCount, const std::wstring& initialFile)
{
    return std::make_shared<ImageBrowserImpl>(name, paneCount, initialFile);
}

