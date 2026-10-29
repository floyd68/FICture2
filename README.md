# FICture2 – Ultra-Fast Image Compare & DDS Texture Viewer for Modders

**FICture2** is an ultra-lightweight, high-performance image viewer and comparison tool built **specifically for game modders and texture artists**.

It aims for a **sub-1MB executable** with **near-instant startup** — no splash screen, no heavy frameworks, no delay.
Double-click a texture and you're already comparing it.

> **FICture2** is an acronym for **Floyd's Image Compare miniaTURE 2**.

## Demo Video

[![FICture2 Demo](https://img.youtube.com/vi/2eowYOEa5dw/0.jpg)](https://youtu.be/2eowYOEa5dw)

Watch the demo: [https://youtu.be/2eowYOEa5dw](https://youtu.be/2eowYOEa5dw)

## Download

FICture2 is available on NexusMods: [https://www.nexusmods.com/fallout4/mods/100267](https://www.nexusmods.com/fallout4/mods/100267)

## What's New in v1.7

- **Fixed:** A pane added beside the others (dropping a file or folder on the right side of an image, opening another file side by side, a second instance's compare, or a restored session) now gets the same thumbnail strip height as the existing panes, also after that height was changed by dragging the splitter, instead of the default height that left the strips misaligned
- **Fixed:** Quick actions in the thumbnail strip no longer cancel each other: clicking a folder and pressing Enter or Backspace right away, double-clicking a file and then a folder, or dropping several files to add panes in quick succession now runs every action once, in the order it was made (previously a second action made before the first had run replaced it)
- **Fixed:** Closing FICture2 now stops the server that receives files opened in another FICture2 window before anything else shuts down, within 1 s even while another window is connecting or a connection stalls. A connection that sends nothing is dropped after 2 s instead of occupying the server, and up to 8 opens are handled at the same time while further ones wait their turn
- **Improved:** Selecting images no longer rewrites `FICture2.ini` for every image: Open Recent is updated in memory and saved once about a second after the last selection, and when the window closes. **Clear Recent Files** now also holds for other open FICture2 windows: entries they selected before the clear are no longer added back when they save their own recent files (the time of the last clear is kept as `RecentFilesClearedAt`)
- **Fixed:** Images in folders whose names contain characters outside the system code page (for example emoji, Japanese or Thai on a Korean or English system) are restored by the session and listed in Open Recent again: `FICture2.ini` is now saved as Unicode (UTF-16 with a BOM) instead of in the ANSI code page, which replaced those characters with `?`. An existing INI is converted on its first save and keeps all of its settings. Paths containing `;` are also kept whole (they were cut at the `;`, and Open Recent lost every entry after one), and instances that save settings at the same time no longer overwrite each other's changes
- **Fixed:** Explorer DDS thumbnails: the thumbnail handler now reads a file its stream delivers in several parts instead of failing on the first short read, releases its decoding resources on Explorer's thread after each thumbnail instead of keeping them until the thread exits, and no longer retries a DDS that DirectXTex rejects through the Windows imaging decoder
- **Improved:** Thumbnails of online-only OneDrive files (Files On-Demand) no longer download the files: like Explorer, FICture2 now shows the thumbnail Windows gets from OneDrive, so browsing and scrolling a cloud folder leaves its files online-only and uses no disk space. In a folder of 157 online-only photos, the visible thumbnails appeared after about 1.4 s the first time and 0.1 s after that, instead of about 5-7 s, and scrolling through the whole strip downloaded no file instead of one per thumbnail shown. Opening an image still downloads it, and a file Windows has no thumbnail for is still read (and downloaded) for its thumbnail
- **Improved:** Thumbnails in one pane no longer wait for slow reads in another pane: images inside a large solid archive, on a network share or in online-only OneDrive folders are read in their own queues, so a local folder's thumbnails load while those reads continue, and a slow main image inside an archive no longer holds up thumbnail decoding. In a split view with a deep folder of a synthetic 240 MB solid 7z in one pane, the other pane's local thumbnails appeared after about 0.02 s instead of about 11 s
- **Improved:** Thumbnails of folders inside large solid 7z and RAR archives load many times faster. Each thumbnail used to decompress the archive from its start up to its image; now the thumbnails on screen and in the prefetch margin are read in archive order on one retained extraction position: images passed on the way are kept for their own thumbnails (up to 64 MB), and the next thumbnails continue where the previous read stopped instead of starting over. In a synthetic 240 MB solid 7z with a folder of 160 images behind 280 MB of other files, the visible thumbnails appeared after about 11 s instead of about 110 s, jumping to the end of the folder took about 1 s instead of about 110 s, returning to the start of the folder about 11 s instead of about 115 s, and switching to the next folder about 0.6 s instead of about 110 s. The main image is read ahead of thumbnail extraction, leaving an archive releases its extraction state, and an archive whose thumbnails were just shown can be renamed over or deleted right away
- **Fixed:** Thumbnails are no longer blurry after the thumbnail row is made taller, and wide images (16:9, panoramas, camera photos) are sharp at any row height: a thumbnail is decoded at the size it is shown (the row height times the image's aspect) and is decoded once more, in size steps, when the row grows past what it holds. The current thumbnail stays visible until the sharper one is ready, dragging the splitter does not reload thumbnails while the mouse moves (they update once it is released), making the row smaller never reloads anything, and images smaller than the thumbnail (including small mipmapped DDS files) are decoded only once
- **Fixed:** A mipmapped DDS smaller than the thumbnail size no longer shows as a flat colour block in the thumbnail strip: its full-size image is used instead of its smallest (1x1) mip
- **Fixed:** An image or thumbnail whose file could not be read completely (a read error, a file that shrank while being read, or a canceled read) is no longer decoded from a buffer padded with zeros, which could show or cache a corrupted picture; the read now reports no data instead. Reading local image files is also faster, and canceling a large read stops it between 2 MB chunks
- **Improved:** Panning a zoomed image and dragging a splitter respond faster: all redraws caused by one mouse move are now drawn in a single frame (previously a pan presented about two frames per move, and a synchronized two-pane pan about four), so the view keeps up with the mouse (roughly 3-4x more mouse moves handled per second while dragging) and both synchronized panes always move in the same frame; window updates from background loading, animations and input also take turns fairly, and an idle window still renders nothing
- **Improved:** Waiting for a hover tooltip to appear or a toast (such as "Path copied to clipboard") to disappear no longer redraws the window continuously: the window renders once when the tooltip shows and once when the toast goes away. With the D2D renderer (`--renderer=d2d`), the alpha checkerboard draws only its visible part, so it stays fast at high zoom, with the same tiles and colors as before
- **Improved:** Large folders no longer make the thumbnail strip heavier: only the thumbnails on screen plus a short prefetch margin (up to 32 on each side) have tiles, bitmaps and decode requests, so UI objects and queued decodes stay the same for 100 or 10,000 images, during and after folder loading. Thumbnails that leave the margin cancel their pending loads, and recently seen thumbnails come back on scroll without being decoded (or extracted from an archive) again. Decoded thumbnails now share one bounded memory budget across all panes (256 MB, least recently shown dropped first), so thumbnail memory levels off instead of growing with every image scrolled past; an image replaced on disk gets a new thumbnail when its folder is listed again
- **Fixed:** The info bar names the presented format of sRGB and signed block-compressed DDS textures shown on the GPU (for example `BC1 sRGB`, `BC7 sRGB`, `BC5 SNORM`) and shows its bit depth instead of `Unknown` and `x ?`; images converted on the CPU (including uncompressed sRGB DDS) keep showing `B8G8R8A8`
- **Fixed:** sRGB DDS textures (`_SRGB` formats such as BC1/BC3/BC7 or uncompressed RGBA8/BGRA8) are no longer shown too dark in thumbnails, Explorer thumbnails, the D2D renderer or the main view, including block-compressed sRGB DDS shown on the GPU by the default D3D renderer (also in R/G/B/A channel isolation and when served from the GPU cache): their stored colors are kept instead of being converted to linear values and displayed as if they were display-encoded
- **Fixed:** Going up from (or into) a large folder no longer freezes the window for a second or more while its thumbnails are still loading: a new folder listing, a row-height change, toggling the navigation items and restoring a session now draw one frame instead of one frame per tile setting, so repeated double-clicks on the Up tile are no longer replayed as extra navigations afterwards
- **Fixed:** Switching to an image that is not yet in the GPU cache (for example with the previous/next keys) no longer flashes the empty background for a frame: the previous image stays until the new one is drawn in its place, a transparent or differently sized new image is never drawn over the previous one, and the image information switches in the same frame as the picture
- **Fixed:** Leaving a folder inside a large (for example solid 7z) archive no longer stalls the next folder's thumbnails: the archive read still in progress is canceled (while the archive is opening, waiting for it, or extracting), so the new folder's thumbnails load at once instead of after the old extraction finishes; canceling a full-resolution image inside an archive stops its read the same way
- **Fixed:** Exit no longer hangs on a stuck image or archive read: folder listing, image decoding and archive opening each get a bounded time (2 s) to stop, and a service that does not stop in time ends the process without further cleanup (logged when logging is on)
- **Improved:** Thumbnails on screen load before offscreen ones, and thumbnails that scroll into view while a large folder is still loading move ahead of the queue (the main image still loads first)
- **Improved:** An idle window no longer keeps rendering: the selection outline breathes briefly after a selection instead of forever, synchronized panes no longer redraw each other continuously, and folder loading no longer polls every frame (idle CPU drops from ~7–12% of a core to ~0)
- **Fixed:** Replacing an image file (or its archive) and returning to it showed the old picture from the GPU cache; a graphics target recreation no longer drops GPU (channel-isolation) presentation, and device loss or renderer fallback rebuilds the image from the right source
- **Fixed:** Opening a file from another instance for side-by-side compare, the window title, and view sync now follow the newly selected image immediately instead of the previous image still on screen; screenshots are named after the image actually shown
- **Fixed:** Thumbnails of images inside archives could fail to load during fast scrolling or folder changes when an earlier request for the same image was canceled
- **Fixed:** Fast image switching could leave the main image or a thumbnail stuck loading, or drop the newest image, when an older decode failed or finished at the wrong moment; full-resolution decodes no longer run at background priority
- **Fixed:** With logging enabled, opening, restoring, or receiving from another instance a path that contains characters outside the system code page no longer crashes, and the log file can be written when FICture2 is installed in such a folder
- **Fixed:** ZIP, 7z and RAR archives whose path or entry names contain characters outside the system code page can be opened and extracted, and archive logging can no longer break opening or extraction
- **Fixed:** Rotated D2D images preserve their parent's placement and scaling
- **Fixed:** A crash when an image whose path contains characters outside the system code page (for example emoji, or Japanese or Thai on a Korean or English system) failed to decode
- **Fixed:** An intermittent hang on exit (and a way for thumbnails to stall) caused by decode threads missing their shutdown or cache-space wakeup
- **Fixed:** A redraw-signal race that could leave decoded images waiting for another repaint
- **Fixed:** Thumbnails now load when only one decode worker is available, including the default configuration on two-hardware-thread systems
- **New:** DDS mip inspection — **Ctrl + [** / **Ctrl + ]** step mip levels; right-click **Mip Level** picks a level. Current mip and source dimensions show in the info bar
- **New:** Channel isolation — **R** / **G** / **B** / **A** show one channel as grayscale; **N** restores RGBA (D3D path)
- **New:** Alpha Channel override — right-click chooses Auto / Transparency / Opaque (data). Encoding (straight/premultiplied) is kept separate from usage so data-bearing alpha no longer darkens colors by mistake
- **New:** Image Information — press **I** (or use the context menu) for path, format, mip, alpha, channel, and view state
- **New:** Compare up to **8** viewers in an equal-width grid (5–6 panes: 2×3; 7–8 panes: 2×4)
- **New:** Open Recent (MRU) from the right-click menu, with Clear Recent Files
- **New:** Save Pane Screenshot from the composed offscreen frame — not blocked by overlapping windows
- **New:** BSA archive browsing alongside ZIP / 7Z / RAR / BA2; archive readers are cached for snappier large-pack browsing
- **New:** Thumbnail-strip horizontal scrollbar when the strip overflows
- **Improved:** Path / info bar tooltips; right-click the path to copy it
- **Changed:** Hotkeys — navigation toggle is now **Alt + N**, sampling quality is **Alt + Q**, alpha checkerboard is **Alt + A** (**N** is used to restore RGBA after channel isolation)

## Why FICture2 exists

Most image viewers claim to support DDS — but in reality:

- BCn formats are often **decoded incorrectly**
- Alpha channels are **mishandled or ignored**
- Mip levels are **flattened or forced**
- Performance collapses on large texture folders

**FICture2 was built because no existing lightweight tool handled DDS BCn textures correctly and fast enough for real modding workflows.**

## Proper DDS & BCn support (not “partial” support)

- Native DDS support via **DirectXTex**
- Correct decoding of:
  - BC1 / BC3 / BC4 / BC5 / BC6H / BC7
- Accurate alpha channel handling
- No forced recompression
- No incorrect color-space shortcuts

What you see in FICture2 is what the engine actually gets.

## Fast image viewing

- Fast loading of large DDS textures
- Optimized thumbnail generation for massive mod folders
- Smooth zoom & pan with mouse-centric controls
- Designed for inspection, not editing

## Image compare mode (side-by-side)

- Compare **2–8 images simultaneously**
- Perfect for A/B testing multiple texture replacements
- Split-view comparison with synchronized navigation

### Folder-to-folder sync (compare mode)

When comparing two folders:

- Selecting an image in one panel automatically selects the **same filename** in the other
- Ideal for:
  - Vanilla vs modded textures
  - Competing retextures
  - Conflict resolution

## Built for Vortex Mod Manager users

- Automatic **side-by-side comparison of conflicted files**
- Files with the same filename are paired automatically
- Zero manual setup

FICture2 is a **natural companion tool for Vortex**.

## Modder-centric image browser

- Thumbnail list includes:
  - Images
  - Subfolders (including `..`)
  - Folder previews with embedded image thumbnails
- Handles real mod directory structures

## Ultra-light, zero bloat

- Executable size target: **< 1 MB** (size-first release builds)
- Near-instant cold start
- No runtime frameworks
- No background services

## Who this is for

- Modders working heavily with DDS / BCn textures
- Anyone resolving mod conflicts visually
- Anyone tired of opening Photoshop just to *check* a texture

## Status

- Actively developed
- Performance-first design
- Feedback welcome

> **A sub-1MB, ultra-fast image viewer that finally handles DDS BCn textures correctly — built for modders.**

## Tech overview

This repository is the “app shell” that wires the UI and decode pipeline together:

- **`FD2D/` (submodule)**: lightweight Win32 UI framework using Direct2D/DirectWrite (and optional D3D11 swapchain renderer)
- **`ImageCore/` (submodule)**: async image decode pipeline (WIC + DirectXTex)
- **`IniStore.h`**: shared header-only Unicode INI reader/writer with cross-process transactions (mirrored to NIFDiff via `scripts/sync-ini-store.ps1`)
- **`external/DirectXTex/` (submodule)**: DirectXTex library (external dependency)
- **`external/libarchive/` (submodule)**: ZIP/7Z/RAR archive support (built as a minimal static library)

## Installation & running

### Requirements

- Windows 10/11 (x64)
- Visual Studio 2022 with **Desktop development with C++**
- Windows 10/11 SDK

### Build

1. Clone repo with submodules:
   - `git clone --recurse-submodules https://github.com/floyd68/FICture2.git`
2. Open `FICture2.slnx` in Visual Studio
3. Select **Release x64**
4. Build `FICture2` project

### Build (CMake)

1. Clone repo with submodules:
   - `git clone --recurse-submodules https://github.com/floyd68/FICture2.git`
2. Configure:
   - `cmake -S . -B build -G "Visual Studio 18 2026" -A x64`
3. Build:
   - `cmake --build build --config Release`
4. Run:
   - `build\Release\FICture2.exe`

Notes:
- CMake builds libarchive from source by default (`FICTURE2_USE_LIBARCHIVE_CMAKE=ON`).
- Local zlib/xz submodules are used by default (`FICTURE2_USE_LOCAL_ARCHIVE_DEPS=ON`).

### CMake options

Top-level options:
- `FICTURE2_BUILD_THUMBNAIL_PROVIDER` (default: `ON`): build the `ThumbnailProvider` DLL.
- `FICTURE2_USE_LIBARCHIVE_CMAKE` (default: `ON`): build `external/libarchive` via CMake and link `archive_static`.
- `FICTURE2_USE_LOCAL_ARCHIVE_DEPS` (default: `ON`): build `external/zlib` and `external/xz` locally and wire them into libarchive.

DirectXTex (forced in root CMake):
- `BUILD_TOOLS=OFF`: do not build `texconv/texdiag` tools.
- `BUILD_SAMPLE=OFF`: do not build DirectXTex sample projects.
- `BUILD_SHARED_LIBS=OFF`: build DirectXTex as a static library.

libarchive (forced in root CMake):
- Compression deps:
  - `ENABLE_ZLIB=ON`: ZIP/deflate support.
  - `ENABLE_LZMA=ON`: 7z/XZ support (via liblzma).
  - `ENABLE_BZip2=OFF`, `ENABLE_ZSTD=OFF`, `ENABLE_LZ4=OFF`, `ENABLE_LZO=OFF`: disabled to minimize size.
- Crypto backends:
  - `ENABLE_CNG=ON`: use Windows CNG (`bcrypt`) for hashes.
  - `ENABLE_OPENSSL=OFF`, `ENABLE_MBEDTLS=OFF`, `ENABLE_NETTLE=OFF`: disabled.
- Tools and extras:
  - `ENABLE_TAR=OFF`, `ENABLE_CPIO=OFF`, `ENABLE_CAT=OFF`, `ENABLE_UNZIP=OFF`: no CLI utilities.
  - `ENABLE_XATTR=OFF`, `ENABLE_ACL=OFF`, `ENABLE_ICONV=OFF`: disabled to reduce surface area.
  - `ENABLE_PCREPOSIX=OFF`, `ENABLE_PCRE2POSIX=OFF`: disable regex backends.
  - `ENABLE_TEST=OFF`, `ENABLE_INSTALL=OFF`: no tests/install targets.
- `POSIX_REGEX_LIB=LIBC`: prefer libc regex if available.

zlib (forced when `FICTURE2_USE_LOCAL_ARCHIVE_DEPS=ON`):
- `ZLIB_BUILD_STATIC=ON`: static zlib.
- `ZLIB_BUILD_SHARED=OFF`: no shared zlib.
- `ZLIB_BUILD_TESTING=OFF`: no zlib tests.
- `ZLIB_INSTALL=OFF`: no zlib install step.

xz/liblzma (forced when `FICTURE2_USE_LOCAL_ARCHIVE_DEPS=ON`):
- `BUILD_SHARED_LIBS=OFF`: static liblzma.
- `XZ_NLS=OFF`: no gettext-based translations.
- `XZ_DOC=OFF`, `XZ_DOXYGEN=OFF`: no docs.
- `XZ_TOOL_XZ=OFF`, `XZ_TOOL_XZDEC=OFF`, `XZ_TOOL_LZMADEC=OFF`,
  `XZ_TOOL_LZMAINFO=OFF`, `XZ_TOOL_SCRIPTS=OFF`,
  `XZ_TOOL_SYMLINKS=OFF`, `XZ_TOOL_SYMLINKS_LZMA=OFF`: no CLI tools or symlinks.

Override options on configure, for example:
- `cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DFICTURE2_BUILD_THUMBNAIL_PROVIDER=OFF`
- `cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DFICTURE2_USE_LOCAL_ARCHIVE_DEPS=OFF`

### libarchive (required for archive browsing)

FICture2 expects a locally built, minimal libarchive static library in:

- `external/libarchive/build-minimal/libarchive/Debug/archive.lib`
- `external/libarchive/build-minimal/libarchive/Release/archive.lib`

We keep the submodule clean and **do not commit libarchive build outputs or local patches**, so you must build it yourself:

1. Ensure the submodule exists: `git submodule update --init --recursive`
2. Run the script: `build-libarchive-minimal.ps1`

For details and the exact CMake flags used, see `docs/libarchive_integration.md`.

### Run

- Launch the built exe from `x64\Release\FICture2.exe`
- Or run from a terminal:
  - `FICture2.exe "C:\path\to\image.dds"`

## Quick start

- **Run with a file parameter**:
  - `FICture2.exe "C:\path\to\image.dds"`
  - Session restore is skipped; the given file is opened.
  - On exit, the current viewer layout becomes the saved session (a prior multi-pane session is replaced).
- **Run with no parameters**:
  - If a previous session exists with at least one path that still resolves: restores viewers + folders/images + splitters.
  - Missing session paths are skipped and the surviving viewers are compacted left-to-right.
  - If every remembered path is gone (or no session): opens the first supported image found in the current user's **Pictures** folder.
  - Closing during the first moments of startup (before restore/IPC handoff finishes) saves window placement only — it does **not** overwrite the previous session with a blank layout.
- **Window placement**: restored size/position is clamped so the **whole window** stays inside the nearest monitor work area.

## Features

- **Thumbnail strip + main image** optimized for DDS-heavy folders
  - Horizontal **scrollbar** appears when the strip overflows (drag to scroll)
- **Folder navigation** inside the thumbnail strip (includes `..` “up” item)
- **Archive browsing**: ZIP / 7Z / RAR / BA2 / BSA (images and folders)
  - Archive readers are cached so browsing large BA2/BSA packs stays responsive
- **Supported image formats**: DDS, PNG, JPG/JPEG, BMP, GIF, TIF/TIFF, TGA
- **Compare / multi-view**: open up to **8** viewers in an equal-width compare grid
  - 1–4 panes: single row
  - 5–6 panes: two rows × 3 columns
  - 7–8 panes: two rows × 4 columns
  - adding/removing a pane rebuilds equal widths; dragged splitter ratios are restored with the session
- **Sync mode (when 2+ viewers exist)**:
  - selecting an image propagates to other viewers (by filename match within their current folder)
  - zoom/pan propagates to other viewers currently showing the same filename
- **Mouse pan + smooth zoom**:
  - zoom is critically-damped spring animated
  - pointer-based zoom (the point under the mouse stays fixed)
- **Sampling quality toggle** for scaled image display (D2D + D3D11 paths)
- **DDS mip inspection**: decode and display a selected mip level from the source chain
  - **Ctrl + [** / **Ctrl + ]** step to the previous / next mip; right-click **Mip Level** picks a level
  - Current mip index and source dimensions are shown in the info bar when a chain exists
- **Channel isolation**: **R** / **G** / **B** / **A** show one channel as grayscale; **N** restores RGBA (D3D path)
- **Alpha presentation**:
  - Separates alpha *encoding* (straight / premultiplied) from alpha *usage* (transparency / data)
  - Data-bearing alpha (e.g. height in a normal map) no longer darkens the color channels by default
  - Right-click **Alpha Channel** overrides Auto / Transparency / Opaque (data)
  - **Alt + A** toggles the alpha checkerboard background (display aid only)
- **Image Information**: **I** (or context menu) shows path, format, mip, alpha, channel, and view state
- **Image rotation**: rotate the current image 90° at a time
  - `<` / `>` keys rotate left / right; right-click menu adds **Rotate 180°** and **Reset Rotation**
  - Resets automatically when a different image is selected
  - Current angle is shown next to the zoom percentage in the info bar
- **Session persistence** (per-user INI):
  - window placement (auto-saved during move/resize; clamped fully on-screen on restore)
  - open viewers + folders + selected image (saved on exit **after** startup handoff)
  - splitter positions (horizontal compare splits + thumbnail strip height)
  - missing paths are dropped on restore; an all-dead session falls through to Pictures
  - early close before handoff preserves the previous session file list
- **IPC compare strategy (single instance assist)**:
  - if another instance is already running and you launch a file:
    - if filenames match: first instance enters compare mode; second instance exits
    - if filenames differ: second instance runs independently
  - additional Explorer opens while a compare is already in progress are queued and applied as soon as capacity frees up (instead of getting stuck)
  - boot-queued IPC opens are drained once before the session becomes eligible to save
  - the server is stopped first on exit (within 1 s); a connection that sends nothing is dropped after 2 s, and at most 8 connections are served at once
- **Open Recent**: right-click menu lists recently viewed images (persisted in the INI); includes **Clear Recent Files**
- **Save Pane Screenshot**: right-click menu saves a PNG of the current main-image pane from the composed offscreen frame (independent of desktop occlusion; suggested filename next to the open file)
- **Path / info bar**:
  - hover the path for the full path tooltip; right-click the path to copy it
  - truncated info text shows a tooltip on hover
- **Drag & drop** (main image region):
  - **Left 3/4** drop zone: replace the current `ImageBrowser` folder/image with the dropped path (**red overlay** while dragging)
  - **Right 1/4** drop zone: insert a new `ImageBrowser` to the right and open the dropped path there (**green overlay** while dragging)
  - Overlays paint above the compare chrome so the tint stays visible across panes
  - **Multi-select drag & drop**: drop multiple images/folders/archives to open up to 8 viewers
  - **Unsupported files** show an X cursor and cannot be dropped
- **File associations**: register or unregister supported image types from the context menu (per-user / HKCU only; menu label toggles based on current state)
- **Explorer DDS thumbnails**: register/unregister the thumbnail provider from the context menu (admin prompt)
- **First-run file association prompt** (per-user / HKCU only)

## Hotkeys

### Global

- **Esc**: exit application

### Thumbnail list / navigation (focused `ImageBrowser`)

- **Left / Right**: move selection (includes folders and images)
- **Home / End**: jump to start / end of list
- **PgUp / PgDn**: page step selection
- **Enter**: activate selected item
  - folder: navigate into folder
  - `..`: navigate up
  - image: show in main view
- **Backspace**: navigate up (same as `..`)
- **Alt + Up**: navigate up (Explorer-style)
- **Alt + N**: toggle navigation items visibility **globally** (folders + `..`) across all `ImageBrowser` instances
- **Alt + Q**: toggle sampling quality (low/high)
- **Alt + A**: toggle alpha checkerboard
- **Ctrl + O**: open file dialog, replace current viewer image/folder context
- **Ctrl + Shift + O**: open file dialog, create a new viewer (up to 8 total); layout rebuilds to equal widths

### Main image view (focused `ImageBrowser`)

- **R** / **G** / **B** / **A**: isolate a channel as grayscale; **N**: restore RGBA
- **I**: Image Information dialog
- **<**: rotate the current image 90° left
- **>**: rotate the current image 90° right
- **Ctrl + [**: previous DDS mip level
- **Ctrl + ]**: next DDS mip level

## Mouse controls (main image)

- **Mouse wheel**: zoom in/out (smooth spring animation)
  - **Shift + wheel**: ±10% per notch
  - **Wheel**: ±50% per notch
  - Zoom is **pointer-based**: the pixel under the mouse stays fixed during zoom.
- **Left mouse drag**: pan
- **Max zoom**: 5000%

## Drag & drop (main image)

- Drag an image file/folder onto a main image view:
  - **Left 3/4**: replace current view (folder/image) — red overlay while dragging
  - **Right 1/4**: insert a new `ImageBrowser` on the right and open there — green overlay while dragging
- Multiple files/folders/archives can be dropped at once (up to 8 viewers total)
- Unsupported files show an X cursor and are rejected

## Supported formats

- **Images**: `.dds`, `.png`, `.jpg`, `.jpeg`, `.bmp`, `.gif`, `.tif`, `.tiff`, `.tga`
- **Archives**: `.zip`, `.7z`, `.rar`, `.ba2`, `.bsa` (browse images + folders)

## File associations & thumbnails

- **Register / unregister file associations**: right-click the main image area and choose **Register File Associations** or **Unregister File Associations...** (per-user / HKCU; label follows current state)
- **Register DDS thumbnails**: choose **Register Thumbnail Provider (Admin)** (prompts for elevation)
- If the thumbnail provider is already registered, the menu shows **Unregister Thumbnail Provider (Admin)**

## Command line

- **`--renderer=d3d`**: prefer D3D11 swapchain renderer (default)
- **`--renderer=d2d`**: force D2D-only renderer (more compatible)

## Configuration (INI)

All settings are stored per-user at:

- `%LOCALAPPDATA%\FICture2\FICture2.ini`

The file is saved as Unicode (UTF-16 LE with a BOM); an older ANSI file is converted on the first save. Each save rewrites the whole file at once, keeping comments and key order, so several running instances never leave a half-written file. If a save fails after the previous file was moved aside, it is moved back; if even that fails, it stays as `FICture2.ini.replace-backup`, which FICture2 reads and restores on its next save. A line starting with `;` or `#` is a comment; a `;` later in a line is part of the value (paths may contain it).

### Important keys

- **`[Window]`**
  - `Left/Top/Right/Bottom/ShowCmd`
  - Window placement is **auto-saved during move/resize** (debounced) and also saved on exit.
- **`[Viewer]`**
  - `ShowNavItems` (0/1): navigation items visibility (**global** across all `ImageBrowser` instances; toggled by **N**)
- **`[Image]`**
  - `ZoomStiffness` (10..500): higher = faster zoom spring response
- **`[Thumbnails]`**
  - `MinSize` (default 32)
  - `MaxSize` (default 256)
  - `ItemSpacing`, `Padding`, `TileLabelSpacing`
- **`[Session]`** (auto-managed)
  - `ViewerCount` (1..8)
  - `ThumbStripHeight`
  - `HorizontalSplitRatios` (host-tree splitter ratios, including the two-row vertical split when present)
  - `RecentFiles`: pipe-separated MRU list (max 12) used by **Open Recent**
  - `RecentFilesClearedAt`: time of the last **Clear Recent Files** (UTC FILETIME as a decimal number); entries selected before it in any instance are not added back
- **`[Viewer0]..[Viewer7]`** (auto-managed)
  - `CurrentFolder`
  - `DisplayedFile`

## Repository layout

- `FICture2.cpp`: app entrypoint, renderer selection, IPC integration, startup restore logic
- `ImageBrowser.cpp/.h`: core UI component (thumbnail strip + main image + multi-view + sync)
- `FD2D/`: UI framework (submodule)
- `ImageCore/`: decode scheduler/dispatcher/cache (submodule)
- `external/DirectXTex/`: DirectXTex (submodule; **not authored here**)

## Prerequisites

- Windows 10/11
- Visual Studio 2022 (C++ workload)
- Windows SDK installed (VS installer)
- `git` if you want to work with submodules

## Clone

This repo uses **git submodules**. Clone with:

```bash
git clone --recurse-submodules https://github.com/floyd68/FICture2.git
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

## Build (Visual Studio)

1. Open `FICture2.vcxproj` (or open the folder in VS).
2. Select configuration:
   - **Debug|x64** for development
   - **Release|x64** for distribution/perf/size tuning
3. Build + run `FICture2`.

## Build (MSBuild CLI)

From a Developer PowerShell:

```powershell
msbuild .\FICture2.vcxproj -m -p:Configuration=Release -p:Platform=x64
```

## Release “size-first” notes

This repo can be configured to prioritize output size (even if it costs some runtime speed).
Typical knobs include:

- `/O1 /Os` (optimize for size)
- `/Ob1` (limit inlining)
- `/GS-` (disable buffer security checks; smaller & faster builds, but reduces mitigation)
- `/Gy /Gw` + `/OPT:REF /OPT:ICF` (dead stripping / folding)
- `/LTCG` (link-time code generation)

> If you want a speed-first profile again, revert the above to `/O2`, `/Ob2`, `/GS`, and `FavorSizeOrSpeed=Speed`.

## Submodules

- `FD2D`: `https://github.com/floyd68/FD2D`
- `ImageCore`: `https://github.com/floyd68/ImageCore`
- `DirectXTex` (external): tracked as a submodule under `external/DirectXTex`

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2024 EunSuk, Lee (이은석, floyd)

## Troubleshooting

- **Submodules are empty / missing files**: run `git submodule update --init --recursive`.
- **Build fails due to locked exe**: close the running `FICture2.exe` instance.
- **DDS decode performance**: see `ImageCore/README.md` for pipeline details and tuning points.

## Network Activity

**Does FICture2 connect to the internet?**

**FICture2 itself does NOT initiate any network connections.** The application code contains no network APIs, HTTP requests, or telemetry.

However, Windows may make network requests when FICture2 runs due to system-level security features:

- **Certificate Validation**: Windows validates digital signatures of system components (WIC, COM, DirectX)
- **SmartScreen Filter**: Windows checks application reputation
- **Windows Defender**: Real-time cloud-based protection
- **Windows Update**: Component update checks

### Reducing Network Activity (v1.1+)

FICture2 v1.1+ uses **static runtime linking (`/MT`)** to eliminate dependency on VC Runtime DLLs (`vcruntime140.dll`, `msvcp140.dll`). This reduces one major source of Windows certificate validation network requests.

**Optimized app.manifest:**
- Minimal dependencies
- Windows 10/11 compatibility declarations
- Maximum compatibility (Windows 10 1809+)

Remaining network activity comes from **Windows system services only** (WIC, DirectX, Common Controls validation). This is **standard behavior for all Windows applications** using system imaging components.

**For advanced users** who need to further reduce network activity (e.g., air-gapped systems), see:
- 📄 **[docs/network_activity_faq.md](docs/network_activity_faq.md)** - Why it happens
- 📄 **[docs/disable_network_validation.md](docs/disable_network_validation.md)** - How to disable (advanced)
