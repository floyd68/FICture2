## FICture2

Windows image viewer focused on DDS/texture workflows, built on:

- **`FD2D/` (submodule)**: lightweight Win32 UI framework using Direct2D/DirectWrite (and optional D3D11 swapchain renderer)
- **`ImageCore/` (submodule)**: async image decode pipeline (WIC + DirectXTex)
- **`external/DirectXTex/` (submodule)**: DirectXTex library (external dependency)

This repository is the “app shell” that wires the UI (`FD2D`) and the decode pipeline (`ImageCore`) together.

### Quick start

- **Run with a file parameter**:
  - `FICture2.exe "C:\path\to\image.dds"`
  - Session restore is ignored; the given file is opened.
- **Run with no parameters**:
  - If a previous session exists: restores the last session (window placement + viewers + folders/images + splitters).
  - If no session exists: opens the first supported image found in the current user's **Pictures** folder.

### Features

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
- **Drag & drop** image file onto the main image region to open
- **First-run file association prompt** (per-user / HKCU only)

### Hotkeys

#### Global

- **Esc**: exit application

#### Thumbnail list / navigation (focused `ImageBrowser`)

- **Left / Right**: move selection (includes folders and images)
- **Home / End**: jump to start / end of list
- **PgUp / PgDn**: page step selection
- **Enter**: activate selected item
  - folder: navigate into folder
  - `..`: navigate up
  - image: show in main view
- **Backspace**: navigate up (same as `..`)
- **Alt + Up**: navigate up (Explorer-style)
- **N**: toggle navigation items visibility in the thumbnail strip (folders + `..`)
- **Ctrl + O**: open file dialog, replace current viewer image/folder context
- **Ctrl + Shift + O**: open file dialog, create a new viewer on the right (up to 4 total), equal widths

### Mouse controls (main image)

- **Mouse wheel**: zoom in/out (smooth spring animation)
  - **Shift + wheel**: ±10% per notch
  - **Wheel**: ±50% per notch
  - Zoom is **pointer-based**: the pixel under the mouse stays fixed during zoom.
- **Left mouse drag**: pan

### Command line

- **`--renderer=d3d`**: prefer D3D11 swapchain renderer (default)
- **`--renderer=d2d`**: force D2D-only renderer (more compatible)

### Configuration (INI)

All settings are stored per-user at:

- `%LOCALAPPDATA%\FICture2\FICture2.ini`

#### Important keys

- **`[Window]`**
  - `Left/Top/Right/Bottom/ShowCmd`
  - Window placement is **auto-saved during move/resize** (debounced) and also saved on exit.
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

### Repository layout

- `FICture2.cpp`: app entrypoint, renderer selection, IPC integration, startup restore logic
- `ImageBrowser.cpp/.h`: core UI component (thumbnail strip + main image + multi-view + sync)
- `FD2D/`: UI framework (submodule)
- `ImageCore/`: decode scheduler/dispatcher/cache (submodule)
- `external/DirectXTex/`: DirectXTex (submodule; **not authored here**)

### Prerequisites

- Windows 10/11
- Visual Studio 2022 (C++ workload)
- Windows SDK installed (VS installer)
- `git` if you want to work with submodules

### Clone

This repo uses **git submodules**. Clone with:

```bash
git clone --recurse-submodules https://github.com/floyd68/FICture2.git
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

### Build (Visual Studio)

1. Open `FICture2.vcxproj` (or open the folder in VS).
2. Select configuration:
   - **Debug|x64** for development
   - **Release|x64** for distribution/perf/size tuning
3. Build + run `FICture2`.

### Build (MSBuild CLI)

From a Developer PowerShell:

```powershell
msbuild .\FICture2.vcxproj -m -p:Configuration=Release -p:Platform=x64
```

### Release “size-first” notes

This repo can be configured to prioritize output size (even if it costs some runtime speed).
Typical knobs include:

- `/O1 /Os` (optimize for size)
- `/Ob1` (limit inlining)
- `/GS-` (disable buffer security checks; smaller & faster builds, but reduces mitigation)
- `/Gy /Gw` + `/OPT:REF /OPT:ICF` (dead stripping / folding)
- `/LTCG` (link-time code generation)

> If you want a speed-first profile again, revert the above to `/O2`, `/Ob2`, `/GS`, and `FavorSizeOrSpeed=Speed`.

### Submodules

- `FD2D`: `https://github.com/floyd68/FD2D`
- `ImageCore`: `https://github.com/floyd68/ImageCore`
- `DirectXTex` (external): tracked as a submodule under `external/DirectXTex`

### License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2024 EunSuk, Lee (이은석, floyd)

### Troubleshooting

- **Submodules are empty / missing files**: run `git submodule update --init --recursive`.
- **Build fails due to locked exe**: close the running `FICture2.exe` instance.
- **DDS decode performance**: see `ImageCore/README.md` for pipeline details and tuning points.
