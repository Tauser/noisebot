# DM2.6 — Inventário visual congelado

> Saída obrigatória do gate DM2.6 (`docs/DUAL_MCU_MIGRATION_ROADMAP.md`):
> matriz "feature → produtor main → representação no contrato → renderer
> head → teste → status". Este corte só documenta o estado atual — nenhum
> comportamento foi alterado. Depois deste congelamento, qualquer feature
> visual nova entra primeiro nesta matriz, sem criar subfase.

Data do levantamento: 2026-06-21. Fonte: leitura direta do código atual
(main e head), não memória/suposição.

---

## 1. Expressões (`nb_expression_t`)

| Expressão | Produtor (main) | Contrato hoje | Renderer head | Status |
| --- | --- | --- | --- | --- |
| `NEUTRAL`..`ANGRY` (10, `NB_EXPR_*`) | `expression_service.cpp` (`NB_EXPRESSIONS[]`, parâmetros completos por expressão: abertura, squint, curvas, cor, assimetria) | `nb_display_command_t.expression` — 1 byte, ID estático 0-9 (a forma completa não precisa atravessar o link porque já existe espelhada no head) | `nb_head_emo_renderer.cpp`: `kExpressions[expression]` | **Ported e fiel** — correção 2026-06-21: a entrada anterior desta linha ("10 faces aproximadas") estava errada. Os 19 floats das 10 expressões em `kExpressions[]` são idênticos, bit a bit, aos de `NB_EXPRESSIONS[]` (squint, curvas, assimetria de cada canto, tudo presente) — conferido campo a campo nas 10 expressões durante DM2.8. |

Gate de paridade real (capturas lado a lado, golden scenes, interpolação) é
DM2.8.

## 2. Estados (`NB_STATE_*`, 11 total)

| Estado | Produtor (main) | Contrato hoje | Renderer head | Status |
| --- | --- | --- | --- | --- |
| `BOOT_UP`, `IDLE`, `ATTENTIVE`, `RESPONDING`, `TOUCH_REACTING`, `SLEEPING`, `ERROR`, `SAFE_MODE`, `MEDITATION`, `SILENT_COMPANY`, `MAINTENANCE` | `state_machine.c` — autoridade exclusiva do estado | Nenhuma — estado nunca é enviado como tal; só seus efeitos indiretos (expressão/overlay escolhidos por outros serviços em reação ao estado) | Não modelado — head não sabe em que estado a main está | Não ported (DM2.10, `BLOQUEADO`) |

## 3. Blink (`expression_service.cpp`)

| Feature | Produtor | Contrato hoje | Renderer head | Status |
| --- | --- | --- | --- | --- |
| Blink simples (timing Poisson, `poisson_blink_delay_us`) | `expression_service.cpp` (`blink_update`, `s_next_blink_us`) | Nenhum campo de fase de blink no `nb_display_command_t` | Nenhum — `kExpressions[]` tem `open_l/open_r` fixo por expressão, sem timer próprio | **Não ported** |
| Duplo blink (~12% chance, 180-380ms depois) | idem (`s_double_blink_pending`) | — | — | Não ported |
| Blink assimétrico (olho atrasado sorteado) | idem (`blink_trigger_bilateral`, `late_eye`) | — | — | Não ported |
| Blink bar (substitui olhos por barra acima de `blink_ph` threshold) | idem (`BLINK_BAR_PH_THRESH`) | — | — | Não ported |
| Preservação de expressão durante blink | idem (`blink_update` não reseta expressão base) | — | — | Não ported |

Todo blink é, hoje, comportamento **exclusivo do renderer local legado** em
main (fallback, sem display físico ativo no perfil Waveshare-alvo). Migrar
isso é o núcleo de DM2.9 (`BLOQUEADO`).

## 4. Gaze (`gaze_service.c`)

| Feature | Produtor | Contrato hoje | Renderer head | Status |
| --- | --- | --- | --- | --- |
| Target (`gaze_service_set_target`) | `gaze_service.c` | `gaze_x_milli`/`gaze_y_milli` (±1000) | Offset aplicado a `left_x/y`, `right_x/y` em `nb_head_emo_draw` | **Ported** (posição final, sem timing) |
| Glance (`gaze_service_glance`, hold_ms) | idem | — (main resolve a posição final antes de publicar; head não sabe que é um glance temporário) | — | Parcial — efeito visual chega, semântica de "glance temporário" não |
| Anchor (`gaze_service_set_anchor`) | idem | — | — | Não ported como conceito; só o resultado final de posição |
| Drift, saccade, tilt sustentado | catálogo em `IDLE_REFERENCE.md` §3-4, produzido por combinação de `gaze_service` + `idle_service` | Nenhum campo (sem velocidade, sem tilt) | Nenhum | Não ported (DM2.9) |

