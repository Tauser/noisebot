# IDLE Reference — Análise do Vídeo EMO

Documento de referência para o comportamento de `IDLE` baseado na observação
direta de uma sessão idle do EMO (Living.AI), usado como referência visual.

**Material analisado:** `.codex_tmp/emoidle_frames/emoidle.mp4` — 30.3s a 30fps,
1280×720. Crop da face a 320×300, reanálise programática a 10fps com
métricas por **olho separado** (bounding-box independente para esquerdo e
direito), não apenas centróide agregado.

> **Erratum**: a primeira versão deste documento usou apenas centróide global
> e percentual de pixels cyan. Essa análise pobre concluiu erradamente que o
> EMO em idle "praticamente não muda" — perdendo expressões longas de
> curiosidade, head tilt persistente, e sequências compostas de blink.
> Esta versão usa medidas separadas por olho (bbox L vs R, asymmetry de
> largura/altura/posição) e corrige o diagnóstico.

> **Escopo**: face / olhos / gaze. Pescoço (servo) está fora do escopo até
> Etapa 3.3 (motion_service), pós motion_safety verde.

---

## 1. Timeline Real

Linha do tempo dos eventos identificados, baseada em mosaicos visuais
confirmados + dados quantitativos por olho (largura, altura, posição vertical
do bbox de cada olho separadamente):

### 1.1 WAKE / GREETING (0.0s – 4.5s)

| t (s)     | Estado / Evento                                           |
| --------- | --------------------------------------------------------- |
| 0.0–0.6   | Olhos como linhas horizontais finas `—  —` (h≈46)         |
| 0.7–1.1   | Olhos **TALL** (h≈110, ar 0.83) — formato vertical alto   |
| 1.2–1.9   | Olhos curvados com **assimetria** (L wider) + texto "Hi" embaixo. Forma `⌒⌒` (smile-eyes / olho-fechado feliz). Low fill (0.18–0.22) confirma curva côncava. |
| 2.0–2.3   | **WIDE BAR** `—` blink (h≈17, ar 4.5)                    |
| 2.4–3.6   | Squints (h≈45–49) — olhos abrindo com pálpebra parcial    |
| 3.9–4.3   | **Look-down** + squint inferior (y0=141–165 vs base 102) |
| 4.5+      | Settling para idle baseline                               |

Esta é uma **expressão de saudação** — não é apenas "boot-up". O EMO disparou
um motivo `HAPPY/HI` antes de assentar em idle. Vamos mapeá-la como
microexpressão `GREETING` ou `HAPPY_FLASH`.

### 1.2 IDLE BASELINE (5.0s – 17.0s)

Olhos como retângulos arredondados quase quadrados (w≈74, h≈74, ar≈1.0).

**Mas: head tilt persistente.** O olho esquerdo permanece de 6 a 13 px **mais
baixo** que o direito ao longo de todo o intervalo:

```
t=5–9s:   L_y0 - R_y0 = +6 a +13  (média +8)  ← olho L mais baixo que R
t=8–9s:   +6  (mantém)
```

Isso corresponde a **head tilt para a esquerda** do robô (ou seja, a cabeça
está rotacionada no eixo de roll, com o lado esquerdo mais baixo). Essa
inclinação **não é zero como reportei antes** — é uma postura ativa, sutil
mas consistente, que sugere intenção (o EMO está "ouvindo" / "atento" ao
ambiente).

Em frames individuais a inclinação é discreta; só fica visível quando
medimos a posição vertical de cada olho por bbox.

### 1.3 BLINK COMPOSTO COM LOOK-DOWN (18.0s – 20.0s)

| t (s)   | Estado                                                |
| ------- | ----------------------------------------------------- |
| 17.5    | Idle baseline                                         |
| 18.0    | Eyes descem (y0 +50 px) — **olhar para baixo**        |
| 18.1    | Blink 1 — linha "—" na **parte inferior** da face     |
| 18.3–18.8 | Eyes abrem mas mantêm posição baixa, ~700ms hold    |
| 19.0    | Blink 2 — linha "—" novamente (double blink)         |
| 19.4    | Width asymmetry breve (R wider, dw=-15)              |
| 20.0    | Eyes voltando ao baseline                             |

**Esta não é uma piscada simples nem dois blinks soltos.** É uma sequência
composta:
1. Look-down (descida do gaze + descida da posição vertical dos olhos)
2. Blink rápido (bar)
3. Hold em posição baixa
4. Segundo blink
5. Return ao baseline

Sugere "checking" — como uma pessoa que abaixa os olhos para verificar algo
e pisca durante o gesto.

