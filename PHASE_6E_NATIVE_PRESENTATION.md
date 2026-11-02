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

The Qt HUD and other QWidget-painted decorations are behind the dedicated native child surface while native presentation is visible. Overlay/HUD lifecycle and composition are a Phase 6E.2 task.

### Test plan in Qt Creator

1. Build with **Ctrl+B** using the existing Qt 6.12 MSVC 2022 kit.
2. Run WaveformStudio and import an MP4.
3. Select Bars or Dancing Particles and confirm the preview remains visible and animated.
4. Open Help → GPU Renderer Diagnostics and inspect Runtime rendering statistics:
   - `Phase 6E.1 native presentation: ACTIVE`
   - `frames` should increase while the preview is playing.
   - `Phase 6D Spectrum async readback queued` should stop increasing during native-presented frames (completed/pending counters may retain their earlier values).
5. Resize the preview / main window, minimize and restore, and reopen a video.
6. Switch to Waveform, Circular, or another Qt-rendered mode and confirm the native child surface hides.
7. Force the native presenter to fail only in a controlled development test; verify the Phase 6D asynchronous readback path resumes.

### Build and runtime validation

The source changes are committed, but they have **not** been compiled or run on the user's Windows/Qt 6.12 machine. Treat the first Qt Creator build and preview test as required validation before enabling this path for regular use.
