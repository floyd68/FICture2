#include "ImageBrowserThumbStripController.h"

#include "CommonUtil.h"
#include "FD2D/FD2D.h"
#include "ThumbImageTile.h"
#include "ThumbNavTile.h"
#include "VirtualFileSystem.h"
#include "ArchiveReader.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace
{
    struct FolderEntryData
    {
        VirtualPath path {};
        bool isArchive { false };
    };

    std::wstring ToUpper(std::wstring s)
    {
        for (auto& c : s)
        {
            c = static_cast<wchar_t>(towupper(c));
        }
        return s;
    }

    std::wstring GetArchiveBadgeText(const VirtualPath& path)
    {
        std::wstring ext = CommonUtil::ToLower(path.hostPath.extension().wstring());
        if (ext == L".zip" || ext == L".7z" || ext == L".rar" || ext == L".bsa" || ext == L".ba2")
        {
            if (!ext.empty() && ext.front() == L'.')
            {
                ext.erase(ext.begin());
            }
            return ToUpper(ext);
        }

        return L"";
    }

    ThumbNavTile::IconTint GetArchiveIconTint(const VirtualPath& path)
    {
        const std::wstring ext = CommonUtil::ToLower(path.hostPath.extension().wstring());
        if (ext == L".bsa" || ext == L".ba2")
        {
            return ThumbNavTile::IconTint::ArchiveBlue;
        }

        if (ext == L".zip" || ext == L".7z" || ext == L".rar")
        {
            return ThumbNavTile::IconTint::ArchiveRed;
        }

        return ThumbNavTile::IconTint::None;
    }
}

ImageBrowserThumbStripController::RebuildResult ImageBrowserThumbStripController::RebuildList(
    const RebuildListContext& context) const
{
    RebuildResult result {};
    if (!context.panel || context.items == nullptr)
    {
        return result;
    }

    const auto& panel = context.panel;
    auto& items = *context.items;
    const float thumbW = context.thumbW;
    const float thumbH = context.thumbH;
    const bool showNavItems = context.showNavItems;
    const VirtualPath& currentFolder = context.currentFolder;
    const VirtualPath& preferSelectPath = context.preferSelectPath;
    const auto& onSelectIndex = context.onSelectIndex;
    const auto& onActivateIndex = context.onActivateIndex;
    const auto& pathEquals = context.pathEquals;
    const auto& makeStableName = context.makeStableName;
    const auto& isSupportedImage = context.isSupportedImage;
    const std::vector<VirtualFileEntry>* preloadedEntries = context.preloadedEntries;

    items.clear();

    std::vector<FolderEntryData> folders;
    std::vector<VirtualPath> files;
    std::unordered_set<std::wstring> seenFolderKeys;
    std::unordered_set<std::wstring> seenFileKeys;

    if (!currentFolder.hostPath.empty())
    {
        std::vector<VirtualFileEntry> listedEntriesOwned {};
        const std::vector<VirtualFileEntry>* listedEntries = preloadedEntries;
        if (listedEntries == nullptr)
        {
            listedEntriesOwned = VirtualFileSystem::ListDirectory(currentFolder);
            listedEntries = &listedEntriesOwned;
        }

        seenFolderKeys.reserve(listedEntries->size());
        seenFileKeys.reserve(listedEntries->size());

        for (const auto& entry : *listedEntries)
        {
            const std::wstring entryKey = CommonUtil::ToLower(entry.path.GetDisplayPath());

            if (entry.isDirectory)
            {
                if (seenFolderKeys.insert(entryKey).second)
                {
                    folders.push_back({ entry.path, false });
                }
            }
            else if (entry.path.IsFilesystemPath() &&
                ArchiveReaderFactory::HasArchiveExtension(entry.path.hostPath.filename().wstring()))
            {
                if (seenFolderKeys.insert(entryKey).second)
                {
                    folders.push_back({ entry.path, true });
                }
            }
            else if (isSupportedImage && isSupportedImage(entry.path))
            {
                if (seenFileKeys.insert(entryKey).second)
                {
                    files.push_back(entry.path);
                }
            }
        }
    }

    std::sort(folders.begin(), folders.end(), [](const FolderEntryData& a, const FolderEntryData& b)
    {
        return CommonUtil::ToLower(a.path.GetFilename()) < CommonUtil::ToLower(b.path.GetFilename());
    });

    std::sort(files.begin(), files.end(), [](const VirtualPath& a, const VirtualPath& b)
    {
        return CommonUtil::ToLower(a.GetFilename()) < CommonUtil::ToLower(b.GetFilename());
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

        for (const auto& folder : folders)
        {
            const VirtualPath& dir = folder.path;
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
            if (folder.isArchive)
            {
                tile->SetBadgeText(GetArchiveBadgeText(dir));
                tile->SetIconTint(GetArchiveIconTint(dir));
            }
            else
            {
                tile->SetBadgeText(L"");
                tile->SetIconTint(ThumbNavTile::IconTint::None);
            }
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

void ImageBrowserThumbStripController::SelectItemByIndex(const SelectItemContext& context, size_t index) const
{
    if (context.items == nullptr || context.selectedIndex == nullptr || context.selectedFocus == nullptr)
    {
        return;
    }

    auto& items = *context.items;
    auto& selectedIndex = *context.selectedIndex;
    auto& selectedFocus = *context.selectedFocus;
    const auto& ensureSelectionVisible = context.ensureSelectionVisible;
    const bool syncSuppressBroadcast = context.syncSuppressBroadcast;
    const size_t imageBrowserCount = context.imageBrowserCount;
    const auto& applyMainFromIndex = context.applyMainFromIndex;
    const auto& applyNonImageSelection = context.applyNonImageSelection;
    const auto& publishFileName = context.publishFileName;

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

    if (selectedFocus && ensureSelectionVisible)
    {
        ensureSelectionVisible();
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
        if (applyNonImageSelection)
        {
            applyNonImageSelection(items[selectedIndex]);
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
            const std::wstring fileNameLower = CommonUtil::ToLower(item.path.filename().wstring());
            if (!fileNameLower.empty() && publishFileName)
            {
                publishFileName(fileNameLower);
            }
        }
    }
}
