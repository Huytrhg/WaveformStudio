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

## Phase 6F.2 — avoid redundant background rebuilds

The first user measurements showed:

- Hybrid Bars: 25.2 FPS; scene preparation average 9.235 ms; cached background rebuild average 9.145 ms.
- CPU fallback Bars: 21.5 FPS; Qt SpectrumRenderer paint average 1.722 ms.
- Hybrid Dancing Particles: 25.7 FPS; scene preparation average 9.293 ms; background rebuild average 9.078 ms.
- CPU fallback Dancing Particles: 25.4 FPS; Qt SpectrumRenderer paint average 1.644 ms.
- Native GPU path averaged approximately 0.53–0.54 ms, with no fallback/errors; native Present CPU time was approximately 0.215–0.231 ms.

The close match between scene-preparation and background-rebuild times indicated that rebuilding the full-size cached video background dominated the measured scene-preparation interval. In the existing code, ordinary Spectrum band smoothing and the 16 ms motion timer both marked the background dirty, even when no audio-reactive background effect was enabled. That caused redundant full-size QImage allocation and QPainter background composition between video frames.

Phase 6F.2 changes invalidation behavior:

- New video frames still invalidate the background cache.
- Size changes still trigger a rebuild through the existing cache-size check.
- Explicit background-effect setting changes still invalidate the cache.
- Spectrum smoothing only invalidates the cache when an audio-reactive background effect is enabled (audio zoom, audio shake, exposure pulse, edge glow, or animated/audio-reactive Bokeh).
- The motion timer follows the same conditional invalidation rule.
- Bokeh's own animation timer continues to invalidate the background while animated Bokeh is active.

This keeps dynamic background effects responsive while allowing static-background Bars / Dancing Particles to reuse the prepared background across Spectrum animation paints. The native presentation, D3D11 renderer, staging rings, export path, and video conversion path are not changed.

## Phase 6F.3 — one-click text diagnostics and cache review

The Preview panel's **Diagnostics** button now opens a selectable, read-only text report instead of a message-only popup. The report contains the complete Phase 6F CPU preview measurements followed by the D3D11 runtime diagnostics. **Copy all diagnostics** copies that same report to the system clipboard; the user can paste it directly into a chat or issue report. The report can also be selected and copied manually. No screenshot or manual transcription is required.

The copy action captures one report snapshot when the dialog opens, so the text shown and copied are identical. This change is limited to diagnostics UI and clipboard handling; it does not change preview rendering, frame pacing, cache invalidation, GPU resources, native presentation, or export.

### Review of the Phase 6F.2 measurements

The follow-up measurements show that the cached background is already being rebuilt close to once per accepted video frame in the tested Bars run:

- Hybrid Bars: 640 background builds / 651 accepted video frames.
- Hybrid Dancing Particles: 631 background builds / 1,301 accepted video frames.
- Scene preparation average fell from approximately 9.2 ms to 4.3–4.5 ms.
- QVideoFrame-to-QImage conversion remains approximately 19.8–20.1 ms.

These results do not justify adding another broad cache throttle: accepted video frames legitimately change the background, and audio-reactive effects can also require fresh background composition. Further reducing rebuilds without separating the static video layer from dynamic effect layers could introduce stale frames or delayed Bokeh / Audio Zoom response. Phase 6F.3 therefore preserves the current invalidation behavior and makes the complete measurements easier to collect. The next performance investigation should focus on the expensive QVideoFrame conversion path, with format/backend-aware measurements before changing its implementation.

### Qt Creator validation

1. Pull the latest branch and build with the existing Qt 6.12 MSVC 2022 kit.
2. Play a video and open **Preview → Diagnostics**.
3. Confirm the full CPU and D3D11 report is visible in a selectable text area.
4. Click **Copy all diagnostics**, then paste into Notepad or directly into the chat.
5. Confirm the pasted report contains both the Phase 6F CPU section and the D3D11 section.
6. Confirm preview playback, Bokeh, Audio Zoom, native presentation, and export behavior remain unchanged.

## Phase 6F.4 — inspect the QVideoFrame mapping path

Phase 6F.4 adds diagnostic-only measurements immediately before the existing `QVideoFrame::toImage()` call. It records:

- Actual Qt pixel-format name, frame width/height, and plane count.
- Per-plane stride and mapped byte count (up to four planes).
- Time spent mapping a read-only copy of the accepted frame.
- Time spent unmapping that probe frame, plus map failures.
- The existing `QVideoFrame::toImage()` last / average / maximum timing remains available as the separate end-to-end conversion measurement.

The probe maps a copy of the `QVideoFrame` handle in read-only mode, reads only layout metadata, then unmaps it. It does not modify frame pixels or replace the existing `toImage()` conversion. The probe timings are diagnostic measurements and must **not** be added to the `toImage()` time as if they were sequential sub-phases: Qt's internal conversion may use its own mapping path, and the probe itself may affect buffer readiness/cache state. Compare the map/unmap figures alongside the conversion average, not as an exact decomposition of it.

### How to collect Phase 6F.4 data

1. Pull the latest branch and build with the existing Qt 6.12 MSVC 2022 kit.
2. Play the same MP4 for 20–30 seconds.
3. Open **Preview → Diagnostics** and click **Copy all diagnostics**.
4. Paste the complete report. Keep the video and preview dimensions unchanged when comparing runs.
5. Include the frame format, dimensions, plane layout, map/unmap timings, and the existing conversion average/max.

This phase does not change Bokeh rendering, D3D11 presentation, frame pacing, image scaling, export, or the selected video conversion method. It is not an export optimization and does not yet attempt direct GPU texture sharing or a format-specific conversion shortcut.

### Decision gate after measurement

- If map time is high, investigate the frame's memory/backend mapping cost before considering a different input path.
- If map is cheap but `toImage()` remains high, inspect Qt's format conversion and color-space path for the reported pixel format.
- If the frame is already in a GPU-backed or hardware surface format, determine whether the current Qt 6.12 Multimedia backend exposes a safe native-handle path before designing a D3D11 import.
- Any later optimization must preserve a tested fallback for unsupported formats and verify color range, matrix, transfer characteristics, orientation, and frame lifetime.

No conversion implementation has been changed in Phase 6F.4. No build or runtime result is claimed in this environment.

## Current status

- Phase 6F.1 CPU instrumentation has been committed.
- Phase 6F.2 conditional background-cache invalidation has been committed.
- Phase 6F.3 one-click copyable CPU + D3D11 diagnostics has been committed.
- This environment has not compiled the project with the user's Windows / Qt 6.12 MSVC toolchain. A successful Qt Creator build and runtime validation are still required.
- Phase 6F.2 is expected to reduce the Cached video background rebuild count from roughly one per paint toward one per accepted video frame when no dynamic background effects are active. Actual FPS and timing improvements must be measured on the user's system; no build or performance result is claimed here.