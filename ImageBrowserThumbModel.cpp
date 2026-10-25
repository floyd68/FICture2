#include "ImageBrowserThumbModel.h"

#include "ArchiveReader.h"
#include "CommonUtil.h"

#include <algorithm>
#include <iterator>

namespace
{
    bool OrderedBefore(const ThumbModelEntry& a, const ThumbModelEntry& b)
    {
        if (a.sortKey != b.sortKey)
        {
            return a.sortKey < b.sortKey;
        }
        return a.arrival < b.arrival;
    }
}

std::wstring ThumbItemModel::SortKeyFor(const Floar::VirtualPath& path)
{
    return CommonUtil::ToLower(path.GetFilename());
}

std::wstring ThumbItemModel::LookupKeyFor(const Floar::VirtualPath& path)
{
    return CommonUtil::ToLower(path.GetDisplayPath());
}

bool ThumbItemModel::SetAspect(const std::wstring& lookupKey, float aspect)
{
    if (!(aspect > 0.0f))
    {
        return false;
    }
    float& stored = m_aspectByKey[lookupKey];
    if (stored == aspect)
    {
        return false;
    }
    stored = aspect;
    return true;
}

float ThumbItemModel::AspectOf(const std::wstring& lookupKey) const
{
    const auto it = m_aspectByKey.find(lookupKey);
    return (it != m_aspectByKey.end()) ? it->second : 0.0f;
}

void ThumbItemModel::Reset(const Floar::VirtualPath& folder, bool showNavItems, ImageFilter isImage)
{
    m_folder = folder;
    m_showNavItems = showNavItems;
    m_isImage = std::move(isImage);
    m_up.reset();
    m_folders.clear();
    m_images.clear();
    m_folderKeys.clear();
    m_imageKeys.clear();
    m_aspectByKey.clear();
    m_nextArrival = 0;
    m_listed = 0;

    if (showNavItems && !folder.hostPath.empty())
    {
        Floar::VirtualPath parent = folder.GetParent();
        if (parent != folder)
        {
            ThumbModelEntry up {};
            up.kind = ThumbItemKind::Up;
            up.path = std::move(parent);
            m_up = std::move(up);
        }
    }
}

size_t ThumbItemModel::Merge(const std::vector<Floar::VirtualFileEntry>& entries)
{
    m_listed += entries.size();
    std::vector<ThumbModelEntry> folders;
    std::vector<ThumbModelEntry> images;

    for (const auto& entry : entries)
    {
        bool isFolder = entry.isDirectory;
        bool isArchive = false;
        if (!isFolder && entry.path.IsFilesystemPath() &&
            Floar::ArchiveReaderFactory::HasArchiveExtension(entry.path.hostPath.filename().wstring()))
        {
            isFolder = true;
            isArchive = true;
        }
        if (!isFolder && !(m_isImage && m_isImage(entry.path)))
        {
            continue;
        }

        std::wstring lookupKey = LookupKeyFor(entry.path);
        auto& keys = isFolder ? m_folderKeys : m_imageKeys;
        if (!keys.insert(lookupKey).second)
        {
            continue;   // already listed
        }

        ThumbModelEntry item {};
        item.kind = isFolder ? ThumbItemKind::Folder : ThumbItemKind::Image;
        item.path = entry.path;
        item.isArchive = isArchive;
        item.sortKey = SortKeyFor(entry.path);
        item.lookupKey = std::move(lookupKey);
        item.arrival = m_nextArrival++;
        (isFolder ? folders : images).push_back(std::move(item));
    }

    const size_t added = folders.size() + images.size();
    MergeSorted(m_folders, std::move(folders));
    MergeSorted(m_images, std::move(images));
    return added;
}

void ThumbItemModel::MergeSorted(std::vector<ThumbModelEntry>& into, std::vector<ThumbModelEntry>&& added)
{
    if (added.empty())
    {
        return;
    }
    // Sort the chunk (keys are precomputed), then one linear merge.
    std::sort(added.begin(), added.end(), OrderedBefore);
    const auto middle = static_cast<std::ptrdiff_t>(into.size());
    into.insert(into.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    std::inplace_merge(into.begin(), into.begin() + middle, into.end(), OrderedBefore);
}

size_t ThumbItemModel::DisplayCount() const
{
    size_t count = m_images.size();
    if (m_showNavItems)
    {
        count += m_folders.size() + (m_up ? 1 : 0);
    }
    return count;
}
