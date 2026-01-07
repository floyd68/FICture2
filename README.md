# FICture2

Windows DDS texture viewer/sample app built on top of:

- **`FD2D/` (submodule)**: lightweight Win32 UI framework using Direct2D/DirectWrite (and optional D3D11 for GPU blits)
- **`ImageCore/` (submodule)**: async image decode pipeline (WIC + DirectXTex)
- **`external/DirectXTex/` (submodule)**: DirectXTex library (external dependency)

This repository is the “app shell” that wires the UI (`FD2D`) and the decode pipeline (`ImageCore`) together.

## Features

- Thumbnail strip + main image preview (DDS-heavy workloads)
- Async decode/resize with prioritization: main image loads don’t get starved by thumbnail work
- DDS fast path via DirectXTex, including mip selection for thumbnails/previews
- Optional GPU path for full-resolution DDS (D3D11 SRV upload)
- Size-first Release configuration options available (see below)

## Repository layout

- `FICture2.cpp`: app UI composition (split panel, thumbnail list, selection behavior)
- `FD2D/`: UI framework (submodule)
- `ImageCore/`: decode scheduler/dispatcher/cache (submodule)
- `external/DirectXTex/`: DirectXTex (submodule; **not authored here**)

## Prerequisites

- Windows 10/11
- Visual Studio 2022 (C++ workload)
- Windows SDK installed (VS installer)
- `git` + GitHub CLI (`gh`) if you want to work with submodules/repo automation

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

- `FD2D`: https://github.com/floyd68/FD2D
- `ImageCore`: https://github.com/floyd68/ImageCore
- `DirectXTex` (external): tracked as a submodule under `external/DirectXTex`

## Troubleshooting

- **Submodules are empty / missing files**: run `git submodule update --init --recursive`.
- **Build fails due to locked exe**: close the running `FICture2.exe` instance.
- **DDS decode performance**: see `ImageCore/README.md` for pipeline details and tuning points.