## 5. Animações de estado (wake, sleeping, listening, speaking, touch, erro)

| Animação | Produtor (main) | Contrato hoje | Renderer head | Status |
| --- | --- | --- | --- | --- |
| Listening | `conductor`/`visual_state_facade_set_overlay(LISTENING)` | `overlay_flags` bit 0 | `nb_head_emo_renderer` desenha o ícone do overlay | **Ported e confirmado visualmente** (DM2.4, bring-up 2026-06-20) |
| Speaking | idem (`SPEAKING`) | bit 1 | idem | Ported e confirmado |
| Sleeping | idem (`SLEEPING`) | bit 2 | idem | Ported e confirmado |
| Heart | idem (`HEART`) | bit 4 | idem | Ported e confirmado |
| Alert/erro | idem (`ALERT`) | bit 3 | idem (ícone existe) | Ported, **não confirmado visualmente** ainda |
| Blush | idem (`BLUSH`) | bit 5 | idem | Ported, não confirmado |
| Message | idem (`MESSAGE`) | bit 6 | idem | Ported, não confirmado |
| Timer | idem (`TIMER`) | bit 7 | idem | Ported, não confirmado |
| Wake/greeting (sequência completa, `IDLE_REFERENCE.md` §1.1 e §6) | `wake_service` + `idle_service` + `expression_service` (timeline de frames) | Nenhum — é uma timeline, não cabe em overlay flag estático | Nenhum | Não ported (DM2.9) |
| Touch reacting | `touch_semantic_service` → `behavior_engine` → expressão/overlay transitório | Mesmo caminho de expressão (1 byte) | — | Parcial, mesma limitação de §1 |

## 6. Overlays — `ui_overlay_service` (status rail local: INFO/SUCCESS/WARNING/ERROR, toast, ícones persistentes)

| Feature | Produtor | Contrato hoje | Renderer head | Status |
| --- | --- | --- | --- | --- |
| Status rail (`NB_UI_OVERLAY_INFO/SUCCESS/WARNING/ERROR`) | `ui_overlay_service.cpp` | Nenhum — é inteiramente local ao renderer legado do main, não existe no `nb_display_protocol.h` | Nenhum | Não ported — nem desenhado ainda (DM2.11, `BLOQUEADO`) |
| Toast, texto limitado, ícones persistentes | idem | — | — | Não ported |

Importante: não confundir com os 8 `NB_DISPLAY_OVERLAY_*` da seção 5 (esses
já atravessam o link); o status rail é um sistema visual **separado**, ainda
100% preso ao renderer legado do main.

## 7. Visão (bbox/preview)

Marcado explicitamente no roadmap como dependência futura de **DM4**, fora
do escopo de congelamento deste corte — não detalhado aqui.

---

## 8. Resumo por status

| Status | Features |
| --- | --- |
| **Ported e confirmado** | 4 overlays (listening/speaking/sleeping/heart), gaze (posição final) |
| **Ported, não confirmado** | 4 overlays (alert/blush/message/timer) |
| **Parcial** (efeito chega, semântica/timing não) | Glance, touch reacting |
| **Não ported** | Blink (todas as variantes), drift/saccade/tilt, estados (`NB_STATE_*`), wake/greeting timeline, status rail/toast |

## 9. Implicação para a ordem do roadmap

O gargalo real não é o renderer do head (já desenha 10 expressões + gaze +
8 overlays, DM2.1-DM2.5 `FEITO`) — é o **contrato** (`nb_display_command_t`
v1, 16 bytes, sem campo de animação/timing). Confirma o motivo de DM2.7
("contrato visual v2 modular") ser pré-requisito de DM2.8 (paridade de
faces) e DM2.9 (motor de animação): sem campos novos no contrato, blink e
gaze dinâmico não têm como chegar ao head, independente de quanto o
renderer evolua.
