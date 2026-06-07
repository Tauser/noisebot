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
- After lowering the invisible camera path to the OV2640 `240x240` YUV422 base
  mode, a second 2026-06-04 hardware soak ran for 1800.8s with 61/61 valid
  observations, zero failures, zero reboots, `max_capture_ms=174`,
  `max_jpeg_bytes=0`, `min_psram_free=7132240`, `min_dma_free=16643` and final
  camera closed. It also measured `min_fps=24.0`, so long-run capture stability
  is validated while the render FPS acceptance gate remains open.
- The expression renderer now uses dynamic dirty rectangles for the normal face
  and light idle rotation, while keeping the full conservative face rect for
  sleep, wake, speaking and out-of-eye effects. Hardware validation after flash
  measured steady-state render at `fps=52.4`, `dirty=220x112` and
  `push=18.8ms`; a 120s `vision-soak --min-fps 30` improved the valley but still
  failed at `min_fps=27.3`, so the long-run FPS gate remains open.
- `idle_service` now enters `CAMERA_STEADY` while the camera hot session is
  active, avoiding `POSE_TILT` and wide gaze motifs that produced
  near-full-screen dirty rectangles during invisible captures. The
  `vision-soak` harness can now start an `audio_io_v2` RX probe before each
  observation via `--audio-io-probe-ms`, and reports probe failures, busy skips,
  I2S recoveries/drops and the worst FPS sample. Hardware validation after flash
  passed a 120s
  `vision-soak --duration-s 120 --interval-s 10 --min-fps 30 --audio-io-probe-ms 5000`
  with 13/13 valid observations, zero failures/reboots, `min_fps=34.9`,
  `max_capture_ms=175`, `min_dma_free=15795`, zero I2S recoveries/drops and
  final camera closed. The follow-up 30-minute soak,
  `vision-soak --duration-s 1800 --interval-s 30 --min-fps 30 --audio-io-probe-ms 5000`,
  passed with 60/60 valid observations, zero failures/reboots, `min_fps=30.5`,
  worst dirty rect `253x186`, `max_capture_ms=175`, `max_jpeg_bytes=0`,
  `min_psram_free=6965800`, `min_dma_free=15723`, zero audio probe failures,
  zero I2S recoveries/drops and final camera closed.
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
- `/api/render/status` now exposes live render metrics for camera/presence
  trials: FPS, clear/layer/push time, dirty rect size and full/partial/skipped
  push counters. Hardware evidence on 2026-06-04 showed the old 85% full-push
  threshold forcing full 320x240 pushes at `fps=22.9` and `push=42.4ms`; raising
  it to 95% keeps the face path on partial 305x228 pushes at `baseline_fps=27.1`.
- With the render metric endpoint and the 95% threshold, a 2026-06-04 absence
  trial using `--close-each-sample --fps-sample-delay-s 4 --min-fps 25` passed:
  3/3 valid observations, zero false positives, max presence score 42,
  `min_fps=26.7`, final presence state `absent` and final camera closed.
- The presence trial now reports observed `present`/`lost` transitions plus the
  firmware `transition_count` initial/final/delta, and accepts
  `--require-initial-state`/`--require-final-state`. `mode=lost` requires an
  observed `present -> absent` transition instead of accepting a run that was
  already absent at the start.
- The firmware presence JSON also includes `detected_event_count`,
  `lost_event_count` and `last_event_ms`. Presence/lost trials now use those
  deltas to prove that `PRESENCE_DETECTED`/`PRESENCE_LOST` were published, not
  just that the internal shadow state changed.
- The event-counter firmware was built and flashed on 2026-06-04. Post-flash
  sanity returned a valid 640x480 `/api/vision/observe` payload with
  `detected_event_count=0`, `lost_event_count=0`, `last_event_ms=0`, while
  `/api/render/status` stayed at `fps=27.0`. An absence smoke with the new
  fields passed before the final buffer-size adjustment: 3/3 valid observations,
  zero false positives, `baseline_fps=25.4` and `min_fps=26.6`.
