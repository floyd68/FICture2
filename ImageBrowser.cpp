#include "ImageBrowser.h"

#include "framework.h"
#include "ThumbNavTile.h"

#include "FD2D/FD2D.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>
#include <windowsx.h>
#include <shlobj.h>

namespace
{
    class ImageBrowserImpl : public FD2D::Wnd
    {
    public:
        explicit ImageBrowserImpl(const std::wstring& name, int paneCount)
            : Wnd(name)
        {
            SetPaneCount(paneCount);
            BuildUi();
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
            UpdateThumbSizingFromPane();
            Wnd::OnRender(target);
        }

        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override
        {
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
            std::shared_ptr<FD2D::Image> image {};
            std::shared_ptr<ThumbNavTile> navTile {};
            std::shared_ptr<FD2D::Text> label {};
        };

        static constexpr UINT_PTR kThumbApplyTimerId = 0x4D21;
        static constexpr ULONGLONG kKeyRepeatMinIntervalMs = 60;
        static constexpr UINT WM_FIC2_DEFERRED_ACTION = WM_APP + 0x7A11;

        enum class DeferredActionKind
        {
            None,
            ToggleNavItems,
            NavigateToFolder,
            NavigateUp,
            ActivateSelected,
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
            const float labelLineH = m_thumbLabelDip * 1.2f;
            const float availableForThumb = paneH - (contentPadding * 2.0f) - m_thumbItemSpacing - labelLineH;

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
                if (item.image)
                {
                    item.image->SetThumbnailSize({ m_thumbW, m_thumbH });
                    item.image->Invalidate();
                }
                if (item.navTile)
                {
                    item.navTile->SetFixedSize({ m_thumbW, m_thumbH });
                    item.navTile->Invalidate();
                }
                if (item.label)
                {
                    item.label->SetFixedWidth(m_thumbW);
                    item.label->Invalidate();
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
            constexpr float thumbStripPadding = 8.0f; // matches thumbs->SetPadding(8)
            constexpr float thumbLabelPt = 8.0f;
            constexpr float thumbLabelDip = thumbLabelPt * (96.0f / 72.0f);
            constexpr float thumbLabelLineH = thumbLabelDip * 1.2f;
            constexpr float thumbItemSpacing = 2.0f; // tile <-> label spacing (constant)

            constexpr float kThumbMinSide = 32.0f;
            constexpr float kThumbMaxSide = 256.0f;

            constexpr float thumbStripMinH = (thumbStripPadding * 2.0f) + thumbItemSpacing + thumbLabelLineH + kThumbMinSide;
            constexpr float thumbStripMaxH = (thumbStripPadding * 2.0f) + thumbItemSpacing + thumbLabelLineH + kThumbMaxSide;

            rootSplit->SetSecondPaneMinExtent(thumbStripMinH);
            rootSplit->SetSecondPaneMaxExtent(thumbStripMaxH);
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

            constexpr float thumbW = 128.0f;
            constexpr float thumbH = 128.0f;
            m_thumbPanel = thumbs;
            m_thumbW = thumbW;
            m_thumbH = thumbH;
            m_thumbLabelDip = thumbLabelDip;
            m_thumbItemSpacing = thumbItemSpacing;

            std::wstring mainPath = L"D:/Works/FICture2/landscape/cavebaseground01.dds";
            m_currentFolder = std::filesystem::path(mainPath).parent_path();
            RebuildThumbList(mainPath);

            ApplyActiveSelectionStyle();
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

            std::wstring mainPath = L"D:/Works/FICture2/landscape/cavebaseground01.dds";

            if (m_paneCount == 1)
            {
                auto mainImage = std::make_shared<FD2D::Image>(L"mainImage0");
                mainImage->SetSourceFile(mainPath);
                mainImage->SetImagePurpose(ImageCore::ImagePurpose::FullResolution);
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
                    auto img = std::make_shared<FD2D::Image>(L"mainImage" + std::to_wstring(i));
                    img->SetSourceFile(mainPath);
                    img->SetImagePurpose(ImageCore::ImagePurpose::FullResolution);
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
            for (size_t i = 0; i < m_mainImages.size(); ++i)
            {
                if (m_mainImages[i])
                {
                    m_mainImages[i]->SetSelected(i == m_activePane);
                }
            }
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
            mainImage->SetImagePurpose(ImageCore::ImagePurpose::FullResolution);
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

        void RebuildThumbList(const std::filesystem::path& preferSelectPath)
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
                    tile->SetText(L"");
                    tile->SetIcon(ThumbNavTile::IconKind::Up);
                    tile->SetOnClick([this]()
                    {
                        QueueNavigateUp();
                    });

                    auto label = std::make_shared<FD2D::Text>(base + L"_label");
                    label->SetText(L"(parent)");
                    label->SetFont(L"Segoe UI", m_thumbLabelDip);
                    label->SetFixedWidth(m_thumbW);
                    label->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.75f, 1.0f));
                    label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

