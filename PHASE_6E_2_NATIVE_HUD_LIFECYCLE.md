# WaveformStudio — Phase 6E.2 Native HUD & Lifecycle

Branch: `feature/b4.3.3-gaussian-glow-filled`

## Scope

Phase 6E.2 builds on the tested Phase 6E.1 DXGI presentation path. It does not change the D3D11 Spectrum composite, the native swap-chain copy shader, Bokeh rendering, or the Phase 6D fallback.

## Native HUD overlay

- A small Win32 child HWND is created as a child of the DXGI preview HWND, guaranteeing it is above the swap-chain surface.
- The HUD displays the same first-line renderer statistics and preview FPS as the previous Qt HUD.
- It draws only its own compact top-left rectangle; it does not cover the rest of the video.
- `WM_NCHITTEST` returns `HTTRANSPARENT`; `WM_MOUSEACTIVATE` returns `MA_NOACTIVATE`.
- The native HUD is shown only after a successful native-presented frame. On Qt fallback, it is hidden and the existing QPainter HUD is used.

## Lifecycle handling

- QWidget hide: hide HUD/preview children and disable the native swap chain.
- Main-window minimize: hide native surfaces and disable native presentation.
- Restore/show: ensure the preview child exists; normal paint/configure flow recreates or reactivates the swap chain.
- Resize: resize the preview child and reposition/resize the HUD.
- QWidget native handle change: destroy stale child HWNDs, disable the old swap chain, and recreate the preview child when visible.
- Destruction: destroy the HUD before its parent preview HWND.

## Validation checklist

1. Build in Qt Creator with the Qt 6.12 MSVC 2022 kit.
2. Play an MP4 using Bars; confirm native presentation is ACTIVE and the HUD is visible above the video.
3. Confirm HUD frame count/FPS changes during playback.
4. Confirm clicking/dragging in the preview does not get intercepted by the HUD.
5. Enable Bokeh Overlay; verify Bokeh remains visible beneath the HUD and correctly composited.
6. Resize the preview and main window repeatedly; confirm HUD alignment.
7. Minimize and restore the main window; confirm preview/HUD return and native frame count resumes.
8. Repeat with Dancing Particles.
9. Switch to Waveform, Circular, and other Qt modes; confirm native HUD/preview hide and Qt HUD remains visible.
10. Reopen another video and repeat.

## Validation status

Code committed. A Windows/Qt 6.12 build and runtime validation are still required; no claim of a successful local build is made here.
