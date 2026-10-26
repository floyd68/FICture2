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
//
// Staged listing: a progressive listing classifies and de-duplicates chunks as
// they arrive (Stage) but changes the displayed items only on Commit, which the
// strip runs when it rebuilds. Display indices therefore stay those of the last
// rebuild, so the strip's layout, tiles and selection index can refer to the
// model by index between rebuilds. Reset and Merge are a stage plus a commit.
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
    // As Reset, but the displayed items (and aspects) are replaced only by the
    // next Commit; later Stage calls de-duplicate against the new listing.
    void StageReset(const Floar::VirtualPath& folder, bool showNavItems, ImageFilter isImage);

    // Classifies `entries`, drops duplicates and unsupported files, and merges
    // the rest into the order. Returns how many items were added.
    size_t Merge(const std::vector<Floar::VirtualFileEntry>& entries);
    // As Merge, but the items join the display order only on the next Commit.
    size_t Stage(const std::vector<Floar::VirtualFileEntry>& entries);
    // Applies a staged reset and merges the staged items into the display
    // order. Returns true if the displayed items changed.
    bool Commit();

    // The displayed (committed) listing.
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
    // Item `index` (< DisplayCount()) in display order.
    const ThumbModelEntry& At(size_t index) const;
    // Entries staged since the last reset, before filtering (diagnostics).
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

    // Displayed (committed) items.
    Floar::VirtualPath m_folder {};
    bool m_showNavItems { true };
    std::optional<ThumbModelEntry> m_up {};
    std::vector<ThumbModelEntry> m_folders {};
    std::vector<ThumbModelEntry> m_images {};
    std::unordered_map<std::wstring, float> m_aspectByKey {};

    // The listing being staged: classification, de-duplication keys, and what
    // the next Commit applies.
    ImageFilter m_isImage {};
    std::unordered_set<std::wstring> m_folderKeys {};
    std::unordered_set<std::wstring> m_imageKeys {};
    uint64_t m_nextArrival { 0 };
    size_t m_listed { 0 };
    bool m_resetPending { false };
    Floar::VirtualPath m_pendingFolder {};
    bool m_pendingShowNavItems { true };
    std::optional<ThumbModelEntry> m_pendingUp {};
    std::vector<ThumbModelEntry> m_stagedFolders {};
    std::vector<ThumbModelEntry> m_stagedImages {};
};
