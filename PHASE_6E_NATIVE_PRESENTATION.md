# WaveformStudio — Phase 6E Native Presentation Assessment

## Status

Phase 6E is an architecture and integration checkpoint. The existing Phase 6C/6D asynchronous readback path remains the production fallback and is not changed by this checkpoint.

## Findings from the current branch

1. `SpectrumDisplay` receives decoded video through `QVideoSink::videoFrameChanged`, calls `QVideoFrame::toImage()`, optionally scales the result to 960x540, and stores it as `videoImage`. This is a CPU-visible frame path.
2. `paintVideoBackground()` prepares a CPU `QImage`; the Phase 4A renderer uploads that image with `ID3D11DeviceContext::UpdateSubresource()` and composites Spectrum bars into `GpuRenderer::cachedTexture`.
3. Phase 6D copies `cachedTexture` to staging textures and asynchronously maps them to CPU images. This is the readback that a native presentation path can bypass.
4. `NativeGpuPresenter` already exists in this branch, but it creates its own D3D11 device and swap chain, then uploads a CPU `QImage` on every call. It is not connected to the active paint path (the call is intentionally disabled after a prior native-window flicker issue). It therefore does not provide shared-device presentation or zero-copy.
5. The active renderer and the existing native presenter own different D3D11 devices. A texture created by one device cannot simply be bound as a shader resource by the other device without an explicit sharing design and synchronization.

## Safe Phase 6E implementation sequence

### 6E.1 — Shared-device swap-chain presenter

- Keep the D3D11 device and immediate context owned by `GpuRenderer`.
- Create a dedicated native child preview surface with its own HWND; do not bind a swap chain to the Qt widget that is also painted by `QPainter`.
- Create the DXGI swap chain from the adapter/factory associated with the existing `GpuRenderer` device.
- Make the Phase 4A render target shader-readable and draw that texture into the child surface's back buffer, then call `Present()`.
- Enable this path only after the child surface is visible and a successful present is confirmed.
- When native presentation succeeds, do not enqueue Phase 6D staging readback for that frame. Keep Phase 6D readback as an automatic fallback if initialization, resize, or Present fails.

### 6E.2 — Integrate overlays and lifecycle

- Move the preview HUD to a Qt overlay sibling above the native child surface.
- Ensure resize, hide/show, device-loss, and window-destruction paths release/recreate swap-chain resources safely.
- Keep Bokeh and non-bar Spectrum modes on the existing Qt fallback until their drawing is composited into the same GPU target.
- Preserve the current asynchronous Bokeh readback until Bokeh itself is part of the shared final render target.

### 6E.3 — Decoder-to-GPU interop (separate milestone)

Even after 6E.1, the current `QVideoFrame::toImage()` and `UpdateSubresource()` stages remain CPU conversion/upload. True decoder-to-display zero-copy requires obtaining a D3D11 texture from Qt Multimedia's Windows backend and proving that it belongs to, or can be safely shared with, the renderer device. That work must be validated against the exact Qt 6.12 Multimedia backend and Intel UHD 730 driver; it must not be inferred from the existence of `QVideoSink` alone.

## Acceptance tests

1. Existing Phase 6C/6D preview remains unchanged when native presentation is disabled.
2. Native child HWND and swap chain initialize on the same D3D11 device as `GpuRenderer`.
3. A test frame is presented without staging `CopyResource`, CPU `Map`, or `QImage` readback on the native-presented path.
4. Resize/minimize/restore and repeated video open/close do not flash, freeze, or leave a stale child surface.
5. Forced native-present failure automatically returns to the Phase 6D asynchronous readback path.
6. Runtime statistics distinguish native-present frames from asynchronous readback frames; they must not label the current CPU video upload as zero-copy.

## Current decision

Do not enable the existing `NativeGpuPresenter` as-is. It uses a second D3D11 device, accepts CPU images, and was previously disabled because presenting into a Qt-painted HWND caused flicker. The next code change should implement 6E.1 inside the existing `GpuRenderer` device and use a dedicated native child surface. This assessment intentionally makes no changes to the stable Phase 6C/6D rendering path.
