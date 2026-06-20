# DM4 — Migração da câmera para o head-controller

## Objetivo

Transferir a autoridade física da câmera DVP (OV2640, Freenove CAM N16R8) para
o head-controller, mantendo a análise semântica (presença, reconhecimento de
rosto) no server, e sem mover decisão de comportamento para o head. Segue a
mesma filosofia de DM2: contrato semântico primeiro, hardware depois, fallback
documentado em cada corte.

## Estado herdado

- `camera_hal.c`/`.h` (Layer 1) e `camera_service` (Layer 4) vivem hoje em
  `firmware/main-controller`, mas os pinos DVP são fisicamente exclusivos da
  placa Freenove/head — isso é resíduo pré-migração dual-MCU, não arquitetura
  alvo.
- `vision_preview_service` desenha o preview JPEG no display do main a 5 FPS
  com bbox de detecção (`MSG_FACE_BOX`), recebido do server via bridge.
- `nb_inter_mcu_protocol.h` já reservava `NB_LINK_MSG_CAMERA_COMMAND` e
  `NB_LINK_MSG_CAMERA_EVENT` desde F0, sem payload definido.
- O head-controller ainda não tem nenhum componente de câmera.

## DM4.1 — contrato semântico (concluído, sem hardware)

Criado `nb_camera_protocol.h`/`.c` em
`firmware/shared/components/nb_inter_mcu_protocol`, espelhando o desenho de
`nb_display_protocol.h`:

- `nb_camera_link_command_t` (16 bytes): opcode (`REQUEST_SNAPSHOT`,
  `SET_PREVIEW`, `SET_MODE`), preview on/off, modo (`QQVGA`/`QVGA`) e
  `request_id` para correlacionar resposta;
- `nb_camera_link_event_t` (16 bytes): status (`OK`/`ERROR`/`BUSY`/
  `UNAVAILABLE`), modo, dimensões e `length` do frame disponível — nunca
  carrega pixels;
- capability `NB_LINK_CAP_CAMERA_SEMANTIC`;
- pixels reais (JPEG) trafegam depois pelo canal `BULK`, sob demanda,
  identificados pelo mesmo `request_id` — igual ao desenho de áudio/LTM do
  plano dual-MCU;
- 66/66 testes de host verdes (`test/host/inter_mcu_protocol`), cobrindo
  validação de versão, opcode, preview, modo e campos reservados.

Prefixo `nb_camera_link_*` em vez de `nb_camera_*`: o main-controller já tem
`nb_camera_mode_t`/`nb_camera_event_t` locais no `camera_hal`/`camera_service`
legado (passo 7 abaixo remove esse caminho). Os dois coexistem até lá; o
prefixo extra evita colisão de símbolo nesse meio-tempo.

Este corte não toca GPIO, não move `camera_hal.c` e não altera
`vision_preview_service`. Prova apenas que o contrato compila, valida e
serializa corretamente nos dois lados.

## DM4.2 — round trip semântico no head (concluído, sem hardware)

Criado `nb_head_camera_service` no head-controller, espelhando
`nb_head_display_service`:

- `CONFIG_NB_HEAD_CAMERA_ENABLED` (default `n`) liga apenas o receptor
  semântico; nenhum driver DVP é tocado;
- `nb_head_link_service` despacha `NB_LINK_MSG_CAMERA_COMMAND` (canal
  `CONTROL`) para o serviço e responde `NB_LINK_MSG_CAMERA_EVENT` (canal
  `EVENT`) com `status=UNAVAILABLE`, já que o hardware está deferido;
- capability `NB_LINK_CAP_CAMERA_SEMANTIC` somada à `NB_LINK_CAP_DISPLAY_SEMANTIC`
  no `HELLO` do head;
- perfil `sdkconfig.dm4.defaults` (head) habilita
  `CONFIG_NB_INTER_MCU_SPI_ENABLED` + `CONFIG_NB_HEAD_CAMERA_ENABLED` para o
  próximo teste de bancada, mesmo padrão de `sdkconfig.dm2.defaults`.

Build validado localmente (sem flash): `idf.py build` limpo com `-Werror` no
main-controller, no head-controller (perfil padrão) e no head-controller com
o perfil `sdkconfig.dm4.defaults`. Nenhum board foi flasheado nesta etapa.

## DM4.3 — cliente de câmera no main (concluído, sem hardware)

Adicionado ao `nb_main_link_service` (infra, Layer 2):

- `nb_main_link_service_request_camera(const nb_camera_link_command_t *)`:
  envia o comando pelo canal `CONTROL` sob demanda (fire-once, sem fila de
  estado persistente como o display); exige `READY` e a capability do peer;
