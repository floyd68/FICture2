# FICture2 – Ultra-Fast Image Compare & DDS Texture Viewer for Modders

**FICture2** is an ultra-lightweight, high-performance image viewer and comparison tool built **specifically for game modders and texture artists**.

It aims for a **sub-1MB executable** with **near-instant startup** — no splash screen, no heavy frameworks, no delay.
Double-click a texture and you're already comparing it.

> **FICture2** is an acronym for **Floyd's Image Compare miniaTURE 2**.

## Demo Video

[![FICture2 Demo](https://img.youtube.com/vi/2eowYOEa5dw/0.jpg)](https://youtu.be/2eowYOEa5dw)

Watch the demo: [https://youtu.be/2eowYOEa5dw](https://youtu.be/2eowYOEa5dw)

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

- Compare **2–4 images simultaneously**
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
- **`external/DirectXTex/` (submodule)**: DirectXTex library (external dependency)

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

### Run

- Launch the built exe from `x64\Release\FICture2.exe`
- Or run from a terminal:
  - `FICture2.exe "C:\path\to\image.dds"`

## Quick start

- **Run with a file parameter**:
  - `FICture2.exe "C:\path\to\image.dds"`
  - Session restore is ignored; the given file is opened.
- **Run with no parameters**:
  - If a previous session exists: restores the last session (window placement + viewers + folders/images + splitters).
  - If no session exists: opens the first supported image found in the current user's **Pictures** folder.

## Features

- **Thumbnail strip + main image** optimized for DDS-heavy folders
- **Folder navigation** inside the thumbnail strip (includes `..` “up” item)
- **Compare / multi-view**: open up to **4** viewers side-by-side (equal widths)
- **Sync mode (when 2+ viewers exist)**:
  - selecting an image propagates to other viewers (by filename match within their current folder)
  - zoom/pan propagates to other viewers currently showing the same filename
- **Mouse pan + smooth zoom**:
  - zoom is critically-damped spring animated
  - pointer-based zoom (the point under the mouse stays fixed)
- **High quality filtering** for scaled image display (D2D + D3D11 path)
- **Session persistence** (per-user INI):
  - window placement (auto-saved during move/resize)
  - open viewers + folders + selected image
  - splitter positions (horizontal compare splits + thumbnail strip height)
- **IPC compare strategy (single instance assist)**:
  - if another instance is already running and you launch a file:
    - if filenames match: first instance enters compare mode; second instance exits
    - if filenames differ: second instance runs independently
- **Drag & drop** (main image region):
  - **Left 3/4** drop zone: replace the current `ImageBrowser` folder/image with the dropped path (**red overlay** while dragging)
  - **Right 1/4** drop zone: insert a new `ImageBrowser` to the right and open the dropped path there (**green overlay** while dragging)
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
- **N**: toggle navigation items visibility **globally** (folders + `..`) across all `ImageBrowser` instances
- **Ctrl + O**: open file dialog, replace current viewer image/folder context
- **Ctrl + Shift + O**: open file dialog, create a new viewer on the right (up to 4 total), equal widths

## Mouse controls (main image)

- **Mouse wheel**: zoom in/out (smooth spring animation)
  - **Shift + wheel**: ±10% per notch
  - **Wheel**: ±50% per notch
  - Zoom is **pointer-based**: the pixel under the mouse stays fixed during zoom.
- **Left mouse drag**: pan

## Drag & drop (main image)

- Drag an image file/folder onto a main image view:
  - **Left 3/4**: replace current view (folder/image) — red overlay while dragging
  - **Right 1/4**: insert a new `ImageBrowser` on the right and open there — green overlay while dragging

## Command line

- **`--renderer=d3d`**: prefer D3D11 swapchain renderer (default)
- **`--renderer=d2d`**: force D2D-only renderer (more compatible)

## Configuration (INI)

All settings are stored per-user at:

- `%LOCALAPPDATA%\FICture2\FICture2.ini`

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
  - `ViewerCount` (1..4)
  - `ThumbStripHeight`
  - `HorizontalSplitRatios`
- **`[Viewer0]..[Viewer3]`** (auto-managed)
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