- The presence trial now reports `first_candidate_elapsed_ms` and score
  min/average/p95/max. A 2026-06-04 uncontrolled `presence` run failed with
  8/8 valid observations, one `candidate`, no `present`, score
  `40/44.25/66/66`, no `PRESENCE_DETECTED`, `baseline_fps=26.4` and
  `min_fps=24.5`; a controlled person-entry run is still required.
- `vision-presence-trial --start-delay-s N` arms a controlled entry run and
  starts latency measurement after the delay. With `--require-initial-state` the
  harness now takes a pre-delay observation, and `--arm-timeout-s N` can wait
  until that initial state is actually observed before starting the entry delay.
  The report also includes capture-time and `spatial_score` min/average/p95/max
  to separate capture latency from score/debounce limits.
- StackChan comparison on 2026-06-04: its local camera path uses
  V4L2/`esp_video` with streaming buffers kept active; `Capture()` discards early
  buffers and `StreamCaptures()` reads the current frame. It does not provide an
  equivalent embedded presence detector, but it does point NoiseBot toward a
  warm continuous vision loop instead of isolated snapshots for sub-500ms
  presence latency.
- StackChan/Xiaozhi UI reference on 2026-06-04: the app/server protocol uses
  explicit `OnCamera`/`OffCamera` messages based on camera subscribers, and the
  firmware UI keeps small status indicators as icons. NoiseBot mirrors that
  product behavior with a camera badge in `ui_overlay_service`: the badge is on
  while the camera hot session is active and is cleared when the session closes.
- Resolution and JPEG quality model (corrected 2026-06-07): the OV2640 DVP
  driver only ever delivers its native `240x240` frame — `VIDIOC_S_FMT`/
  `VIDIOC_TRY_FMT` with a forced QQVGA/QVGA/VGA size is rejected on this path
  with `errno=22` (`EINVAL`), proven empirically and traced to its root cause
  in the vendored driver source (see "Why resolution is fixed at 240x240"
  below) — not assumed from a comment. The HAL keeps the driver's reported
  width/height and only negotiates the pixel format.
  `camera_hal_mode_width()`/`camera_hal_mode_height()` therefore always report
  `240`, and `/api/camera/status` additionally exposes `effective_width`/
  `effective_height` (read back from `VIDIOC_G_FMT` after init) so the
  dashboard never has to guess. `safe`/`better` camera modes still exist (they
  drive memory-threshold checks before HAL init) but **no longer claim to
  change resolution or JPEG quality** — the previous "safe=QQVGA 160x120,
  better=QVGA 320x240" framing was aspirational and never matched what the
  driver could deliver. JPEG quality is now a short-capture vision intent:
  `camera_service_capture_snapshot()` uses
  `CAMERA_SVC_SNAPSHOT_JPEG_QUALITY=82` for snapshot/analyze/face-detect
  workflows. The robot no longer exposes live MJPEG monitoring; camera time is
  reserved for internal perception and explicit analysis calls.
