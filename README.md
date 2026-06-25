# win-capture-sdk_qt6

This repository contains the Qt 6 viewer and the local capture/display SDK code
used by the viewer.

The source tree is intentionally limited to these runtime paths:

- `sdk/gcapture`: local capture SDK for Windows Media Foundation and DirectShow.
- `sdk/gdisplay`: local display / EDID helper SDK used by the viewer.
- `apps/qt6_viewer`: Qt viewer application.
- External `GVFG_Standalone`: GVFG capture SDK imported as prebuilt `gvfg*.dll`
  and `gvfg*.lib`.

Legacy low-level hardware backend source is not part of this project. GVFG
hardware access lives behind the external GVFG SDK.

## Build

Recommended flow:

1. Open this repository root in Qt Creator.
2. Select a Desktop Qt 6 MSVC 64-bit kit.
3. Configure.
4. Build `qt6_viewer` or `all`.

Build outputs are written under the CMake build directory:

- `bin`: executables and runtime DLLs.
- `lib`: import libraries.

## GVFG Integration

GVFG is imported from a sibling standalone SDK by default:

```text
../../GVFG_Standalone
```

The expected external layout is:

```text
GVFG_Standalone/
  sdk/gvfg/include/gvfg_capture.h
  helpers/gvfg_preview/include/gvfg_preview.h
  build/<config>/bin/gvfg.dll
  build/<config>/bin/gvfg_preview.dll
  build/<config>/lib/gvfg.lib
  build/<config>/lib/gvfg_preview.lib
```

Optional convert helper support is enabled when these files exist:

```text
helpers/gvfg_convert/include/gvfg_convert.h
build/<config>/bin/gvfg_convert.dll
build/<config>/lib/gvfg_convert.lib
```

Relevant CMake options:

```text
BUILD_GVFG_SDK=ON
GVFG_STANDALONE_ROOT=<path-to-GVFG_Standalone>
GVFG_STANDALONE_BUILD_DIR=<path-containing-bin-and-lib>
```

If `GVFG_STANDALONE_BUILD_DIR` is not set, CMake scans
`GVFG_STANDALONE_ROOT/build/*` and uses the first directory containing
`bin/gvfg.dll` and `lib/gvfg.lib`.

## Packaging

`pack_qt6_viewer.bat` copies:

- `qt6_viewer.exe`
- `gcapture.dll`
- `gdisplay.dll`
- `gvfg.dll`
- `gvfg_preview.dll`
- `gvfg_convert.dll` when present
- FFmpeg runtime DLLs when present

Then it runs `windeployqt` and creates a release zip.
