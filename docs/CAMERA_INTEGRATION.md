# Camera integration strategy

NoiseBot uses an ESP32-S3 with audio, display, SD, WiFi/bridge and an OV2640 DVP
camera sharing the same constrained internal-memory and DMA domain. The camera
must therefore be integrated as a budgeted subsystem, not as a default boot
device.

During bring-up, the installed module is treated as the board's OV2640 DVP
camera. The diagnostic backend uses `esp_video`/V4L2 directly, so camera
ownership is explicit: open the video device, mmap driver buffers, dequeue one
frame, return it, and keep or close the session deliberately.

## Current policy

- The normal firmware build keeps the camera disabled unless the diagnostic
  camera flag is enabled for bring-up.
- `CONFIG_NB_CAMERA_DIAG_ENABLED` enables the OV2640 diagnostic path.
- The camera is never initialized during boot.
- Snapshot capture is lazy and rejected while audio is busy: active listening,
  local playback or bridge SAY playback.
- Once opened, the camera stays in a short "hot session" instead of being
  initialized/deinitialized for every external dashboard/API click. This avoids
  repeated DMA heap fragmentation.
- The external dashboard explicitly closes the hot session when leaving the system view;
  otherwise the service closes it after a timeout.
- A connected but idle bridge does not block camera diagnostics.
- The HAL leaves camera buffer allocation to the driver instead of rejecting
  capture based on precomputed DMA thresholds. After the driver opens, the
  service reports the measured internal, DMA and PSRAM headroom.
- `/api/camera/status` reports `supported=false` when the diagnostic camera build is
  not enabled.
- `/api/health` and `/api/camera/status` expose internal, DMA and PSRAM heap
  figures so camera bring-up can be correlated with WiFi, SD and bridge health.

## Memory policy adopted for product runtime

- WiFi uses a reduced local-control profile instead of default throughput
  buffers.
- LwIP is sized for the local REST API and bridge TCP, not many parallel
  internet sockets.