- Why resolution is fixed at 240x240 — full evidence chain (investigated
  2026-06-07, replacing the earlier "errno=22" comment with a proven root
  cause traced through the vendored driver source rather than an assumption):
  - The OV2640 sensor driver genuinely supports far more than 240x240. Its
    compiled-in mode table `ov2640_format_info[]`
    (`managed_components/espressif__esp_cam_sensor/sensors/ov2640/ov2640.c:56`)
    has 12 entries with full SCCB register sequences, including
    `RGB565/YUV422/JPEG 640x480`, `JPEG 320x240`, `JPEG 1280x720`,
    `JPEG 1600x1200` and several `RAW8` modes up to `1024x600`; the project's
    `Kconfig.ov2640` (`managed_components/espressif__esp_cam_sensor/sensors/ov2640/Kconfig.ov2640:24`)
    exposes all of them as a `choice`. `ov2640_query_support_capability()`
    reports `fmt_yuv=fmt_rgb565=fmt_jpeg=1`.
  - Which entry is active is decided exactly **once**, at build/init time, not
    negotiable at runtime through the standard V4L2 surface:
    `dvp_video_init()` (`esp_video_dvp_device.c:196`) calls
    `esp_cam_sensor_set_format(sensor, NULL)`; inside `ov2640_set_format()`
    (`ov2640.c:696-709`) a `NULL` format falls back to
    `&ov2640_format_info[CONFIG_CAMERA_OV2640_DVP_IF_FORMAT_INDEX_DEFAULT]` — a
    **compile-time Kconfig constant**. `sdkconfig.defaults` selects index 4
    (`CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS=y`).
  - `VIDIOC_S_FMT`/`VIDIOC_TRY_FMT` on the DVP video device
    (`dvp_video_set_format()`, `esp_video_dvp_device.c:365-377`) is a **hard
    equality check**: any `width`/`height`/`pixelformat` that does not exactly
    match the format the sensor was already initialized with returns
    `ESP_ERR_INVALID_ARG` (`errno=22`/`EINVAL`) immediately — no negotiation,
    no fallback, no attempt to reconfigure the sensor or reallocate buffers.
    `camera_hal_init()` now runs a real `VIDIOC_TRY_FMT` sweep at every boot
    (`camera_hal_probe_try_fmt()`) over `160x120`/`320x240`/`640x480` plus the
    native size, logging accept/reject + `errno` for each — turning this from
    an asserted fact into a per-boot, hardware-observable proof. (`TRY_FMT` is
    non-committing — it never changes driver state — so the sweep is safe to
    run unconditionally before the real `VIDIOC_S_FMT`.)
  - `VIDIOC_ENUM_FMT` on this device always reports exactly one pixel format
    (`dvp_video_enum_format()` rejects any `index >= 1`,
    `esp_video_dvp_device.c:354-363`), and `enum_framesizes`/
    `enum_frameintervals` are `NULL` in `s_dvp_video_ops`
    (`esp_video_dvp_device.c:463-478`) — so `VIDIOC_ENUM_FRAMESIZES` always
    returns `ESP_ERR_NOT_SUPPORTED` on the DVP backend. This is *why*
    `camera_hal_log_framesizes()` never finds anything: the capability is
    architecturally unexposed on this backend, not a bug in the probe.
  - There **is** a private ioctl pair that genuinely reprograms the sensor at
    runtime — `VIDIOC_S_SENSOR_FMT`/`VIDIOC_G_SENSOR_FMT`
    (`esp_video_ioctl.h:24-25` →
    `dvp_video_set_sensor_format()`/`dvp_video_get_sensor_format()`,
    `esp_video_dvp_device.c:425-439`), which call
    `esp_cam_sensor_set_format()` (rewrites SCCB registers + `ov2640_set_outsize`)
    followed by `init_config()` (recomputes `buf_size = width * height * bpp / 8`
    and DMA alignment for the new mode — at `640x480` YUV422 that is `614400`
    bytes/frame vs `115200` at `240x240`, i.e. roughly 5x the PSRAM per buffer).
    `camera_hal_init()` now calls `VIDIOC_G_SENSOR_FMT`
    (`camera_hal_log_sensor_format()`) and logs the active sensor mode's build-
    time name/size/fps directly from the driver, e.g.
    `"DVP_8bit_20Minput_YUV422_240x240_25fps"`.
  - However, `VIDIOC_S_SENSOR_FMT` is **not actually usable from application
    code** to switch resolution: it requires a valid
    `const esp_cam_sensor_format_t *` pointing at one of the *other* entries in
    `ov2640_format_info[]` (with their private, internal `regs`/`regs_size`
    register-list pointers). `esp_video` 1.3.1 never calls
    `esp_cam_sensor_query_support_formats()` and exposes no ioctl that
    enumerates that table to applications — `VIDIOC_G_SENSOR_FMT` only ever
    returns the *currently active* entry. There is no supported way to obtain
    a pointer to, say, the `640x480` entry from outside the sensor driver.
  - **Conclusion**: 240x240 is not a hardware ceiling and not a `camera_hal.c`
    limitation — it is a deliberate compile-time choice
    (`CONFIG_CAMERA_OV2640_DVP_DEFAULT_FMT` in `sdkconfig.defaults`) baked into
    the vendored `esp_video`==1.3.1 + `esp_cam_sensor` OV2640 pairing, and the
    standard V4L2 negotiation surface (`TRY_FMT`/`S_FMT`/`ENUM_FMT`/
    `ENUM_FRAMESIZES`) is architecturally incapable of changing it on this DVP
    backend. The only real path to a higher resolution is to **reflash** with
    a different `CAMERA_OV2640_DVP_DEFAULT_FMT` Kconfig choice (e.g.
    `CAMERA_OV2640_DVP_JPEG_640X480_25FPS`) — a build-time decision with a real
    PSRAM-budget consequence (`>300KB` free headroom rule in `CLAUDE.md`/
    `docs/PERSISTENCE.md`) that needs its own measurement on hardware, not a
    runtime negotiation routine in `camera_hal.c`.