### 1.4 CURIOSITY 1 (21.5s – 26.0s) — **5 segundos**

| t (s)     | Métrica                                           |
| --------- | ------------------------------------------------- |
| 21.5–21.7 | Look-down breve (preparação)                     |
| 21.8      | Olho **esquerdo** começa a alargar (dw=+20)       |
| 22.0–23.0 | Olho L estabiliza em **w=97 vs R w=74** (dw=+24, ~30% maior) |
| 23.1      | Squint dentro da expressão (h cai 30 px)         |
| 23.2      | Blink (preserva expressão)                        |
| 23.4–25.9 | Mantém olho esquerdo wider                        |
| 26.0      | Volta ao baseline                                 |

**Esta é a expressão central que você apontou e eu perdi.** Olho esquerdo
sustentado em ~30% maior que o direito por **4 segundos contínuos**, com um
blink no meio (que preserva a forma). Equivalente a uma sobrancelha
levantada / olhar interrogativo / "huh?" / curious.

### 1.5 SINGLE BLINK (26.8s)

Squint baixo de ~80ms isolado.

### 1.6 CURIOSITY 2 + HEAD TILT (27.5s – 30.0s)

| t (s)     | Métrica                                           |
| --------- | ------------------------------------------------- |
| 27.5–27.8 | Olho esquerdo começa a alargar novamente          |
| 27.9–28.5 | dw=+24 (mesmo padrão de Curiosity 1)              |
| 28.0+     | Olho **direito** sobe relativamente (vert_asym sugere head tilt) |
| 29.0+     | Asymmetry visível em ambos: width L > width R **e** y0 R > y0 L |
| 30.0      | Volta começa, olhos descem (look-down de novo)    |

Esta segunda curiosidade vem **acompanhada de head tilt** mais explícito que
o baseline. Termina em look-down (preparando próxima ação).

---

## 2. O Que Eu Reportei Errado Antes

| Reportei                         | Realidade                                           |
| -------------------------------- | --------------------------------------------------- |
| "Gaze fixo no centro"            | Gaze fica na vizinhança do centro, mas a cabeça está **inclinada** e há expressões que mudam a forma dos olhos por segundos. |
| "Cabeça estática ±1–2°"          | Tilt persistente (~+8 px de assimetria vertical L↔R, sustentado 12s). |
| "Sem motifs visíveis em IDLE"    | 2 expressões de curiosidade (5s + 3s) + 1 blink composto + 1 wake. |
| "Apenas blink + drift sutil"     | Blinks são compostos (look-down + double-blink + return). |
| "Double-blink ~30%"              | Frequência alta confirmada, mas vem dentro de uma sequência mais rica. |

**Causa do erro**: a primeira análise mediu apenas `cyan_pct` e o centróide
global. Mudanças de **forma** (largura assimétrica, altura assimétrica) e de
**rotação** (tilt entre olhos) ficaram fora da métrica. Ao olhar o vídeo
diretamente em mosaico, fica óbvio.

---

## 3. Catálogo de Comportamentos Observados

Os comportamentos que precisam existir em IDLE para reproduzir o EMO:

| ID                | Descrição                                                     | Duração | Frequência observada |
| ----------------- | ------------------------------------------------------------- | ------- | -------------------- |
| `BASELINE`        | Retângulos arredondados simétricos                            | sempre  | sempre               |
| `HEAD_TILT_HOLD`  | Postura com asym vertical sustentada (~8 px ≈ 2° de roll)     | 5–15 s  | quase contínuo       |
| `SOFT_DRIFT`      | Micro-jitter no gaze, ±0.04 do centro                         | sempre  | sempre               |
| `BLINK_BAR`       | Blink "—" rápido (80–120ms)                                   | ~100 ms | a cada ~7–10 s       |
| `DOUBLE_BLINK`    | Dois `BLINK_BAR` com gap 400–1000ms                           | total ~1.5 s | ~30% dos blinks |
| `LOOK_DOWN_BLINK` | Look-down + blink bar (no fundo da face) + hold ~700ms + 2º blink + return | ~2 s | observado 1× em 30s |
| `CURIOUS_TILT`    | Olho L (ou R) alargando ~30%, sustentado 3–5s, com blink interno preservando forma | 3–5 s | 2× em 30 s |
| `GREETING_HAPPY`  | Eyes sobe → forma `⌒⌒` curva (low fill) → blink-bar → settle | ~3 s | 1× em 30 s (no início) |

### 3.1 Mapeamento para o que já existe no firmware

