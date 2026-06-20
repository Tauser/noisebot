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

- `nb_camera_command_t` (16 bytes): opcode (`REQUEST_SNAPSHOT`, `SET_PREVIEW`,
  `SET_MODE`), preview on/off, modo (`SAFE_QQVGA`/`BETTER_QVGA`) e
  `request_id` para correlacionar resposta;
- `nb_camera_event_t` (16 bytes): status (`OK`/`ERROR`/`BUSY`/`UNAVAILABLE`),
  modo, dimensões e `length` do frame disponível — nunca carrega pixels;
- capability `NB_LINK_CAP_CAMERA_SEMANTIC`;
- pixels reais (JPEG) trafegam depois pelo canal `BULK`, sob demanda,
  identificados pelo mesmo `request_id` — igual ao desenho de áudio/LTM do
  plano dual-MCU;
- 66/66 testes de host verdes (`test/host/inter_mcu_protocol`), cobrindo
  validação de versão, opcode, preview, modo e campos reservados.

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

O main-controller ainda não tem cliente de câmera (passo 5 abaixo) — não há
hoje quem envie `NB_LINK_MSG_CAMERA_COMMAND` em runtime. A prova ponta a ponta
real (main envia comando → head responde evento) fica para a próxima
sub-etapa, junto com a primeira validação de bancada com o enlace conectado.

## Ordem de implementação

1. ~~Definir contrato semântico de câmera (DM4.1)~~ — `FEITO`.
2. ~~Round trip semântico no head, sem hardware (DM4.2)~~ — `FEITO`.
3. Criar `nb_head_camera_hal` no head-controller, exclusivo dos pinos DVP da
   Freenove, espelhando a separação física já usada por `nb_head_display_hal`.
4. Portar `camera_hal_init/capture/release` para o head; manter a mesma
   interface de frame (`nb_camera_frame_t`) internamente ao componente.
5. Estender `nb_head_camera_service` para acionar o HAL real (hoje só
   responde semântica sem hardware) e manter o preview local (renderizado no
   próprio display do head, sem depender do main).
6. Adicionar cliente no main: solicita snapshot sob demanda, recebe metadados
   pelo `CONTROL`, e busca os bytes JPEG pelo canal `BULK` apenas quando o
   server/bridge realmente precisar (presença, reconhecimento).
7. Remover `camera_hal.c`/`camera_service` do main e mover a responsabilidade
   de preview/overlay de bbox para o head (`vision_preview_service` deixa de
   desenhar localmente; bbox passa a ser parâmetro do estado visual remoto via
   `visual_state_facade`, análogo aos overlays de DM2).
8. Validar headroom de PSRAM no head com câmera + display simultâneos (mínimo
   de 300 KB livres, conforme CLAUDE.md).
9. Só depois remover qualquer caminho de câmera do main em DM6.

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

## Fora do corte DM4.1/DM4.2

- driver físico DVP no head (`nb_head_camera_hal`);
- preview local no head;
- transferência BULK de JPEG;
- cliente de câmera no main (quem envia `NB_LINK_MSG_CAMERA_COMMAND` em
  runtime);
- remoção de `camera_hal`/`camera_service` do main.