- **Experimento de alta resolucao (640x480) — concluido 2026-06-07: `JPEG_640X480_25FPS`
  marcado NAO-FUNCIONAL em hardware real, com evidencia de captura quebrada.**
  `sdkconfig.defaults` selecionou `CONFIG_CAMERA_OV2640_DVP_JPEG_640X480_25FPS=y`
  (no lugar de `YUV422_240X240_25FPS`), o firmware foi compilado e flashado via
  `idf.py -p COM12 flash`, e o resultado foi validado em hardware real (nao
  assumido) em duas rodadas independentes de boot + captura via
  `/api/camera/snapshot` + `/api/camera/status`, com leitura do log serial em
  paralelo.
  - **A negociacao de formato funcionou exatamente como o codigo espera** — o
    sensor entrou no modo nativo 640x480 (`modo do sensor ativo:
    "DVP_8bit_20Minput_JPEG_640x480_25fps" 640x480 fmt=12 fps=25`),
    `VIDIOC_G_FMT` reportou `640x480 fmt=JPEG`, e `VIDIOC_S_FMT` aceitou o
    formato (`last_sfmt_errno=0`). Os probes `VIDIOC_TRY_FMT` confirmaram que
    QQVGA/QVGA/VGA continuam recusados com `errno=22` (EINVAL) e so o tamanho
    nativo e aceito pelo `S_FMT` — a mesma assinatura ja documentada para
    240x240, agora provada tambem para 640x480.
  - **Mas a captura de frames falha de forma reprodutivel e consistente.**
    Em ambas as rodadas, `camera_hal_capture()` recebeu 10-12 buffers
    consecutivos com `bytesused=0` (`frame invalido tentativa=N ... bytes=0
    fmt=0x4745504a`); na primeira rodada as 12 tentativas falharam e a camera
    foi desinicializada com `ESP_FAIL` (`/api/camera/snapshot` -> HTTP 503
    `{"ok": false, "error": "ESP_FAIL"}`, `/api/camera/status` ->
    `effective_width=0 effective_height=0 last_error=-1 (ESP_FAIL)
    phase=capture`); na segunda rodada uma tentativa "passou" do ponto de
    vista do V4L2 (sem log de `frame invalido`), mas o buffer dequeued trazia
    um tamanho de **lixo/memoria nao inicializada — `last_jpeg_bytes=4294780279`
    (`(uint32_t)(-187017)`, ~4GB, impossivel para um JPEG 640x480 num buffer de
    307200 bytes)** que `camera_service` registrou como sucesso
    (`capture_count=1 fail_count=0 last_error=0`); so a checagem de sanidade da
    camada HTTP recusou o payload (`/api/camera/snapshot` -> HTTP 503
    `ESP_FAIL`). **Achado adicional, fora do escopo deste experimento mas digno
    de nota**: `camera_service` confia no `bytesused` retornado pelo driver sem
    validar contra o tamanho do buffer alocado — vale revisitar essa checagem
    de sanidade especificamente (nao so o caminho feliz `>0 && <= buf_size`).
  - **Por que nao e um problema do struct `v4l2_format` zero-inicializado** —
    a hipotese natural antes de testar em hardware era que um `v4l2_format`
    zero-inicializado (so `width`/`height`/`pixelformat` preenchidos, sem
    `field`/`bytesperline`/`sizeimage`/`colorspace`/etc.) pudesse ser um
    descritor invalido para o caminho JPEG e explicar tanto a recusa do
    `TRY_FMT` quanto os buffers vazios. Para testar isso de forma controlada,
    `camera_hal_init()` agora loga a struct completa (todos os campos de
    `v4l2_pix_format`, nao so resolucao/formato) antes/depois de `G_FMT`,
    `TRY_FMT` e `S_FMT`, e o probe nativo do `TRY_FMT` passou a reusar — campo
    a campo, via `*base_fmt` — exatamente a struct que `VIDIOC_G_FMT` reportou,
    em vez de zero-inicializar e preencher so tres campos
    (`camera_hal_log_v4l2_format()`, `camera_hal_probe_try_fmt(fd, pixfmt,
    base_fmt, base_valid)`). O resultado refutou a hipotese de forma limpa:
    - `VIDIOC_G_FMT` em si reporta `field=0 bytesperline=0 sizeimage=0
      colorspace=0 priv=0 flags=0 ycbcr_enc=0 quantization=0 xfer_func=0` — ou
      seja, **a struct zero-inicializada e exatamente o que o proprio driver
      considera "formato atual valido"**.
    - `VIDIOC_TRY_FMT` recusa essa mesma struct, byte a byte identica ao que
      `G_FMT` acabou de devolver, com `errno=22` —
      `VIDIOC_TRY_FMT nativo (struct preservada de G_FMT): rejeitado errno=22
      ... ate o proprio formato reportado pelo driver foi recusado`. Isso prova
      que `TRY_FMT` esta simplesmente nao-implementado/sempre-EINVAL no backend
      DVP (consistente com `VIDIOC_ENUM_FRAMESIZES` tambem sempre falhar com
      `errno=22`), e nao e sensivel ao conteudo da struct.
    - `VIDIOC_S_FMT`, com a mesma struct preservada de `G_FMT`, **aceita**
      (`last_sfmt_errno=0`) e devolve a struct sem recalcular
      `bytesperline`/`sizeimage` — confirmando que o `set_format` do driver so
      faz a checagem de igualdade de `width`/`height`/`pixelformat` ja
      documentada acima, e ignora os demais campos tanto na entrada quanto na
      saida.
    - **Conclusao**: a falha de captura nao tem nada a ver com a forma como o
      app monta o `v4l2_format` — a negociacao (`G_FMT`/`TRY_FMT`/`S_FMT`) se
      comporta de modo identico independentemente da struct usada. O problema
      esta no pipeline de captura em si (DMA/timing/registradores do modo
      `DVP_8bit_20Minput_JPEG_640x480_25fps` especificamente), fora do alcance
      de qualquer ajuste possivel a partir do codigo da aplicacao.
  - **Conhecido e esperado, nao uma regressao**: enquanto este modo estava
    ativo, frames JPEG nativos nao alimentam `camera_service_analyze_yuv422()`
    (precisa de `YUYV`/`YUV422P`), entao `/api/vision/observe` reportava
    `scene=unknown`. Esse trade-off ja era esperado e documentado antes do
    teste — nao influenciou a decisao de marcar o modo como nao-funcional
    (a captura quebrada sozinha ja e suficiente).
  - **Decisao**: `JPEG_640X480_25FPS` esta marcado **nao-funcional nesta placa**
    para fins de monitoramento/snapshot — nenhum frame valido foi produzido em
    24 tentativas de captura ao longo de duas rodadas de boot independentes.
    Nao ha numeros de FPS/tempo de captura/tamanho medio de frame a registrar
    porque nunca houve um frame valido para medir. Proximo passo: testar
    `JPEG_320X240_50FPS` (outro modo OV2640 nativo compilavel, ver secao
    abaixo) antes de decidir se volta para `YUV422_240X240_25FPS` como
    baseline definitivo.
  - **Rollback (mantido pronto, nao executado ainda)**: revert
    `sdkconfig.defaults` para `CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS=y`,
    apagar o `sdkconfig` em cache (ou `idf.py fullclean`), `idf.py reconfigure`,
    rebuild e reflash. Nenhuma mudanca em `camera_hal.c`/`camera_service.c`
    precisa ser desfeita — o codigo de pass-through/diagnostico honesto
    funciona para qualquer um dos tres modos (`YUV422_240X240`,
    `JPEG_640X480`, `JPEG_320X240`).