- Large non-DMA queues are allocated in PSRAM when available.
- Non-critical service tasks use PSRAM stacks when
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` is enabled.
- Flash/OTA paths keep internal memory because flash writes can temporarily
  disable external memory/cache access.
- Audio I2S DMA and hot real-time buffers remain internal by design.

This keeps the robot responsive while the bridge, WiFi, SD logging, audio and UI
are active.

## Why the first camera attempt destabilized the robot

The failure pattern was not a single camera bug. The logs showed global resource
pressure:

- `sdmmc_read_sectors: not enough mem`
- `diskio_sdmmc: sdmmc_read_blocks failed`
- `wifi:mem fail`
- bridge disconnects and slow services

On the ESP32-S3, PSRAM is large but not equivalent to internal SRAM. WiFi,
SD/SPI, I2S DMA, HTTP and camera DMA still need internal-capable memory. A DVP
camera driver brought into the normal runtime can reserve enough internal memory
to make unrelated services fail.

## Camera Runtime Model

The camera path is deliberately session-based instead of boot-owned:

- use reduced WiFi buffer settings appropriate for local control;
- keep large buffers in PSRAM where possible;
- leave camera frame allocation to the ESP video/V4L2 driver;
- open the video device only for explicit diagnostics or vision observations;
- reuse the open session for repeated captures until timeout or explicit close;
- share board resources deliberately, especially SCCB/I2C ownership;
- isolate camera work from conversational/audio paths.

NoiseBot has its own board, OV2640 DVP pinout and C17 component rules. Camera
code must therefore stay inside the NoiseBot HAL/service boundaries instead of
copying board-specific implementations from other products.

## Bring-up plan

1. Keep product builds on `CONFIG_NB_CAMERA_DIAG_ENABLED=n`.
2. Use the `espressif/esp_video` managed component only in the diagnostic
   camera build used for bring-up.
3. Build a diagnostic firmware with `CONFIG_NB_CAMERA_DIAG_ENABLED=y`.
4. Boot with bridge idle and SD logging quiet.
5. Capture one 640x480 snapshot through `/api/camera/snapshot` or
   `/api/vision/observe`.
6. Log before/after values for:
   - `MALLOC_CAP_DMA`
   - `MALLOC_CAP_INTERNAL`
   - `MALLOC_CAP_SPIRAM`
   - largest free DMA block
7. Repeat with WiFi and bridge connected but idle.
8. Confirm snapshot is rejected during active listening or bridge SAY playback.
9. Keep the camera session open across repeated captures and close it only by
   timeout or `/api/camera/session/close`.
10. Keep the backend in C and inside NoiseBot's component rules.

## Production acceptance criteria

- Bridge stays connected for at least 30 minutes with camera support compiled in.
- No `wifi:mem fail` during idle, listening or external dashboard polling.
- No SD degraded-mode flapping while camera is idle.
- Snapshot capture is serialized and cannot run during active conversation.
- PSRAM free remains above 300 KB after capture and release.
- Internal DMA headroom remains above the HAL threshold after camera deinit.
- The robot remains fully usable with no camera present or with camera disabled.
- NoiseBot does not reject camera open based on precomputed DMA/internal
  thresholds. Allocation success is left to the camera driver. Once the session
  is open, subsequent captures reuse the existing driver allocation instead of
  re-testing and reallocating the camera.

## Current diagnostic finding

- The OV2640 is detected reliably through SCCB (`PID=0x26`).
- The active backend is `esp_video`/V4L2, not `esp32-camera`.
- The stable product-diagnostic path now captures 640x480 frames through a
  session model: the first open pays the allocation cost, then repeated captures
  reuse the same video session until timeout or explicit close.
- `/api/camera/status` exposes session/memory/error counters for the external dashboard.
- `/api/vision/observe` exposes the first behavior-facing observation:
  resolution, JPEG size, capture time, luma average/min/max, contrast and a
  simple motion score.
- `/api/diag/snapshot` includes the latest vision observation without taking a
  new picture during the diagnostic dump. Hardware validation on 2026-06-04
  returned parseable JSON with `vision.available=true`, `valid=true`, 640x480
  observation metrics and an SD diagnostic dump.
- `noisebot_server debug vision-soak` runs the repeated observation soak and
  records failures, reboots, FPS, heap and capture latency. Hardware validation
  on 2026-06-04 ran for 1801.5s with 59/59 valid observations, zero failures,
  zero reboots, final camera closed and Voice Audio v2 release-check still green.
- Presence detection for Roadmap 13.1 is now present as on-demand shadow state
  in `vision_service`: `/api/vision/observe`, `/api/vision/status` and
  `/api/diag/snapshot` include a `presence` block, while
  `NB_EVT_PRESENCE_DETECTED`/`NB_EVT_PRESENCE_LOST` are emitted only on debounced
  transitions. Continuous capture and behavior/attention integration remain
  gated on false-positive measurements.
- `noisebot_server debug vision-soak --expect-absence --min-fps 25` validates
  that shadow-mode presence stays absent while preserving render FPS. A 31.5s
  hardware smoke on 2026-06-04 produced 6/6 valid observations, zero presence
  false positives, `min_fps=25.2`, max presence score 54 and final camera closed.
- `noisebot_server debug vision-presence-trial` is the focused Roadmap 13.1
  harness for `absence`, `presence` and `lost` trials. A 2026-06-04 absence
  smoke produced 5/5 valid observations, zero false positives, max presence
  score 42 and final camera closed; the same mode with `--min-fps 25` failed at
  `min_fps=22.9`, so continuous/polling vision still needs FPS tuning before
  the acceptance criterion can close.
- The presence trial reports `baseline_fps` before the first camera capture.
  A 2026-06-04 run with `--close-each-sample --fps-sample-delay-s 2 --min-fps 25`
  showed `baseline_fps=22.9` and `min_fps=22.9`, confirming the current FPS gate
  is blocked before attributing additional loss to vision polling.
- The bridge v2 can answer local vision questions from that endpoint without
  invoking the LLM.
- Hardware validation on 2026-05-25 showed camera snapshots, bridge connection
  and TTS playback working together in the same firmware build.

## Open camera/vision work

- Promote the basic observation into presence detection (`PRESENCE_DETECTED` /
  `PRESENCE_LOST`) continuous mode only after shadow/lighting false positives
  are measured.
- Keep MJPEG streaming out of the main product loop until snapshot mode has a
  long-run memory profile.