- `nb_main_link_service_get_last_camera_event(nb_camera_link_event_t *)`:
  copia o último evento recebido do head pelo canal `EVENT`;
- `on_message` do main passa a validar e armazenar `NB_LINK_MSG_CAMERA_EVENT`.

Nenhum consumidor de produto chama essas funções ainda — não há Layer 4/5
publicando `NB_EVT_*` a partir do evento de câmera, nem qualquer serviço
decidindo quando solicitar um snapshot. Isso é deliberado: o cliente existe,
mas a decisão de quando/por que capturar (presença, reconhecimento, bbox)
fica para quando `vision_preview_service`/`camera_service` migrarem (passo 7).

Build validado (sem flash): `idf.py build` limpo com `-Werror` no
main-controller com o perfil padrão e com `sdkconfig.dm2.defaults` (enlace
habilitado, exercitando os caminhos novos), e no head-controller com o
perfil padrão e `sdkconfig.dm4.defaults`.

## DM4.4 — primeira prova de consumo real (concluída, sem hardware)

`app_main` ganhou um probe opt-in (`CONFIG_NB_DM4_CAMERA_PROBE`, dependente de
`CONFIG_NB_DM1_BENCH_PROFILE`), mesmo padrão do `CONFIG_NB_DM2_DISPLAY_PROBE`:
após o enlace chegar a `READY`, envia 5 `NB_CAMERA_LINK_OP_REQUEST_SNAPSHOT`
reais via `nb_main_link_service_request_camera()`, espera até 2 s por
`nb_main_link_service_get_last_camera_event()` correlacionado pelo
`request_id`, e loga o resultado.

Isso é o primeiro "consumidor real" do cliente de câmera — não é mais só uma
chamada direta de teste, é o mesmo caminho que um futuro `vision_service`
remoto usaria. Como o head ainda não inicializa o driver DVP, a resposta
esperada em bancada é `status=UNAVAILABLE` para as 5 requisições; isso prova
o roteamento ponta a ponta (`main → CONTROL → head → EVENT → main`) sem
qualquer hardware de câmera.

Perfil `sdkconfig.dm4.defaults` (main) criado, espelhando o do head: liga
`CONFIG_NB_INTER_MCU_SPI_ENABLED` + `CONFIG_NB_DM1_BENCH_PROFILE` +
`CONFIG_NB_DM4_CAMERA_PROBE`. Para o próximo teste de bancada, gravar
`build-dm4` (perfil `sdkconfig.dm4.defaults`) nos dois MCUs e confirmar 5
linhas `DM4 camera event request_id=N status=3` no log da main.

Build validado (sem flash): `idf.py build` limpo com `-Werror` no
main-controller com o perfil padrão e com `sdkconfig.dm4.defaults`. Nenhum
board foi flasheado.

## DM4.5 — HAL físico portado, build validado (aguardando bancada)

Criados no head-controller, porte de `camera_hal.c`/`i2c_hal.c` (main
legado):

- `nb_head_i2c_hal`: bus I2C/SCCB único em `NB_HEAD_PIN_I2C_SDA/SCL`, mesmo
  desenho do `i2c_hal` do main;
- `nb_head_camera_hal`: driver DVP via esp_video/V4L2 nos pinos
  `NB_HEAD_PIN_CAM_*` de `nb_hw_config_head.h`, preservando a negociação de
  formato que funcionou no main (struct de `VIDIOC_G_FMT` reaproveitada em
  `VIDIOC_S_FMT`, fallback JPEG, warmup de 2 frames). As funções de
  diagnóstico puramente informativas do original (`VIDIOC_ENUM_FRAMESIZES`,
  `VIDIOC_G_SENSOR_FMT`, sweep de `VIDIOC_TRY_FMT`) não foram portadas —
  não afetam a inicialização;
- `CONFIG_NB_HEAD_CAMERA_HW_ENABLED` (depende de `NB_HEAD_CAMERA_ENABLED`,
  `select CAMERA_OV2640`), mesmo padrão de `CONFIG_NB_HEAD_DISPLAY_HW_ENABLED`;
- `nb_head_camera_service` aciona o HAL real em `REQUEST_SNAPSHOT` quando
  `hardware_ready`: captura de verdade e responde `OK`/`ERROR` com
  dimensões e tamanho do frame — nunca os pixels;
- `sdkconfig.dm4-hw.defaults` (head): liga o enlace, o receptor semântico e
  o hardware, com a mesma resolução do main (`YUV422 240x240 25fps`,
  `CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS`).

