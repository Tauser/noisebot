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

### Execução C0–C3 — 2026-06-20

OV2640 físico testado pela primeira vez nesta sessão, MACs confirmados por
`esptool chip_id` (main `90:e5:b1:cc:3d:58` COM5, head `20:6e:f1:b2:3c:f4`
COM12 — a nota de memória anterior tinha as portas trocadas, corrigida).

**Achado real de C0:** `nb_head_i2c_hal_scan()` (probe genérico
`i2c_master_probe`) não detecta o OV2640 — `E (807) i2c.master: probe
device timeout`, `SCCB 0x3C ausente`. Apesar disso, o driver `ov2640`
embutido no backend esp_video detecta o sensor segundos depois pelo seu
próprio protocolo SCCB: `I (827) ov2640: Detected Camera sensor PID=0x26`
(PID correto). **Gate C0 é um falso negativo conhecido**: o probe genérico
de 0 bytes não é compatível com o SCCB do OV2640; usar a detecção do driver
(`PID=0x26` no log) como critério real de aceite, não o scan genérico.

**Gates C1/C2 aprovados de primeira**, sem iteração: `esp_video_init` e
`VIDIOC_QUERYCAP` sem erro; `VIDIOC_S_FMT` aceito na primeira tentativa com
`CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS` (mesmo perfil do main
legado). `camera pronta esp_video 240x240 fmt=0x50323234
PSRAM=8189KB->7963KB`.

**Bug real encontrado e corrigido em C3:** a primeira tentativa do probe deu
2/5 capturas OK e 3 timeouts, com a telemetria do enlace mostrando
`retry=6 timeout=1` — a causa era `nb_head_camera_service_apply()` chamando
`nb_head_camera_hal_capture()` (bloqueante, ioctls V4L2) **dentro da própria
task do enlace** (via `on_message`), atrasando ACK/heartbeat o suficiente
para o main esgotar o timeout de 2 s em chamadas subsequentes. Corrigido
desacoplando a câmera para uma task dedicada (`nb_head_camera`, prioridade
6, abaixo do enlace) com fila própria — a task do enlace só enfileira o
comando (`nb_head_camera_service_apply`) e a resposta volta por uma fila
separada (`nb_head_link_service_send_camera_event`), consumida só pela task
do enlace (o `nb_link_engine` não tem lock interno; é single-thread por
design).

**Resultado após a correção:** 5/5 capturas reais no log do head —
`request_id=1..5 status=0 width=240 height=240 length=115200` — com a
telemetria do enlace limpa (`invalid=0 retry=0 timeout=0`) durante toda a
sequência. A confirmação linha-a-linha no log da main não foi capturada de
forma limpa nesta sessão (limitação de timing do script de captura serial
usado, não do firmware); a telemetria da main no mesmo intervalo mostrou
`invalid=0 retry=0 timeout=0`, consistente com sucesso. Recomenda-se
reconfirmar o log da main na próxima sessão antes de marcar C3 como
definitivamente fechado.

Gates C0 (com a ressalva do falso negativo), C1, C2 e C3 (lado head)
**aprovados**. C4 (headroom com display físico simultâneo) e o soak ainda
não foram executados.

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
