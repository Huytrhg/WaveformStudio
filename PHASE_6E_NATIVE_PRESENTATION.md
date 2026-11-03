# WaveformStudio — Phase 6E Native Presentation

## Phase 6E.1 implementation status

Implemented on branch `feature/b4.3.3-gaussian-glow-filled`.

### What changed

- `GpuRenderer` now creates the DXGI swap chain from the same D3D11 device that renders the Phase 4A Spectrum target.
- The retained Spectrum render target now has both render-target and shader-resource bindings.
- A small full-screen copy shader samples that render target and draws it into the swap-chain back buffer.
- The swap chain targets a dedicated child HWND owned by `SpectrumDisplay`; it does not present into the QWidget HWND that Qt paints.
- A successful native-present frame bypasses the Phase 6D staging copy, CPU Map, and QImage readback.
- If native swap-chain setup, resize, or Present fails, the current frame falls through to the existing Phase 6D asynchronous readback path.
- Native presentation is limited to the existing GPU-rendered Bars / Dancing Particles path (visual modes 0 and 4). Other modes continue to use Qt painting.
- Runtime statistics report native-present frame count, CPU Present call time, and the latest native-presentation status/error.

### Important limitations

This is **not end-to-end zero-copy**. Qt Multimedia still converts decoded video frames to `QImage`; the preview still uploads the CPU-prepared background into the D3D11 render target. Phase 6E.1 removes the final D3D11 staging readback for successfully native-presented frames only.

The preview HUD is now implemented as a small native child window parented directly to the DXGI preview HWND. It stays above the swap-chain surface, occupies only the top-left HUD area, and returns HTTRANSPARENT so it does not consume mouse input. When native presentation falls back, the native HUD is hidden and the existing Qt-painted HUD remains available. Phase 6E.2 also hides/disables native presentation during widget hide/minimize and reactivates it after restore; it repositions child surfaces on resize and recreates them after a QWidget native-handle change.

## Phase 6E.2 — Native HUD and lifecycle

Implemented on the same feature branch.

- Added a dedicated Win32 HUD child window above the DXGI preview child window. The HUD draws the current renderer status and preview FPS using GDI; no additional graphics library or D3D rendering pass is required.
- The HUD is sized to the label rather than covering the full preview. `WM_NCHITTEST` returns `HTTRANSPARENT`, and `WM_MOUSEACTIVATE` returns `MA_NOACTIVATE`.
- The Qt HUD remains the fallback when native presentation is inactive or the selected visualizer uses Qt painting.
- Added hide/minimize handling to hide the native surfaces and release the swap chain; restore causes the preview to be initialized again on the next paint.
- Resize updates the preview child HWND and HUD geometry. `QEvent::WinIdChange` destroys stale child handles and recreates the preview child.
- The existing D3D11 render/composite path, supported modes (Bars and Dancing Particles), Bokeh composition, and Phase 6D fallback are unchanged.

### Test plan in Qt Creator

1. Build with **Ctrl+B** using the existing Qt 6.12 MSVC 2022 kit.
2. Run WaveformStudio and import an MP4.
3. Select Bars or Dancing Particles and confirm the preview remains visible and animated.
4. Open Help → GPU Renderer Diagnostics and inspect Runtime rendering statistics:
   - `Phase 6E.1 native presentation: ACTIVE`
   - `frames` should increase while the preview is playing.
   - `Phase 6D Spectrum async readback queued` should stop increasing during native-presented frames (completed/pending counters may retain their earlier values).
5. Confirm the native HUD is visible at the top-left of the preview, updates its frame count/FPS, and does not block mouse input.
6. Resize the preview / main window and confirm both the native preview and HUD remain correctly aligned.
7. Minimize and restore the main window; confirm the native preview and HUD return without flashing, stale surfaces, or errors.
8. Switch to Waveform, Circular, or another Qt-rendered mode and confirm the native preview/HUD hide and the Qt HUD remains visible.
9. Reopen a video and repeat the mode/resize checks.
10. Force the native presenter to fail only in a controlled development test; verify the Phase 6D asynchronous readback path resumes.

### Build and runtime validation

The Phase 6E.1 and 6E.2 source changes are committed, but they have **not** been compiled or run on the user's Windows/Qt 6.12 machine. Treat the first Qt Creator build and the test plan above as required validation before enabling this path for regular use.