- **Experimento de alta resolucao (320x240) — concluido 2026-06-07: tambem
  NAO-FUNCIONAL, com a MESMA assinatura de falha do modo 640x480.**
  `sdkconfig.defaults` selecionou `CONFIG_CAMERA_OV2640_DVP_JPEG_320X240_50FPS=y`,
  o firmware foi recompilado (full rebuild — a troca de `choice` do Kconfig
  altera `CONFIG_CAMERA_OV2640_DVP_IF_FORMAT_INDEX_DEFAULT`, uma constante de
  build-time) e flashado via `idf.py -p COM12 flash`, e validado em hardware
  real em duas rodadas de boot independentes, com leitura do log serial em
  paralelo a chamadas de `/api/camera/snapshot`/`/api/camera/status`.
  - **Negociacao de formato — identica ao modo 640x480, exatamente como
    previsto**: `modo do sensor ativo: "DVP_8bit_20Minput_JPEG_320x240_50fps"
    320x240 fmt=12 fps=50`; `VIDIOC_G_FMT` reporta `320x240 fmt=JPEG
    field=0 bytesperline=0 sizeimage=0 colorspace=0 ...` (struct zero exceto
    width/height/pixelformat — confirma de novo que essa e a forma "valida"
    para o proprio driver); `VIDIOC_TRY_FMT` recusa QQVGA/QVGA/VGA E o proprio
    320x240 nativo com `errno=22` (incluindo a struct preservada, byte a byte
    identica a que `G_FMT` relatou); `VIDIOC_S_FMT` aceita de primeira
    (`last_sfmt_errno=0`) com a mesma struct preservada e a devolve sem
    recalcular `bytesperline`/`sizeimage`. Ou seja: a camada de negociacao
    V4L2 se comporta de forma **byte-a-byte identica** nos dois modos JPEG
    nativos testados — a unica coisa que muda e a resolucao reportada.
  - **Captura — falha identica, 24/24 tentativas (12+12 em duas rodadas)**:
    `frame invalido tentativa=1..12 index=0 bytes=0 fmt=0x4745504a` em ambas
    as rodadas, seguido de `camera desinicializada` e `ESP_FAIL`.
    `/api/camera/snapshot` -> HTTP 503 `{"ok": false, "error": "ESP_FAIL"}`
    (ambas as chamadas); `/api/camera/status` -> `effective_width=0
    effective_height=0 last_error=-1 (ESP_FAIL) phase=capture fail_count=2
    last_jpeg_bytes=0 capture_count=0`. Nenhum frame valido, nenhum dado de
    desempenho a registrar — identico ao pior caso do experimento 640x480.
  - **Conclusao acumulada (640x480 + 320x240): 48/48 falhas de captura em
    QUATRO rodadas de boot independentes, em DOIS modos JPEG nativos
    diferentes, com a MESMA assinatura (`bytesused=0`/lixo, negociacao OK,
    pipeline de captura quebrado).** Isso descarta definitivamente a hipotese
    de "resolucao especifica" — o problema e estrutural ao caminho de captura
    JPEG nativo do `esp_video`/`esp_cam_sensor` para o sensor OV2640 nesta
    placa (DVP), nao a uma resolucao ou timing de um modo isolado. Nao ha
    ajuste possivel a partir do codigo da aplicacao (a negociacao ja se provou
    correta e identica em ambos os modos) — seria necessario depurar o driver
    `esp_video`/`esp_cam_sensor` em si (DMA, registradores SCCB do modo,
    timing do controlador DVP) ou trocar de sensor/biblioteca, ambos fora do
    escopo deste experimento.
  - **Decisao final**: `YUV422_240X240_25FPS` — o unico modo com captura
    *comprovadamente funcional* nesta placa (e a base de todo o trabalho de
    visao/snapshot/MJPEG ja existente) — volta a ser o baseline, sem
    ressalvas. Nenhum dos dois modos JPEG nativos (640x480 ou 320x240) e
    viavel como alternativa de monitoramento de alta resolucao nesta placa.
    `sdkconfig.defaults` foi revertido para
    `CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS=y`, o firmware foi
    recompilado e reflashado, e a captura foi revalidada em hardware antes de
    fechar o experimento — **confirmado funcionando, primeira tentativa, sem
    nenhum `frame invalido`**: `modo do sensor ativo:
    "DVP_8bit_20Minput_YUV422_240x240_25fps" 240x240 fmt=2 fps=25`,
    `/api/camera/snapshot` -> HTTP 200 `image/jpeg` 17816 bytes,
    `/api/camera/status` -> `effective_width=240 effective_height=240
    last_frame_format=yuv422p capture_count=1 fail_count=0 last_error=0
    capture_ms=227`. Essa mesma rodada tambem prova que o novo logging
    completo de `v4l2_format` (G_FMT/TRY_FMT/S_FMT) nao interfere em nada no
    caminho de captura que funciona — `VIDIOC_TRY_FMT` continua recusando tudo
    (inclusive a struct preservada do `G_FMT`, com o mesmo `errno=22` visto
    nos modos JPEG — confirma mais uma vez que `TRY_FMT` e
    universalmente nao-implementado neste backend DVP, independente do
    formato de pixel), e mesmo assim a captura funciona perfeitamente — o
    problema esta isolado ao pipeline de captura JPEG nativo, nao a nada que o
    aplicativo controla.
  - **Estado final do experimento**: `sdkconfig.defaults` no baseline
    `YUV422_240X240_25FPS`; instrumentacao de diagnostico
    (`camera_hal_log_sensor_format`, `camera_hal_probe_try_fmt`,
    `camera_hal_log_v4l2_format`, named constants
    `NB_CAMERA_NATIVE_FALLBACK_WIDTH/HEIGHT`) permanece no codigo. As
    constantes de fallback foram realinhadas para `240x240`, acompanhando o
    baseline final, para que `/api/camera/status` nao anuncie `640x480` antes
    do primeiro frame. A instrumentacao roda em todo boot, documenta o
    comportamento real do driver para qualquer pessoa que reabrir essa
    investigacao no futuro, e nao tem custo de runtime alem de algumas linhas
    de log no caminho de inicializacao (nao roda em nenhum caminho critico).
