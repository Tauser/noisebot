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

## Ordem de implementação

1. ~~Definir contrato semântico de câmera (DM4.1)~~ — `FEITO`.
2. ~~Round trip semântico no head, sem hardware (DM4.2)~~ — `FEITO`.
3. ~~Cliente de câmera no main, sem hardware (DM4.3)~~ — `FEITO`.
4. Criar `nb_head_camera_hal` no head-controller, exclusivo dos pinos DVP da
   Freenove, espelhando a separação física já usada por `nb_head_display_hal`.
5. Portar `camera_hal_init/capture/release` para o head; manter a mesma
   interface de frame (`nb_camera_frame_t`) internamente ao componente.
6. Estender `nb_head_camera_service` para acionar o HAL real (hoje só
   responde semântica sem hardware) e manter o preview local (renderizado no
   próprio display do head, sem depender do main).
7. Wirear `nb_main_link_service_request_camera`/`get_last_camera_event` a um
   consumidor real (presença/reconhecimento) e buscar os bytes JPEG pelo
   canal `BULK` sob demanda.
8. Remover `camera_hal.c`/`camera_service` do main e mover a responsabilidade
   de preview/overlay de bbox para o head (`vision_preview_service` deixa de
   desenhar localmente; bbox passa a ser parâmetro do estado visual remoto via
   `visual_state_facade`, análogo aos overlays de DM2).
9. Validar headroom de PSRAM no head com câmera + display simultâneos (mínimo
   de 300 KB livres, conforme CLAUDE.md).
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

## Fora do corte DM4.1/DM4.2/DM4.3

- driver físico DVP no head (`nb_head_camera_hal`);
- preview local no head;
- transferência BULK de JPEG;
- qualquer consumidor de produto (presença, reconhecimento) chamando o
  cliente de câmera do main em runtime;
- remoção de `camera_hal`/`camera_service` do main.