                    auto item = std::make_shared<FD2D::StackPanel>(base + L"_item", FD2D::Orientation::Vertical);
                    item->SetSpacing(m_thumbItemSpacing);
                    item->AddChild(tile);
                    item->AddChild(label);

                    m_thumbPanel->AddChild(item);
                    m_items.push_back({ ThumbItemKind::Up, parent, tile, nullptr, tile, label });
                }

                for (const auto& dir : folders)
                {
                    std::wstring base = L"nav_folder_" + std::to_wstring(tileId++);
                    auto tile = std::make_shared<ThumbNavTile>(base + L"_tile");
                    tile->SetFixedSize({ m_thumbW, m_thumbH });
                    tile->SetText(L"");
                    tile->SetIcon(ThumbNavTile::IconKind::Folder);
                    tile->SetOnClick([this, dir]()
                    {
                        QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                    });

                    auto label = std::make_shared<FD2D::Text>(base + L"_label");
                    label->SetText(dir.filename().wstring());
                    label->SetFont(L"Segoe UI", m_thumbLabelDip);
                    label->SetFixedWidth(m_thumbW);
                    label->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.75f, 1.0f));
                    label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    label->SetEllipsisTrimmingEnabled(true);
                    label->SetOnClick([this, dir]()
                    {
                        QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                    });

                    auto item = std::make_shared<FD2D::StackPanel>(base + L"_item", FD2D::Orientation::Vertical);
                    item->SetSpacing(m_thumbItemSpacing);
                    item->AddChild(tile);
                    item->AddChild(label);
                    m_thumbPanel->AddChild(item);

                    m_items.push_back({ ThumbItemKind::Folder, dir, tile, nullptr, tile, label });
                }
            }

            for (const auto& p : files)
            {
                std::wstring thumbName = L"thumb_" + std::to_wstring(tileId++);
                auto thumb = std::make_shared<FD2D::Image>(thumbName);
                thumb->SetThumbnailSize({ m_thumbW, m_thumbH });
                thumb->SetImagePurpose(ImageCore::ImagePurpose::Thumbnail);
                thumb->SetSourceFile(p.wstring());

                const size_t index = m_items.size();
                thumb->SetOnClick([this, index]()
                {
                    SelectItemByIndex(index, MainApplyMode::Immediate);
                });

                auto item = std::make_shared<FD2D::StackPanel>(thumbName + L"_item", FD2D::Orientation::Vertical);
                item->SetSpacing(m_thumbItemSpacing);
                item->SetPadding(0.0f);

                auto label = std::make_shared<FD2D::Text>(thumbName + L"_label");
                label->SetText(p.filename().wstring());
                label->SetFont(L"Segoe UI", m_thumbLabelDip);
                label->SetFixedWidth(m_thumbW);
                label->SetColor(D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f));
                label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                label->SetEllipsisTrimmingEnabled(true);
                label->SetOnClick([this, index]()
                {
                    SelectItemByIndex(index, MainApplyMode::Immediate);
                });

                item->AddChild(thumb);
                item->AddChild(label);
                m_thumbPanel->AddChild(item);

                m_items.push_back({ ThumbItemKind::Image, p, thumb, thumb, nullptr, label });
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
                SelectItemByIndex(selectIndex, MainApplyMode::None);
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
        float m_thumbLabelDip { 10.0f };
        float m_thumbItemSpacing { 2.0f };

        DeferredActionKind m_deferredKind { DeferredActionKind::None };
        std::filesystem::path m_deferredPath {};
    };
}

std::shared_ptr<FD2D::Wnd> CreateImageBrowser(const std::wstring& name, int paneCount)
{
    return std::make_shared<ImageBrowserImpl>(name, paneCount);
}

