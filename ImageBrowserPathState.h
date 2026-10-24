#pragma once

// ImageBrowserPathState: the selected / requested / displayed paths of one
// image browser pane. The browser owns it; the main image reports presentation
// changes into it. UI thread only.
//
//   selected  - display path of the thumbnail-strip selection (image, folder,
//               or the "up" item); empty when nothing is selected.
//   requested - image the main pane was asked to show for that selection;
//               empty when the selection is not an image.
//   displayed - image currently presented. It changes only when the main image
//               presents the current request (or clears its content), so the
//               previous image stays "displayed" while the next one loads and a
//               failed or superseded load never becomes displayed.
//
// Which path each consumer uses:
//   requested - IPC open-name snapshot, file-name selection/view sync, window
//               title, session persistence, "pane has a document" checks
//   displayed - loaded-image information and screenshots (they describe the
//               pixels on screen)
//   selected  - thumbnail-strip state and folder information

#include "CommonUtil.h"

#include <filesystem>
#include <string>

struct ImageBrowserPathState
{
    std::wstring selected {};
    std::wstring requested {};
    std::wstring displayed {};

    // An image item was selected and handed to the main image.
    void SelectImage(const std::wstring& path)
    {
        selected = path;
        requested = path;
    }

    // Records `path` as the requested image, then runs `startLoad`. The order
    // matters: the load may present synchronously (a GPU cache hit reports
    // OnPresented from inside startLoad), and that presentation must match the
    // new request rather than be rejected against the previous one.
    template <typename StartLoad>
    void RequestImage(const std::wstring& path, StartLoad&& startLoad)
    {
        SelectImage(path);
        startLoad();
    }

    // A folder or "up" item was selected: there is no requested image. The main
    // image is cleared separately (OnPresentationCleared).
    void SelectNonImage(const std::wstring& displayPath)
    {
        selected = displayPath;
        requested.clear();
    }

    void ClearSelection()
    {
        selected.clear();
        requested.clear();
    }

    // The main image presented `path` (it reports normalized paths). Only the
    // current request can become the displayed path; anything else is a stale
    // presentation and is rejected. The request's own spelling is kept, so
    // names derived from it (screenshots) keep the file's case.
    bool OnPresented(const std::wstring& path)
    {
        if (path.empty() || requested.empty() ||
            CommonUtil::NormalizePathLowerForCompare(path) != CommonUtil::NormalizePathLowerForCompare(requested))
        {
            return false;
        }
        displayed = requested;
        return true;
    }

    // The main image no longer shows any content (cleared, or GPU content lost).
    void OnPresentationCleared()
    {
        displayed.clear();
    }

    bool HasRequest() const
    {
        return !requested.empty();
    }

    // Lower-case file name of the requested image ("" when none).
    std::wstring RequestedFileNameLower() const
    {
        return requested.empty()
            ? std::wstring()
            : CommonUtil::ToLower(std::filesystem::path(requested).filename().wstring());
    }

    // File name of the requested image for the window title ("" when none).
    std::wstring RequestedFileName() const
    {
        return requested.empty() ? std::wstring() : std::filesystem::path(requested).filename().wstring();
    }
};
