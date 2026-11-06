# WaveformStudio — Phase 6F Optimization Pause & Resume Notes

**Status:** Optimization work is intentionally paused. Continue feature development from the stable Phase 6F.11 baseline. Revisit performance only after the application has been used in real operating conditions and representative measurements are available.

## 1. Current decision

- Keep **Phase 6F.11** as the accepted, user-verified Preview performance baseline.
- Stop the Phase 6F optimization sequence for now. Do not proceed with Phase 6F.12/6F.13 as planned experiments.
- Continue implementing the remaining product features.
- Avoid unrelated changes to the Preview frame pipeline while implementing features. If a feature genuinely requires such a change, isolate it and compare against the Phase 6F.11 baseline.
- Reassess performance after real-world operation, using repeatable measurements and the user's actual workloads.

The user explicitly tested the rollback and confirmed: **“OK, xác nhận đã ổn”**.

## 2. Stable source checkpoint

Repository: `Huytrhg/WaveformStudio`  
Branch: `feature/b4.3.3-gaussian-glow-filled`

The rollback restored these files to their Phase 6F.11 versions:

| File | Phase 6F.11 content SHA |
|---|---|
| `mainwindow.cpp` | `5e6350ff3e85c7449ff5f31a2c90bb536deea2e9` |
| `PHASE_6F_PREVIEW_PIPELINE_PROFILING.md` | `cc0fc0ac147c314e982d72a3d0dea3603682a6e1` |

Branch links:
- [mainwindow.cpp](https://github.com/Huytrhg/WaveformStudio/blob/feature/b4.3.3-gaussian-glow-filled/mainwindow.cpp)
- [Phase 6F profiling notes](https://github.com/Huytrhg/WaveformStudio/blob/feature/b4.3.3-gaussian-glow-filled/PHASE_6F_PREVIEW_PIPELINE_PROFILING.md)

These are content/blob SHAs for the restored files, not a claim that a new build was performed.

## 3. What Phase 6F.11 contains

Phase 6F.11 adds GUI-thread workload and update-to-paint latency profiling, including:

- Accepted and skipped callback processing durations (last/average/maximum).
- `QVideoFrame::toImage()` failure counts.
- Update-to-paint pending latency (oldest/newest).
- Paint-to-paint interval.
- Existing callback, PTS, and Preview presentation measurements.

The established preview pacing uses source PTS at approximately **33,300 microseconds (~30 FPS)**, with a wall-clock fallback around 33 ms when PTS is unavailable. The baseline does not introduce a worker thread or an additional frame copy for this pacing logic. Export behavior remains separate and unchanged by this Preview pacing work.

Use the existing profiling document for the full Phase 6F.1–6F.11 history and metric definitions.

## 4. Why Phase 6F.12 was rolled back

Phase 6F.12 introduced an opt-in direct NV12-to-preview RGB32 conversion path controlled by:

```text
WAVEFORM_PREVIEW_NV12_DIRECT=1
```

The experiment mapped NV12 frames and converted them to a 960×540 RGB32 preview image in CPU code, with a fallback to `frame.toImage()`.

The A/B run was not accepted:

- With the direct path disabled (A), Preview was still visibly choppy and slower than the user's previously observed behavior. The run still contained the Phase 6F.12 code/instrumentation, so it was not a clean Phase 6F.11 comparison.
- With direct NV12 conversion enabled (B), conversion averaged about **39.293 ms**; Preview paint rate fell to about **0.26 FPS** during that run, and callback/paint coalescing metrics showed severe backlog.
- The user reported the experience as the worst Preview performance since Phase 6F.12 began.

The user approved rollback. The Phase 6F.12 source and its profiling-document changes were removed from the active branch by restoring the Phase 6F.11 versions. The user then confirmed that Preview was stable/smooth again.

**Do not reintroduce the Phase 6F.12 direct NV12 implementation or its environment-variable switch as a default or assumed optimization.** Any future experiment must first state a concrete hypothesis, be isolated behind an opt-in switch, and be compared with the clean Phase 6F.11 baseline.

## 5. Architecture constraints to remember

The current rendering path is hybrid; do not describe it as fully zero-copy.

- The D3D11 path uploads CPU-prepared video content to a cached D3D11 target.
- Spectrum compositing and supported visualizer rendering can run on the GPU.
- The async staging-readback ring avoids blocking the GUI thread for supported Qt-fallback paths.
- Native presentation uses a same-device DXGI swap chain and a dedicated child HWND for supported modes, bypassing GPU-to-CPU staging readback for successfully native-presented frames. It does not eliminate CPU-side video conversion/preparation and upload.
- Native presentation is limited to supported visualizer modes; other modes use the Qt fallback.
- Export is a separate path and should not be changed as a side effect of Preview optimization.

Historical profiling identified Copy/Map wait as a significant cost before asynchronous readback was introduced. Treat that as historical evidence, not proof of the current bottleneck; measure again before choosing a target.

## 6. Resume checklist — only after real-world evaluation

When the user decides to revisit performance:

1. **Start from the then-current stable branch.** Fetch the latest branch files and commit history first; do not blindly overwrite later feature work with the old Phase 6F.11 snapshots.
2. Confirm Preview is stable before measuring. Record application build/configuration, GPU, video resolution/frame rate/codec, visualizer mode, overlays/effects, and test duration.
3. Capture a clean baseline with existing diagnostics. Avoid adding intrusive per-frame mapping, readback, logging, or extra copies just to measure.
4. Identify the dominant current bottleneck from measurements. Separate callback arrival, frame conversion/downscale, scene preparation, Qt paint, GPU submission, readback, and presentation.
5. Change **one variable at a time**. Keep experiments isolated and reversible; do not combine conversion, pacing, threading, and presentation changes in one phase.
6. Compare against the same workload and duration. Record both numerical metrics and the user's visual assessment of smoothness, latency, color quality, resize/minimize/restore behavior, and overlay correctness.
7. Verify that export output and non-target visualizer modes remain unchanged.
8. Keep a change only if it produces a repeatable improvement without regressions. Otherwise roll it back promptly.
9. Ask the user to validate on their Windows/Qt machine. Do not claim a successful local build or runtime validation unless it was actually performed.

## 7. Resume trigger

Resume optimization only when the user explicitly asks to do so after operational evaluation, or when a concrete, reproducible performance problem blocks a feature or normal use.

Until then, prioritize remaining features and preserve the stable Preview pipeline.
