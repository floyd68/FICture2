#include "ImageBrowserThumbStripController.h"

#include "FD2D/FD2D.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"
#include "VirtualFileSystem.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace
{
    std::wstring ToLower(std::wstring s)
    {
        for (auto& c : s)
        {
            c = static_cast<wchar_t>(towlower(c));
        }
        return s;
    }
}

ImageBrowserThumbStripController::BuildResult ImageBrowserThumbStripController::Build(
    const std::shared_ptr<FD2D::SplitPanel>& rootSplit) const
{
    BuildResult result {};
    if (!rootSplit)
    {
        return result;
    }

    auto thumbs = std::make_shared<FD2D::StackPanel>(L"thumbs", FD2D::Orientation::Horizontal);
    thumbs->SetSpacing(4.0f);
    thumbs->SetPadding(4.0f);

    auto thumbScroll = std::make_shared<FD2D::ScrollView>(L"thumbScroll");
    thumbScroll->SetScrollStep(96.0f);
    thumbScroll->SetSmoothTimeMs(110);
    thumbScroll->SetVerticalScrollEnabled(false);
    thumbScroll->SetContent(thumbs);

    rootSplit->SetSecondChild(thumbScroll);

    result.scroll = thumbScroll;
    result.panel = thumbs;
    return result;
}

ImageBrowserThumbStripController::RebuildResult ImageBrowserThumbStripController::RebuildList(
    const std::shared_ptr<FD2D::StackPanel>& panel,
    std::vector<ThumbItem>& items,
    float thumbW,
    float thumbH,
    bool showNavItems,
    const VirtualPath& currentFolder,
    const VirtualPath& preferSelectPath,
    const std::function<void(size_t)>& onSelectIndex,
    const std::function<void(size_t)>& onActivateIndex,
    const std::function<bool(const VirtualPath&, const VirtualPath&)>& pathEquals,
    const std::function<std::wstring(const wchar_t*, const VirtualPath&)>& makeStableName,
    const std::function<bool(const VirtualPath&)>& isSupportedImage) const
{
    RebuildResult result {};
    if (!panel)
    {
        return result;
    }

    items.clear();

    std::vector<VirtualPath> folders;
    std::vector<VirtualPath> files;

    if (!currentFolder.hostPath.empty())
    {
        auto entries = VirtualFileSystem::ListDirectory(currentFolder);
        for (const auto& entry : entries)
        {
            if (entry.isDirectory || entry.path.IsArchiveFile())
            {
                folders.push_back(entry.path);
            }
            else if (isSupportedImage && isSupportedImage(entry.path))
            {
                files.push_back(entry.path);
            }
        }
    }

    std::sort(folders.begin(), folders.end(), [](const VirtualPath& a, const VirtualPath& b)
    {
        return ToLower(a.GetFilename()) < ToLower(b.GetFilename());
    });

    std::sort(files.begin(), files.end(), [](const VirtualPath& a, const VirtualPath& b)
    {
        return ToLower(a.GetFilename()) < ToLower(b.GetFilename());
    });

    std::vector<std::wstring> desiredOrder;
    desiredOrder.reserve(folders.size() + files.size() + 8);
    std::unordered_set<std::wstring> desiredNames;
    desiredNames.reserve(folders.size() + files.size() + 8);

    auto getExistingChild = [panel](const std::wstring& name) -> std::shared_ptr<FD2D::Wnd>
    {
        const auto& children = panel->Children();
        auto it = children.find(name);
        if (it == children.end())
        {
            return nullptr;
        }
        return it->second;
    };

    if (showNavItems)
    {
        VirtualPath parent = currentFolder.GetParent();
        if (parent != currentFolder)
        {
            std::wstring name = makeStableName ? makeStableName(L"nav_up", parent) : L"nav_up";
            auto tile = std::dynamic_pointer_cast<ThumbNavTile>(getExistingChild(name));
            if (!tile)
            {
                (void)panel->RemoveChild(name);
                tile = std::make_shared<ThumbNavTile>(name);
                (void)panel->AddChild(tile);
            }

            tile->SetFixedSize({ thumbW, thumbH });
            tile->SetText(L"..");
            tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
            tile->SetIcon(ThumbNavTile::IconKind::Up);
            const size_t index = items.size();
            tile->SetOnClick([onSelectIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
            });
            tile->SetOnDoubleClick([onSelectIndex, onActivateIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
                if (onActivateIndex)
                {
                    onActivateIndex(index);
                }
            });

            desiredOrder.push_back(name);
            desiredNames.emplace(name);
            items.push_back({ ThumbItemKind::Up, parent, tile, nullptr, tile, nullptr });
        }

        for (const auto& dir : folders)
        {
            std::wstring name = makeStableName ? makeStableName(L"nav_folder", dir) : L"nav_folder";
            auto tile = std::dynamic_pointer_cast<ThumbNavTile>(getExistingChild(name));
            if (!tile)
            {
                (void)panel->RemoveChild(name);
                tile = std::make_shared<ThumbNavTile>(name);
                (void)panel->AddChild(tile);
            }

            tile->SetFixedSize({ thumbW, thumbH });
            tile->SetText(dir.filename().wstring());
            tile->SetTextPlacement(ThumbNavTile::TextPlacement::Bottom);
            tile->SetIcon(ThumbNavTile::IconKind::Folder);
            const size_t index = items.size();
            tile->SetOnClick([onSelectIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
            });
            tile->SetOnDoubleClick([onSelectIndex, onActivateIndex, index]()
            {
                if (onSelectIndex)
                {
                    onSelectIndex(index);
                }
                if (onActivateIndex)
                {
                    onActivateIndex(index);
                }
            });

            desiredOrder.push_back(name);
            desiredNames.emplace(name);
            items.push_back({ ThumbItemKind::Folder, dir, tile, nullptr, tile, nullptr });
        }
    }

    for (const auto& p : files)
    {
        std::wstring name = makeStableName ? makeStableName(L"thumb_img", p) : L"thumb_img";
        auto thumbTile = std::dynamic_pointer_cast<ThumbImageTile>(getExistingChild(name));
        if (!thumbTile)
        {
            (void)panel->RemoveChild(name);
            thumbTile = std::make_shared<ThumbImageTile>(name);
            thumbTile->SetFixedHeight(thumbH);
            thumbTile->SetSourceFile(p.wstring());
            thumbTile->SetCaption(p.filename().wstring());
            (void)panel->AddChild(thumbTile);
        }
        else
        {
            thumbTile->SetFixedHeight(thumbH);
            thumbTile->SetSourceFile(p.wstring());
            thumbTile->SetCaption(p.filename().wstring());
        }

        const size_t index = items.size();
        thumbTile->SetOnClick([onSelectIndex, index]()
        {
            if (onSelectIndex)
            {
                onSelectIndex(index);
            }
        });

        desiredOrder.push_back(name);
        desiredNames.emplace(name);
        items.push_back({ ThumbItemKind::Image, p, thumbTile, thumbTile->ImageWnd(), nullptr, thumbTile });
    }

    std::vector<std::wstring> toRemove;
    for (const auto& kv : panel->Children())
    {
        if (desiredNames.find(kv.first) == desiredNames.end())
        {
            toRemove.push_back(kv.first);
        }
    }

    for (const auto& name : toRemove)
    {
        (void)panel->RemoveChild(name);
    }

    (void)panel->ReorderChildren(desiredOrder);

    result.hasItems = !items.empty();

    size_t selectIndex = 0;
    if (!preferSelectPath.empty() && pathEquals)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (pathEquals(items[i].path, preferSelectPath))
            {
                selectIndex = i;
                break;
            }
        }
    }

    result.selectIndex = selectIndex;
    return result;
}