- **Experimento raw VGA YUV422 — validado em hardware real em 2026-06-07**:
  para responder se o barramento DVP/raw funciona em VGA quando o caminho JPEG
  nativo falha, foi criado o perfil
  `sdkconfig.experiment.ov2640-yuv422-640x480.defaults`. Ele nao troca o
  baseline do produto; deve ser usado apenas com `sdkconfig` limpo e
  `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.experiment.ov2640-yuv422-640x480.defaults"`.
  O perfil seleciona `CONFIG_CAMERA_OV2640_DVP_YUV422_640X480_6FPS=y`.
  Resultado medido apos flash: `/api/camera/snapshot` retornou JPEG valido
  (`79951` bytes, assinatura `FF D8 FF E0`) e `/api/camera/status` reportou
  `effective_width=640`, `effective_height=480`, `last_frame_width=640`,
  `last_frame_height=480`, `last_frame_bytes=614400`, `last_frame_format=yuv422p`,
  `capture_count=1`, `fail_count=0`, `last_error=0`. Isso prova que o caminho
  DVP/raw VGA funciona nesta placa; a limitacao anterior era o caminho JPEG
  nativo do driver, nao a camera nem o barramento DVP. As tentativas de usar
  VGA como video ao vivo mostraram que o teto fica limitado pela captura raw
  (~350ms/frame) e pelo encode JPEG por software. Decisao de produto em
  2026-06-07: remover o monitoramento de video do robo e usar a camera apenas
  para snapshot/analyze/face-detect. Com isso, `GET /api/camera/stream.mjpg` e
  o proxy `GET /api/vision/stream.mjpg` foram removidos; `/api/camera/snapshot`,
  `/api/vision/observe` e `/api/vision/analyze` permanecem como superficie de
  percepcao.
