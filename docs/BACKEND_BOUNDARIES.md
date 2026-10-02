# Capture backend boundaries

The viewer has two maintained capture paths. They deliberately do not share a
backend implementation.

## GVFG Direct

`apps/qt6_viewer/gvfg` owns the viewer-side GVFG integration. It consumes only
the external `GVFG_Standalone` deliverables configured by
`GVFG_STANDALONE_ROOT` and `GVFG_STANDALONE_BUILD_DIR`:

- `gvfg` for device and frame capture
- `gvfg_preview` for D3D preview
- `gvfg_audio_playback` for WASAPI playback

GVFG capture must not be routed through `sdk/gcapture`. A frame or audio packet
owned by the GVFG SDK must be copied, or completely consumed, before its matching
release call.

`apps/qt6_viewer/app/mainwindow_gvfg.cpp` owns the MainWindow-side GVFG session
operations: capture start, recording start/stop, and GVFG-specific reporting.
The shared `mainwindow_capture.cpp` only dispatches to those operations and
maintains common UI/session state.

## DirectShow

`sdk/gcapture` owns the legacy DirectShow implementation and its public `gcap_*`
API. DirectShow provider, signal probe, custom/raw renderers, audio monitoring,
and FFmpeg recording remain inside this module.

Viewer code may call the public `gcap_*` API. It must not include DirectShow
provider headers or manipulate a DirectShow graph directly.

`apps/qt6_viewer/app/mainwindow_dshow.cpp` owns the MainWindow-side DirectShow
session operations through the public `gcap_*` API: capture start, recording
start/stop, audio monitoring, and DirectShow recording reports. The shared
`mainwindow_capture.cpp` dispatches to this file and does not implement backend
capture or recording setup itself.

## Shared recording helper

`gcapture_ffmpeg_recorder` is the explicit recording boundary shared by the
DirectShow SDK and GVFG viewer paths. The viewer links this target and does not
compile files from `sdk/gcapture/src/providers` or add that private directory to
its include path. Its implementation lives in `sdk/gcapture/src/recording`.
Capture ownership and preview remain backend-specific; only copied frame/audio
data passed to the recorder is shared.

New features and fixes should target GVFG Direct first. DirectShow maintenance
must stay within the `gcapture` boundary. Do not add dependencies between GVFG
code and DirectShow internals.
