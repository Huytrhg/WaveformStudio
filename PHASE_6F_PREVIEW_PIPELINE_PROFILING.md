# WaveformStudio — Phase 6F Preview Pipeline Profiling

Branch: `feature/b4.3.3-gaussian-glow-filled`

## Scope

Phase 6F.1 adds CPU-side instrumentation to the active `SpectrumDisplay` preview pipeline. It is a measurement-only change: it does not alter visualizer geometry, D3D11 resources, swap-chain behavior, asynchronous readback, export, or frame pacing.

The existing Phase 6E.1 native presentation and Phase 6E.2 HUD/lifecycle code remain unchanged.

## New CPU measurements

The Preview panel's **Renderer Diagnostics** dialog now includes a Phase 6F section:

- **Paint FPS** — existing measured QWidget paint rate.
- **Paint last / average / max** — wall-clock duration of one complete `paintEvent`, including QPainter work and CPU-side D3D11 submission/Present calls. It is not GPU execution time.
- **Video frames received / accepted / throttled** — valid `QVideoFrame` callbacks, frames converted for preview, and callbacks skipped by the existing 33 ms frame gate.
- **QVideoFrame to QImage** — CPU time spent in `QVideoFrame::toImage()`.
- **Video downscale** — CPU time spent resizing preview frames to the existing 960x540 maximum.
- **Visualizer scene preparation** — CPU time spent constructing Bars / Dancing Particles geometry, colors, cached background work, and per-frame opacity adjustments before the D3D11 renderer call.
- **Cached video background rebuild** — CPU time spent rebuilding the cached ARGB preview background, plus the rebuild count.
- **Qt SpectrumRenderer paint** — CPU time for the QPainter renderer in Qt-rendered modes and in the Bars / Dancing Particles CPU fallback.

The D3D11 diagnostics below these measurements continue to report the existing GPU upload, draw submission, native Present, and async readback metrics.

## Interpretation

- High **QVideoFrame to QImage** time points toward the Qt video-frame conversion path.
- High **Visualizer scene preparation** points toward CPU geometry/color preparation before GPU submission.
- High **Qt SpectrumRenderer paint** points toward the software visualizer path.
- High **Paint** duration with low measured GPU draw time suggests CPU work, driver submission, or presentation overhead; it does not by itself identify which one.
- High **video downscale** time suggests preview image scaling is a candidate for a later targeted optimization.
- Native Bars / Dancing Particles skip staging readback on successful Phase 6E.1 frames. Qt fallback modes and CPU fallback remain useful comparison cases.

Video-frame receive/conversion/downscale counters are cumulative for the lifetime of the preview widget because they measure the shared video input path. Render-side paint, scene-preparation, background-rebuild, and Qt-renderer statistics reset when the visualizer mode or CPU/GPU renderer path changes. After switching a mode/path, let playback run for 20–30 seconds before recording the new sample. Compare the same video, preview dimensions, visualizer settings, and playback segment when collecting mode-to-mode measurements.

## Qt Creator validation

1. Pull the latest branch and open the existing Qt 6.12 MSVC 2022 kit.
2. Build with **Ctrl+B**.
3. Import an MP4 and open **Preview → Renderer Diagnostics** (the Diagnostics button beside the renderer selector).
4. Play for at least 20–30 seconds before recording a sample, so the cumulative average has enough frames.
5. Record the Phase 6F CPU section and the D3D11 runtime section for:
   - Bars (native presentation expected when available).
   - Dancing Particles (native presentation expected when available).
   - Waveform.
   - Circular.
   - Filled.
6. Repeat Bars and Dancing Particles with **CPU fallback** selected in the renderer selector.
7. Keep the same video, preview size, resolution, and visualizer settings for comparisons.
8. Confirm the visual preview, HUD, resize, minimize/restore, and export behavior remain unchanged.

## Current status

- Phase 6F.1 CPU instrumentation has been committed.
- This environment has not compiled the project with the user's Windows / Qt 6.12 MSVC toolchain. A successful Qt Creator build and the runtime comparison above are still required.
- No performance optimization is claimed in this profiling-only step. Phase 6F optimization should be selected only after reviewing the user's measured results.