- NoiseBot now retains a strong presence candidate (`score>=60`) for up to 2.5s
  and still requires 2 raw candidate samples before publishing
  `PRESENCE_DETECTED`; retained samples keep `candidate` alive but cannot promote
  to `present` by themselves. The firmware also exposes
  `/api/vision/presence/reset` for clean validation runs and `spatial_score` in
  `/api/vision/observe` as diagnostic telemetry. A direct spatial-score bonus
  was tested and rejected because it produced false positives in an empty scene;
  the metric remains visible for future baseline calibration but does not drive
  the presence decision. Hardware validation on 2026-06-04 after the safer
  retention rule: `absence --reset-presence --min-fps 25` passed with 18/18
  valid observations, zero false positives, one candidate, `max_score=56`,
  `avg_capture_ms=153.0`, `min_fps=35.2` and final `absent`; controlled
  `presence --reset-presence --arm-timeout-s 10 --start-delay-s 6 --min-fps 25`
  passed with 56/56 valid observations, initial `absent`, final `present`,
  `PRESENCE_DETECTED` observed at 11826ms, `max_score=62`, `avg_capture_ms=151.8`
  and `min_fps=33.6`. Correctness is validated for on-demand trials, while the
  500ms latency criterion remains blocked by weak/static score evidence rather
  than capture time alone.
- `vision_service` now also has an explicit diagnostic polling mode, off by
  default, with `/api/vision/poll/start`, `/api/vision/poll/status` and
  `/api/vision/poll/stop`. It runs the same safe QQVGA observation path at a
  clamped interval (`250ms` minimum, `300ms` default), reports sample/failure
  counts and capture latency, and does not drive behavior automatically.
  Hardware validation on 2026-06-04 after flash: a default polling run produced
  7 samples in about 4s, zero failures, `avg_capture_ms=165`,
  `max_capture_ms=169`, and stopped cleanly with final `running=false`.
- The bridge v2 can answer local vision questions from the vision endpoint without
  invoking the LLM.
- Hardware validation on 2026-05-25 showed camera snapshots, bridge connection
  and TTS playback working together in the same firmware build.

## Open camera/vision work

- Promote the basic observation into presence detection (`PRESENCE_DETECTED` /
  `PRESENCE_LOST`) continuous mode only after shadow/lighting false positives
  are measured.
- Implement face detect sobre snapshot/analyze sem reintroduzir video ao vivo.