| Comportamento         | Já existe?                                          | Gap                        |
| --------------------- | --------------------------------------------------- | -------------------------- |
| `BASELINE`            | `NB_EXPR_NEUTRAL` em `expression_service`           | OK                         |
| `HEAD_TILT_HOLD`      | Não — requer assimetria sustentada de y_l/y_r      | **Adicionar**              |
| `SOFT_DRIFT`          | `gaze_service` micro-drift (DRIFT_MAX_R)           | OK (já reduzi para 0.040)  |
| `BLINK_BAR`           | `expression_service` blink bilateral + barra ph≥0.75 | OK                         |
| `DOUBLE_BLINK`        | Lógica DOUBLE_BLINK_THRESH                          | Probabilidade já ajustada (30%) |
| `LOOK_DOWN_BLINK`     | Não existe como sequência                          | **Adicionar como motif**   |
| `CURIOUS_TILT`        | `IDLE_MOTIF_CURIOUS_CHECK` é curto (180–420ms)     | **Estender** ou **novo motif** longo |
| `GREETING_HAPPY`      | `NB_EXPR_HAPPY` existe; falta a sequência composta | Composição via `expression_combo_play()` |

---

## 4. Plano Revisado de Refactor

### 4.1 Reverter mudanças baseadas na análise errada

A primeira passada subestimou muito a vivacidade do EMO em idle. Reverter:

- `BLINK_MEAN_MS = 7500` ⟶ **5000** (era 4200; meio termo: blinks regulares
  mas não tão frequentes quanto antes).
- `BLINK_MIN_MS = 2200` ⟶ **1800** (mantém um pouco mais alto que original,
  mas próximo do real).
- `SACCADE_IDLE_MIN_MS = 30000`, `SACCADE_IDLE_RANGE_MS = 50000` ⟶
  **15000 / 25000** (motif a cada 15–40s, não 30–80s; o vídeo mostra ~2
  motifs longos em 30s, então motifs raros mas não tanto).
- `DOUBLE_BLINK_THRESH = 77 (30%)` ⟶ **manter** (essa eu acertei).
- `DOUBLE_BLINK_MIN_US`, `DOUBLE_BLINK_RNG_US` ⟶ **manter** (gap 400–1000ms).
- `DRIFT_MAX_R = 0.040` ⟶ **manter** (era 0.060; valor atual ainda razoável).

### 4.2 Adicionar motifs novos

Três novos motifs em `idle_service.c`:

1. `IDLE_MOTIF_HEAD_TILT_HOLD`
   - Duração 5–15s.
   - Expression: assimetria vertical entre olhos (`y_l`, `y_r`) de ±0.10.
   - Permite blink dentro (não cancela o tilt).
   - Volta suave para baseline simétrico.

2. `IDLE_MOTIF_LOOK_DOWN_BLINK`
   - Sequência: gaze.y +0.4 → blink bar → hold 700ms → blink bar → gaze 0.
   - Duração total ~2s.
   - Implementável combinando `gaze_service_glance(0, +0.4, 1500)` +
     `expression_combo_play()` com 2 frames de SLEEPY 80ms.

3. `IDLE_MOTIF_CURIOUS_TILT` (substitui ou complementa `CURIOUS_CHECK`)
   - Duração 3–5s.
   - Expression: olho L (ou R, sorteado) com `tl/tr/bl/br` ajustados para ficar
     **wider** + slight raise. Preserva durante blink (blink usa squint
     temporário sem zerar a expressão base).
   - Pode incluir tilt vertical leve.

### 4.3 Distribuição de motifs em IDLE (revisada)

```
LINE_BLINK         15%   (era 50%)
DOUBLE_BLINK       já é probabilidade dentro do blink — não conta como motif
LOOK_DOWN_BLINK    15%   (novo)
CURIOUS_TILT       30%   (novo, principal)
HEAD_TILT_HOLD     20%   (novo)
SIDE_PEEK          10%
VERTICAL_SCAN       5%
CROSS_SCAN          5%
```

Total ≈ 100%. Curiosity é o mais comum porque foi o mais comum no vídeo.

### 4.4 ATTENTIVE permanece como está

A distribuição em ATTENTIVE não muda — modelo "robô prestando atenção", com
side peeks e vertical scans frequentes (5–13s entre motifs).

### 4.5 Greeting / wake-up

Não como motif de idle. Pertence a `wake_service` ou ao `circadian_service`.
Mas a sequência composta (`HAPPY` + look-up + curve + bar blink + settle)
deve ser consultada quando esses serviços evoluírem.

---

## 5. Critérios de Aceitação Revisados

Em IDLE, sessão de 60s contínuos:

