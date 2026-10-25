#pragma once

#include "ImageBrowserThumbTypes.h"
#include "VirtualFileSystem.h"
#include "VirtualPath.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The thumbnail strip's items without UI objects: classification,
// de-duplication and order, kept incrementally as folder-listing chunks arrive.
//
// Display order: the Up item and then folders/archives (both only when nav items
// are shown), then images. Folders and images are each ordered by lower-cased
// file name, ties by arrival order. Each entry carries its precomputed sort and
// lookup keys, so merging a chunk sorts only that chunk and merges it linearly
// into the existing order (no full re-sort, no per-comparison case folding).
struct ThumbModelEntry
{
    ThumbItemKind kind { ThumbItemKind::Image };
    Floar::VirtualPath path {};
    bool isArchive { false };   // a folder entry that is an archive file
    std::wstring sortKey {};    // lower-cased file name
    std::wstring lookupKey {};  // lower-cased display path (de-duplication)
    uint64_t arrival { 0 };     // tie-break: listing order
};

class ThumbItemModel
{
public:
    using ImageFilter = std::function<bool(const Floar::VirtualPath&)>;

    // Starts an empty listing of `folder`. `isImage` decides which files become
    // image items (archive files are folders, other files are dropped). Known
    // aspects are kept when `folder` is the folder already listed: a rebuild of
    // the same folder reuses its tiles, which report an aspect only when their
    // bitmap changes.
    void Reset(const Floar::VirtualPath& folder, bool showNavItems, ImageFilter isImage);

    // Classifies `entries`, drops duplicates and unsupported files, and merges
    // the rest into the order. Returns how many items were added.
    size_t Merge(const std::vector<Floar::VirtualFileEntry>& entries);

    const Floar::VirtualPath& Folder() const { return m_folder; }
    bool ShowNavItems() const { return m_showNavItems; }

    // The Up item (parent folder) when nav items are shown and a parent exists.
    const std::optional<ThumbModelEntry>& Up() const { return m_up; }
    // Folders and archives in order (listed even when nav items are hidden;
    // the strip shows them only with nav items).
    const std::vector<ThumbModelEntry>& Folders() const { return m_folders; }
    const std::vector<ThumbModelEntry>& Images() const { return m_images; }

    // Items in the strip: Up + folders (with nav items) + images.
    size_t DisplayCount() const;
    // Entries merged so far, before filtering (diagnostics).
    size_t ListedCount() const { return m_listed; }

    static std::wstring SortKeyFor(const Floar::VirtualPath& path);
    static std::wstring LookupKeyFor(const Floar::VirtualPath& path);

    // Decoded aspect ratio (width / height) of an image, by lookup key; 0 while
    // unknown. Kept across merges, and across a Reset of the same folder.
    // Returns true if changed.
    bool SetAspect(const std::wstring& lookupKey, float aspect);
    float AspectOf(const std::wstring& lookupKey) const;

private:
    static void MergeSorted(std::vector<ThumbModelEntry>& into, std::vector<ThumbModelEntry>&& added);

    Floar::VirtualPath m_folder {};
    bool m_showNavItems { true };
    ImageFilter m_isImage {};
    std::optional<ThumbModelEntry> m_up {};
    std::vector<ThumbModelEntry> m_folders {};
    std::vector<ThumbModelEntry> m_images {};
    std::unordered_set<std::wstring> m_folderKeys {};
    std::unordered_set<std::wstring> m_imageKeys {};
    std::unordered_map<std::wstring, float> m_aspectByKey {};
    uint64_t m_nextArrival { 0 };
    size_t m_listed { 0 };
};
