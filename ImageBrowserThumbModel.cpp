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

bool ThumbItemModel::SetSourceLimited(const std::wstring& lookupKey, uint32_t height)
{
    if (height == 0)
    {
        return false;
    }
    uint32_t& stored = m_sourceLimitedByKey[lookupKey];
    if (stored >= height)
    {
        return false;
    }
    stored = height;
    return true;
}

uint32_t ThumbItemModel::SourceLimitedHeightOf(const std::wstring& lookupKey) const
{
    const auto it = m_sourceLimitedByKey.find(lookupKey);
    return (it != m_sourceLimitedByKey.end()) ? it->second : 0u;
}

void ThumbItemModel::Reset(const Floar::VirtualPath& folder, bool showNavItems, ImageFilter isImage)
{
    StageReset(folder, showNavItems, std::move(isImage));
    (void)Commit();
}

void ThumbItemModel::StageReset(const Floar::VirtualPath& folder, bool showNavItems, ImageFilter isImage)
{
    m_isImage = std::move(isImage);
    m_folderKeys.clear();
    m_imageKeys.clear();
    m_nextArrival = 0;
    m_listed = 0;
    m_stagedFolders.clear();
    m_stagedImages.clear();

    m_resetPending = true;
    m_pendingFolder = folder;
    m_pendingShowNavItems = showNavItems;
    m_pendingUp.reset();
    if (showNavItems && !folder.hostPath.empty())
    {
        Floar::VirtualPath parent = folder.GetParent();
        if (parent != folder)
        {
            ThumbModelEntry up {};
            up.kind = ThumbItemKind::Up;
            up.lookupKey = LookupKeyFor(parent);
            up.path = std::move(parent);
            m_pendingUp = std::move(up);
        }
    }
}

bool ThumbItemModel::Commit()
{
    bool changed = false;
    if (m_resetPending)
    {
        m_resetPending = false;
        if (!(m_pendingFolder == m_folder))
        {
            m_aspectByKey.clear();
        }
        // Listed again: sources are tried at larger buckets again.
        m_sourceLimitedByKey.clear();
        m_folder = std::move(m_pendingFolder);
        m_pendingFolder = {};
        m_showNavItems = m_pendingShowNavItems;
        m_up = std::move(m_pendingUp);
        m_pendingUp.reset();
        m_folders.clear();
        m_images.clear();
        changed = true;
    }
    if (!m_stagedFolders.empty() || !m_stagedImages.empty())
    {
        MergeSorted(m_folders, std::move(m_stagedFolders));
        MergeSorted(m_images, std::move(m_stagedImages));
        m_stagedFolders.clear();
        m_stagedImages.clear();
        changed = true;
    }
    if (changed)
    {
        RebuildIndex();
    }
    return changed;
}

void ThumbItemModel::RebuildIndex()
{
    const size_t count = DisplayCount();
    m_indexByKey.clear();
    m_indexByKey.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        m_indexByKey.emplace(At(i).lookupKey, i);
    }
}

std::optional<size_t> ThumbItemModel::IndexOf(const std::wstring& lookupKey) const
{
    const auto it = m_indexByKey.find(lookupKey);
    if (it == m_indexByKey.end())
    {
        return std::nullopt;
    }
    return it->second;
}

size_t ThumbItemModel::Merge(
    const std::vector<Floar::VirtualFileEntry>& entries,
    const std::vector<ImageSourceIdentity>& identities)
{
    const size_t added = Stage(entries, identities);
    (void)Commit();
    return added;
}

size_t ThumbItemModel::Stage(
    const std::vector<Floar::VirtualFileEntry>& entries,
    const std::vector<ImageSourceIdentity>& identities)
{
    m_listed += entries.size();
    auto& folders = m_stagedFolders;
    auto& images = m_stagedImages;
    const size_t stagedBefore = folders.size() + images.size();

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Floar::VirtualFileEntry& entry = entries[i];
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
        if (i < identities.size())
        {
            item.identity = identities[i];
        }
        (isFolder ? folders : images).push_back(std::move(item));
    }

    return folders.size() + images.size() - stagedBefore;
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

const ThumbModelEntry& ThumbItemModel::At(size_t index) const
{
    if (m_showNavItems)
    {
        if (m_up)
        {
            if (index == 0)
            {
                return *m_up;
            }
            --index;
        }
        if (index < m_folders.size())
        {
            return m_folders[index];
        }
        index -= m_folders.size();
    }
    return m_images[index];
}
