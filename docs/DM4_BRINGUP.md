# DM4 — Bring-up físico da câmera DVP no head

## Objetivo

Validar a câmera OV2640 DVP exclusivamente na Freenove CAM N16R8 (head),
depois que DM4.1–DM4.4 já provaram o contrato e o roteamento semântico sem
hardware. Esta etapa não migra storage, touch ou comportamento.

O baseline normal permanece com `CONFIG_NB_HEAD_CAMERA_HW_ENABLED=n`. O
perfil `sdkconfig.dm4-hw.defaults` é exclusivo de bancada.

## O que já está pronto antes da bancada

- `nb_head_camera_hal` (head): porte de `camera_hal.c` (main legado) para os
  pinos `NB_HEAD_PIN_CAM_*` de `nb_hw_config_head.h`. Mesma lógica de
  negociação V4L2 (preserva a struct de `VIDIOC_G_FMT` ao montar
  `VIDIOC_S_FMT`, fallback JPEG, warmup de frames) que já funcionou no main.
- `nb_head_i2c_hal` (head): porte de `i2c_hal.c`, bus único para o SCCB do
  OV2640 (endereço esperado `0x3C`).
- `nb_head_camera_service` aciona o HAL real quando
  `CONFIG_NB_HEAD_CAMERA_HW_ENABLED=y`: em `REQUEST_SNAPSHOT`, captura de
  verdade e responde `OK`/`ERROR` com dimensões e tamanho do frame (nunca os
  pixels — isso é trabalho do canal `BULK`, fora deste corte).
- Perfil `sdkconfig.dm4-hw.defaults`: liga o enlace, o receptor semântico e o
  hardware, com a mesma resolução validada no main legado (YUV422 240×240
  25fps, índice de formato 4).
- Build validado (sem flash): `idf.py -B build-dm4-hw -D SDKCONFIG=sdkconfig.dm4-hw -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dm4-hw.defaults" build`
  limpo com `-Werror`, exercitando o branch físico completo (ioctls V4L2,
  `select CAMERA_OV2640` resolvido).

**O que isso NÃO prova:** que o OV2640 físico responde, que o SCCB enxerga o
sensor, que `VIDIOC_S_FMT` é aceito na placa real, ou que a captura retorna
bytes válidos. O comentário original em `camera_hal.c` documenta que a
integração no main exigiu iteração real em bancada (negociação de formato,
fallback JPEG, struct preservada de `G_FMT`) — é razoável esperar o mesmo
aqui, mesmo com os pinos corretos.

## Pré-condições

- DM1 aprovado (enlace estável) e DM4.4 confirmado em bancada (round trip
  semântico sem hardware, ver `docs/DM4_CAMERA_MIGRATION.md`).
- Câmera OV2640 da Freenove fisicamente presente e não danificada.
- Nenhum outro driver disputando os pinos `NB_HEAD_PIN_CAM_*` ou o I2C
  (`NB_HEAD_PIN_I2C_SDA/SCL`, compartilhado pelo SCCB).
- PSRAM do head com pelo menos 300 KB livres além dos buffers ativos
  (display + enlace), conforme CLAUDE.md.

## Gate C0 — I2C/SCCB visível

1. Gravar um perfil mínimo que apenas chama `nb_head_i2c_hal_init()` +
   `nb_head_i2c_hal_scan()` no boot (ou usar um log temporário em
   `nb_head_camera_hal_init()` antes do `esp_video_init`).
2. Confirmar que o endereço `0x3C` aparece no scan.

Aceite C0: SCCB do OV2640 responde no barramento. Sem isso, não adianta
prosseguir — o problema é elétrico/pinagem, não software.

## Gate C1 — esp_video_init e VIDIOC_QUERYCAP

1. Gravar `build-dm4-hw` (perfil `sdkconfig.dm4-hw.defaults`) no head.
2. Observar o log: `esp_video_init` deve retornar `ESP_OK` e o `open()` do
   device DVP deve suceder.
3. Se `esp_video_init` falhar, revisar pinagem DVP
   (`NB_HEAD_PIN_CAM_D0..D7/VSYNC/HREF/PCLK/XCLK`) antes de qualquer outra
   tentativa.

Aceite C1: `VIDIOC_QUERYCAP` sem erro, sem reboot, sem panic.

## Gate C2 — negociação de formato

1. Observar `VIDIOC_S_FMT` no log: deve aceitar YUV422/YUYV na primeira ou
   segunda tentativa, ou cair no fallback JPEG.
2. Se `VIDIOC_S_FMT` falhar definitivamente nas duas tentativas, comparar com
   os achados do main legado em `camera_hal.c` — historicamente o driver só
   negocia o tamanho nativo definido em build-time
   (`CONFIG_CAMERA_OV2640_DVP_*`); confirmar que o perfil `dm4-hw.defaults`
   usa exatamente `CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS`, igual ao
   main.

Aceite C2: `VIDIOC_S_FMT` aceito, `s_frame_width`/`s_frame_height` reportados
como 240×240.

## Gate C3 — primeira captura real

1. Usar o probe `CONFIG_NB_DM4_CAMERA_PROBE` (main) + `dm4-hw` (head) juntos:
   gravar `build-dm4` no main e `build-dm4-hw` no head.
2. Confirmar no log do head: `accepted` cresce, e a resposta enviada ao main
   é `status=0` (`NB_CAMERA_LINK_STATUS_OK`) com `width=240 height=240` e
   `length` > 0.
3. Confirmar no log da main: `DM4 camera event request_id=N status=0
   mode=0` para as 5 requisições — diferente do `status=3` esperado em
   bancada sem hardware (DM4.4).

Aceite C3: 5/5 capturas reais com `status=0` e `length` plausível para
YUV422 240×240 (≈115200 bytes não comprimidos, ou bem menor se cair no
fallback JPEG).

## Gate C4 — headroom de memória

1. Repetir o probe com display físico também ativo (`sdkconfig.dm2-hw` +
   câmera) para medir o pior caso de PSRAM concorrente.
2. Confirmar PSRAM livre ≥ 300 KB após `nb_head_camera_hal_init()`, mesmo com
   o framebuffer do display alocado.

Aceite C4: nenhum `ESP_ERR_NO_MEM` em `nb_head_camera_hal_init()` com os dois
subsistemas ativos.

## Soak

- Repetir o probe (5 capturas) pelo menos 10 vezes consecutivas, sem reboot
  do head entre execuções, confirmando captura estável (sem degradar PSRAM
  DMA/internal entre execuções).
- Zero corrupção, panic, watchdog ou impacto no enlace (`retry=0`,
  `timeout=0`, `spi_err=0`) durante o soak.

## Rollback

1. Gravar novamente `build-dm4` (perfil semântico, sem hardware) no head.
2. Confirmar log: receptor semântico ativo, captura física ausente,
   `hardware_ready=0`.
3. Em caso de instabilidade elétrica, desconectar a câmera e voltar ao
   perfil padrão (`CONFIG_NB_HEAD_CAMERA_ENABLED=n`).

DM4 (driver físico) só muda para `FEITO` após evidência de C0–C4 e soak,
anexada a este documento — igual ao critério usado em DM1/DM2.