Build validado (sem flash): `idf.py -B build-dm4-hw -D SDKCONFIG=sdkconfig.dm4-hw
-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dm4-hw.defaults" build`
limpo com `-Werror`, exercitando o branch físico completo (ioctls V4L2,
`select CAMERA_OV2640` resolvido, `CONFIG_NB_HEAD_CAMERA_HW_ENABLED=y`
confirmado no sdkconfig gerado). **Nenhuma câmera real foi testada** — isso
prova só que o código compila e linka corretamente com o backend V4L2 real,
não que o OV2640 físico responde. Procedimento de bring-up físico (gates
C0–C4 e soak) documentado em `docs/DM4_BRINGUP.md`.

## Ordem de implementação

1. ~~Definir contrato semântico de câmera (DM4.1)~~ — `FEITO`.
2. ~~Round trip semântico no head, sem hardware (DM4.2)~~ — `FEITO`.
3. ~~Cliente de câmera no main, sem hardware (DM4.3)~~ — `FEITO`.
4. ~~Primeira prova de consumo real, sem hardware (DM4.4)~~ — `FEITO`.
5. ~~Criar `nb_head_camera_hal` e `nb_head_i2c_hal`, portar
   `camera_hal_init/capture/release` e acionar o HAL real em
   `nb_head_camera_service` (DM4.5)~~ — `FEITO` em software/build. Bring-up
   físico (`docs/DM4_BRINGUP.md`, gates C0–C4 e soak) ainda pendente de
   bancada — sem isso DM4.5 não pode ser considerada validada eletricamente.
6. Manter o preview local no head (renderizado no próprio display, sem
   depender do main) — ainda não implementado.
7. Wirear `nb_main_link_service_request_camera`/`get_last_camera_event` a um
   consumidor de produto real (presença/reconhecimento) e buscar os bytes
   JPEG pelo canal `BULK` sob demanda.
8. Remover `camera_hal.c`/`camera_service` do main e mover a responsabilidade
   de preview/overlay de bbox para o head (`vision_preview_service` deixa de
   desenhar localmente; bbox passa a ser parâmetro do estado visual remoto via
   `visual_state_facade`, análogo aos overlays de DM2).
9. Validar headroom de PSRAM no head com câmera + display simultâneos
   (mínimo de 300 KB livres, conforme CLAUDE.md) — gate C4 de
   `docs/DM4_BRINGUP.md`.
10. Só depois remover qualquer caminho de câmera do main em DM6.

## Invariantes

- main nunca decide quando capturar fora de pedido explícito (server/bridge);
- head não interpreta o conteúdo do frame — apenas captura e entrega;
- análise semântica (presença, identidade) permanece no server;
- nenhum framebuffer de câmera em SRAM — captura e JPEG ficam em PSRAM no
  head;
- ausência de head/câmera desabilita presença visual sem inferir ausência do
  usuário por falta de sensor (conforme `DUAL_MCU_ARCHITECTURE_PLAN.md` §6.3);
- falha de câmera nunca afeta `motion_safety`.

## Gates (a definir por sub-etapa física)

- builds main/head com `-Werror`;
- protocolo host verde (cobre DM4.1, já aprovado);
- headroom de PSRAM mínimo de 300 KB com câmera + display ativos no head;
- soak de captura sem corrupção de frame nem impacto no enlace de display;
- desconexão/reconexão da câmera não derruba o enlace nem o display.

### Gate de bancada DM4.4 (round trip semântico, sem DVP)

1. Gravar `build-dm4` com `sdkconfig.dm4.defaults` na Waveshare (porta da
   main).
2. Gravar `build-dm4-hw`-equivalente (perfil padrão + `sdkconfig.dm4.defaults`
   do head) na Freenove.
3. Observar o log da main: 5 linhas
   `DM4 camera event request_id=N status=3` (3 = `NB_CAMERA_LINK_STATUS_UNAVAILABLE`).
4. Confirmar no head: `accepted` incrementando, `rejected=0`, sem reboot nem
   impacto na telemetria do enlace (`retry=0`, `timeout=0`, `spi_err=0`).
5. Qualquer `status` diferente de 3, timeout sem resposta, ou queda do enlace
   é falha do gate — investigar antes de avançar para o driver DVP físico.

## Fora do corte DM4.1/DM4.2/DM4.3/DM4.4/DM4.5

- bring-up elétrico/funcional real do OV2640 (gates C0–C4 de
  `docs/DM4_BRINGUP.md`) — DM4.5 só portou e compilou o driver, não testou
  a câmera física;
- preview local no head;
- transferência BULK de JPEG;
- qualquer consumidor de produto (presença, reconhecimento) chamando o
  cliente de câmera do main em runtime — o probe da DM4.4 não conta como
  consumidor de produto, é só validação de bancada;
- remoção de `camera_hal`/`camera_service` do main.