void ImageBrowserThumbStripController::SelectItemByIndex(
    std::vector<ThumbItem>& items,
    size_t& selectedIndex,
    std::shared_ptr<FD2D::Wnd>& selectedFocus,
    const std::shared_ptr<FD2D::ScrollView>& thumbScroll,
    const std::shared_ptr<FD2D::MainImage>& mainImage,
    std::wstring& mainPath,
    const VirtualPath& currentFolder,
    ImageBrowserMainPane* mainPane,
    bool syncSuppressBroadcast,
    size_t imageBrowserCount,
    const std::function<void(size_t)>& applyMainFromIndex,
    const std::function<void()>& refreshInfo,
    const std::function<void(const std::wstring&)>& publishFileName,
    size_t index) const
{
    if (items.empty())
    {
        return;
    }

    if (index >= items.size())
    {
        index = items.size() - 1;
    }

    if (selectedIndex < items.size())
    {
        if (items[selectedIndex].image)
        {
            items[selectedIndex].image->SetSelected(false);
        }
        if (items[selectedIndex].navTile)
        {
            items[selectedIndex].navTile->SetSelected(false);
        }
    }

    selectedIndex = index;
    selectedFocus = items[index].focus;

    if (items[index].image)
    {
        items[index].image->SetSelected(true);
    }
    if (items[index].navTile)
    {
        items[index].navTile->SetSelected(true);
    }

    if (thumbScroll && selectedFocus)
    {
        // If layout isn't ready yet (e.g. command-line / IPC open during startup),
        // LayoutRect() can still be empty. In that case, Arrange() will center later.
        const D2D1_RECT_F scrollRect = thumbScroll->LayoutRect();
        const D2D1_RECT_F focusRect = selectedFocus->LayoutRect();
        const bool layoutReady =
            (scrollRect.right > scrollRect.left) &&
            (scrollRect.bottom > scrollRect.top) &&
            (focusRect.right > focusRect.left) &&
            (focusRect.bottom > focusRect.top);

        if (layoutReady)
        {
            thumbScroll->EnsureCentered(focusRect);
        }
    }

    if (items[selectedIndex].kind == ThumbItemKind::Image)
    {
        if (applyMainFromIndex)
        {
            applyMainFromIndex(selectedIndex);
        }
    }
    else if (items[selectedIndex].kind == ThumbItemKind::Folder || items[selectedIndex].kind == ThumbItemKind::Up)
    {
        if (mainImage)
        {
            mainImage->ClearSource();
            mainImage->SetInteractionEnabled(false);
            mainImage->Invalidate();
        }

        if (items[selectedIndex].kind == ThumbItemKind::Up && !currentFolder.empty())
        {
            mainPath = currentFolder.GetDisplayPath();
        }
        else
        {
            mainPath = items[selectedIndex].path.GetDisplayPath();
        }

        if (mainPane)
        {
            mainPane->ResetInfoCache();
        }

        if (refreshInfo)
        {
            refreshInfo();
        }
    }

    // Folder compare sync:
    // If we are in compare mode (2+ ImageBrowsers) and a new image is selected in the thumbnail list,
    // propagate its filename to other ImageBrowsers. Receivers select the same filename in their
    // current directory (if present).
    if (!syncSuppressBroadcast && imageBrowserCount >= 2 && selectedIndex < items.size())
    {
        const ThumbItem& item = items[selectedIndex];
        if (item.kind == ThumbItemKind::Image)
        {
            const std::wstring fileNameLower = ToLower(item.path.filename().wstring());
            if (!fileNameLower.empty() && publishFileName)
            {
                publishFileName(fileNameLower);
            }
        }
    }
}