- [ ] Pelo menos **uma** expressão sustentada (CURIOUS_TILT ou HEAD_TILT_HOLD).
- [ ] Pelo menos **um** LOOK_DOWN_BLINK ou double blink composto.
- [ ] Pelo menos **2 blink bars** isoladas.
- [ ] Drift contido (≤ 0.04 amplitude).
- [ ] Em nenhum frame os olhos ficam idênticos por mais de 15s contínuos
      (ou seja, sempre alguma micromudança ou tilt presente).
- [ ] Transição IDLE → ATTENTIVE: motifs ATTENTIVE assumem em 5–13s.
- [ ] Transição ATTENTIVE → IDLE: motif corrente termina, próximo em 15–40s.

---

## 6. Apêndice — Sequência WAKE / GREETING (referência futura)

Os primeiros 4 segundos do vídeo mostram uma sequência de saudação rica que
**não é parte do idle**, mas serve como referência de design para o
`wake_service` e o `circadian_service` (transição SLEEPING → IDLE com
contato deliberado, e despertar no DAWN do ciclo circadiano).

### Frames-chave

| t (s)    | Visual                                                            | Estado paramétrico equivalente                              |
| -------- | ----------------------------------------------------------------- | ----------------------------------------------------------- |
| 0.0–0.6  | Linhas horizontais finas `—  —` (olhos fechados)                  | `NB_EXPR_SLEEPY` ou `open=0`                                |
| 0.7–1.1  | Olhos altos `❘  ❘` (ar 0.83, h>110)                               | Aberta vertical — surpresa amigável                          |
| 1.2–1.9  | Curva côncava `⌒⌒` + texto **"Hi"** abaixo                       | Próximo de `NB_EXPR_HAPPY` mas mais elevado, com overlay de boca/texto |
| 2.0–2.3  | Bar blink `—` curto                                               | `NB_EXPR_SLEEPY` 80–100ms                                    |
| 2.4–3.6  | Squints alternados (h≈45–49)                                      | Olhos abrindo lentamente                                     |
| 3.9–4.3  | Look-down + squint inferior (y0=141–165)                          | Gaze.y → +0.4, leve squint                                   |
| 4.5      | Settle baseline                                                   | `NB_EXPR_NEUTRAL` simétrico                                  |

### Características importantes

- **Não é boot-up**. É uma performance de saudação intencional, com timing
  deliberado e mídia (texto "Hi") visível no display.
- **Texto "Hi"** durante a curva `⌒⌒` (~700ms): renderizado abaixo dos olhos,
  centralizado. No NoiseBot pode ser feito via overlay temporário no
  `ui_overlay_service` (já existe; usa render layer próprio).
- **Acentuação assimétrica** breve em 1.3–1.9s (L wider, dw≈+15) — sutil "wink"
  durante a curva.
- **Fim com look-down**: o robô termina olhando para baixo brevemente antes
  de assentar — comunica "vou ficar aqui agora" / "transição completa".

### Implementação proposta (quando wake_service for tocado)

Estrutura como `expression_combo_play()` + overlay de texto:

```
sequence WAKE_GREETING:
  1. expression_play(NB_EXPR_SLEEPY,  600ms, 100ms)     // "—  —"
  2. expression_play(NB_EXPR_SURPRISED, 500ms, 200ms)   // "❘  ❘" altos
  3. expression_play(NB_EXPR_HAPPY,   1200ms, 250ms)    // "⌒⌒"
       + ui_overlay_text("Hi", 1100ms)                   // texto sob olhos
  4. expression_play(NB_EXPR_SLEEPY,    90ms,  35ms)    // bar blink
  5. expression_play(NB_EXPR_HAPPY,    700ms, 150ms)    // "⌒⌒" reabre
  6. gaze_service_glance(0, +0.35, 600)                  // look-down de fechamento
  7. expression_service_set(NB_EXPR_NEUTRAL, 350ms)      // settle
                                                          // → idle_service assume
```

Total ~4.0–4.5s. Não cabe no idle_service; pertence a `wake_service` ou ao
handler de transição SLEEPING → IDLE em `circadian_service`. Documentado
aqui apenas como referência observada.

---

## 7. Status

- ✅ Análise corrigida (este documento).
- ⏳ Reversão dos tunings errados.
- ⏳ Implementação dos 3 motifs novos (HEAD_TILT_HOLD, LOOK_DOWN_BLINK,
     CURIOUS_TILT).

A reversão dos tunings é segura e isolada (apenas `#define`). A implementação
dos novos motifs requer trabalho maior em `idle_service.c` e possivelmente
funções helper em `expression_service.cpp` para suportar tilt sustentado e
expressão assimétrica preservada durante blink.
