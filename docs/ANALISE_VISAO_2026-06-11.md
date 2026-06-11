# NoiseBot — Análise Criteriosa do Subsistema de Visão

**Data:** 2026-06-11 · **Escopo:** pipeline de visão fim-a-fim (OV2640 240×240 → server → gaze/face_box → dashboard).
**Base de evidência:** `face_loop.py`, `analysis.py`, `client.py`, `app.py` (wiring), `dashboard.py`, `vision_service.c` (heurística firmware) — todos lidos na íntegra ou em trecho relevante.

Estrutura: §1 análise crítica da abordagem atual · §2 alternativas comparadas · §3 feed no dashboard · §4 proposta from-scratch · Anexo: findings estruturados V-01…V-08.

---

## 1. A abordagem atual (Haar @ 240×240, poll 2 s) é viável?

**Veredito curto: não para gaze; marginal para presença. O "nunca funcionou" não é azar — é o resultado esperado da combinação escolhida.**

### 1.1 Geometria: o que 240×240 enxerga

Com a lente padrão da OV2640 (FOV ≈ 65–68° horizontal; o modo 240×240 ainda recorta o sensor, então o FOV efetivo pode ser menor), uma cabeça humana (~15 cm de largura) projeta aproximadamente:

| Distância do robô | Largura do rosto na imagem (estimativa) |
|---|---|
| 40 cm | ~65–75 px |
| 60 cm | ~42–50 px |
| 80 cm | ~32–38 px |
| 100 cm | ~25–30 px |

Com `minSize=(36,36)` (`analysis.py:99`), o alcance útil termina por volta de **70–80 cm**. Pessoa recostada na cadeira (80–110 cm é postura típica de mesa) fica **abaixo do limiar mínimo de detecção**. Esses números são estimativas geométricas — valem como ordem de grandeza, não como medida; o experimento do §4.5 calibra os reais.

### 1.2 Por que o Haar especificamente falha neste cenário

O `haarcascade_frontalface_default` é um detector de 2001, treinado para rostos **frontais, eretos, bem iluminados**, em janela base de 24×24. As condições da mesa violam quase todas as premissas:

- **Pose:** usuário olha para o monitor, não para o robô — yaw de 20–45° é o caso comum, e o cascade frontal degrada fortemente acima de ~20° de yaw e ~15° de rotação no plano. Perfil não é detectado, ponto.
- **Iluminação de escritório:** janela atrás do usuário (backlight) ou rosto iluminado pelo monitor (luz azulada, baixa intensidade, alto contraste lateral). Haar trabalha sobre diferenças de intensidade tipo features retangulares — backlight inverte exatamente os padrões claro/escuro que ele espera (testa clara/olhos escuros).
- **Sensor + JPEG:** a OV2640 é um sensor de 2003 com SNR pobre em <300 lux; a compressão JPEG em 240×240 cria blocos de 8×8 px — num rosto de 40 px, cada bloco é 20% do rosto. Os micro-gradientes que o Haar usa são destruídos primeiro pelo ruído, depois pela quantização.
- **`minNeighbors=4` em imagem pequena** (`analysis.py:98`): com rosto de 36–50 px há pouquíssimas janelas de detecção sobrepostas possíveis; exigir 4 vizinhos derruba o recall justamente na faixa marginal. (Reduzir para 2 aumentaria recall — e os falsos positivos, que em Haar já são altos.)
- Detalhe de implementação que agrava: o cascade é **recarregado do disco a cada frame** (`analysis.py:83–84`, `CascadeClassifier` dentro de `analyze_jpeg`) — não causa a falha, mas desperdiça ~10–30 ms/tick e indica que o caminho nunca foi profilado.

### 1.3 Por que "nunca funcionou" sem ninguém perceber

Três falhas independentes se mascaram mutuamente — qualquer uma sozinha já produziria o sintoma:

1. `import cv2` dentro da função com fallback silencioso (`analysis.py:57–68`): sem opencv no env (que o próprio prompt confirma não ser garantido), retorna `detector_available=False` **sem nenhum log** — e `face_loop._tick()` interpreta como "sem rosto" e segue (`face_loop.py:78–80`).
2. Wiring do adapter por task de injeção (`app.py:109–117`, `_wire_face_loop_adapter_on_connect`): se a task morrer ou a conexão atrasar, `_send_gaze`/`_send_face_box` retornam silenciosamente (`face_loop.py:89–90, 97–98`).
3. **Todos** os erros do loop são `log.debug` (`face_loop.py:67, 94, 102, 108`) — invisíveis no nível INFO de produção.

Além disso, `_tick()` usa `_dummy_observation()` com 240×240 fixo (`face_loop.py:75, 112–119`) em vez de chamar `observe()` real — as dimensões hardcoded (`:83–84`) e a observação falsa significam que mesmo funcionando, o sistema quebraria silenciosamente no dia em que o experimento 640×480 (mencionado em `client.py:86–96`) virasse padrão.

### 1.4 Poll de 2 s

Para **presença** ("tem alguém aí?"), 2 s é adequado — presença muda em escala de dezenas de segundos.
Para **gaze**, 2 s é perceptivelmente quebrado: a expectativa social de contato visual responde em 200–400 ms. Um robô que vira os olhos 2 s depois de você se mover não parece "olhar para você"; parece olhar para onde você **estava** — o que é mais estranho que não olhar. Gaze convincente precisa de ~2–5 Hz de atualização de alvo **+ suavização contínua no firmware** (o `gaze_service` já existe para interpolar — o alvo pode chegar devagar se o movimento entre alvos for suave).

---

## 2. Existe solução melhor? Comparação

### 2.1 Detectores no servidor (PC/Mac, CPU)

| Critério | Haar (atual) | **YuNet (OpenCV ≥4.5.4)** | MediaPipe BlazeFace | dlib HOG | SCRFD/RetinaFace ONNX |
|---|---|---|---|---|---|
| Modelo | XML ~900 KB | **ONNX ~345 KB (int8 ~100 KB)** | ~250 KB + runtime | embutido | 3–17 MB |
| Dependência nova | nenhuma | **nenhuma** (mesmo `opencv-python-headless` já declarado no extra `vision`) | mediapipe (~60–100 MB, protobuf etc.) | dlib (compilação C++) | onnxruntime (~40 MB) |
| Rosto mínimo | ~36 px (config atual) | **~10–12 px** | ~20 px | ~80 px (!) | ~10 px |
| Pose (yaw) | frontal só | **boa até ~±60–90°** | boa | frontal | excelente |
| Backlight/ruído | ruim | **razoável–boa** (CNN treinada em WIDER Face) | boa | ruim | excelente |
| Latência @240² CPU | 15–40 ms | **2–8 ms** | 5–15 ms | 80–200 ms | 20–60 ms |
| Landmarks | não | **sim (olhos, nariz, boca)** — gaze pode mirar os olhos | sim | não | sim |
| Integração | `CascadeClassifier` | **`cv2.FaceDetectorYN`** — mesma API OpenCV | API própria + ciclo de vida | API própria | código de pós-processamento próprio |

**Recomendação: YuNet.** É estritamente superior ao Haar em todos os eixos relevantes, custa zero dependência nova (o extra `vision` do `pyproject.toml` já pede `opencv-python-headless>=4.8`, que inclui `FaceDetectorYN`), o modelo é pequeno o suficiente para ser versionado no repo (`server/resource/models/`), e detecta rostos de 12 px — o que estende o alcance útil para ~2 m mesmo em 240×240 e dá margem para `minSize` efetivo conservador. MediaPipe é tecnicamente equivalente mas o custo de dependência viola o espírito "sem dependências pesadas novas". dlib HOG é eliminado pelo rosto mínimo de ~80 px (inútil em 240²). SCRFD só se um dia precisar de qualidade máxima — não precisa.

### 2.2 Servidor vs. firmware

O ESP32-S3 **consegue** rodar detecção de rosto (esp-dl `human_face_detect`, two-stage MSR+MNP, usado no esp-who; poucos fps em 240×240). Mas neste produto é a escolha errada:

- **Orçamento de PSRAM:** a regra do projeto exige >300 KB livres; os modelos + tensores intermediários do esp-dl consomem na ordem de 1–2 MB de PSRAM durante a inferência.
- **Orçamento de CPU:** core 0 já carrega WakeNet/AFE (wake word) e o pipeline de áudio de 16 ms; core 1 carrega render + push de display. Inferência de visão criaria contenção exatamente nos dois recursos mais disputados.
- **Filosofia do produto:** visão **já é** uma conveniência dependente do servidor (decisão registrada no ROADMAP — "preview e reconhecimento de face implementados via server"). Offline, o robô mantém presença heurística e perde gaze — degradação aceitável e já desenhada.
- **Latência:** captura JPEG → WiFi → YuNet no PC → MSG_GAZE de volta fica em ~80–150 ms por ciclo — abaixo do necessário para gaze convincente com suavização no firmware.

**Recomendação: detecção permanece no servidor.** Reavaliar firmware-side apenas se um dia o produto precisar de gaze sem servidor (e aí com câmera dedicada de mais luz, não como patch).

### 2.3 A heurística do firmware já basta para presença?

Sim — e essa é a simplificação mais valiosa disponível. O `vision_service.c` calcula um `presence_score` por motion + contraste + faixa de luma (`vision_service.c:92–110`) que **funciona hoje**. Presença ("alguém à frente") e gaze ("onde está o rosto") são problemas diferentes com requisitos diferentes:

- **Presença:** heurística embarcada, sempre ativa, custo ~zero, funciona no escuro parcial e de costas. Já existe.
- **Gaze:** detecção de rosto no servidor, **somente quando necessária**.

### 2.4 Cadência: event-driven em vez de 2 s fixo

O poll fixo de 2 s é o pior dos dois mundos: lento demais para gaze, e ainda assim acorda a câmera o tempo todo (contenção de I/O com o preview de 5 fps e gasto de sessão). A forma certa é uma **máquina de estados orientada pela presença**:

```
IDLE    — detecção desligada; consome presence_score do firmware (já vem no
          /api/vision/observe). Custo de visão: zero.
ACQUIRE — presença provável (score alto por N amostras) OU estado do robô pede
          gaze (ATTENTIVE/RESPONDING): rajada de 3 Hz por até 5 s para achar o rosto.
TRACK   — rosto encontrado: 1–2 Hz + suavização EMA do centro; envia MSG_GAZE
          a cada mudança relevante (> ~5% da imagem) e MSG_FACE_BOX a cada tick.
LOST    — K misses consecutivos: envia face_box vazio, volta a IDLE.
```

Isso entrega latência de aquisição < 1 s quando importa, custo zero quando não há ninguém, e gaze fluido via interpolação do `gaze_service` entre alvos de 1–2 Hz.

---

## 3. Feed de câmera no dashboard

Dado aiohttp + HTML puro + objetivo "ver o que o robô vê":

| Opção | Como | Prós | Contras |
|---|---|---|---|
| **`<img>` com refresh por JS** | `img.src = '/api/vision/snapshot?t=' + Date.now()` num `setInterval` de 500–1000 ms | **Zero mudança no servidor** (endpoint já existe e funciona como proxy); cabe no padrão `fetch()`/polling do dashboard; falha de um frame não derruba nada; para de consumir quando a aba fecha | 1–2 fps, não é "vídeo" |
| MJPEG (`multipart/x-mixed-replace`) | handler aiohttp com `StreamResponse` escrevendo `--frame\r\nContent-Type: image/jpeg` em loop (~25 linhas) | Fluido, `<img>` nativo sem JS | Conexão longa presa por aba aberta; precisa gerenciar sessão da câmera no firmware (quem fecha?); interage mal com o `session_close` existente; mais modos de falha |
| WebSocket + blob | — | nenhum relevante aqui | complexidade sem ganho para o objetivo |

**Recomendação: `<img>` com refresh de 1 s** (cache-buster no query string + `Cache-Control: no-store` no proxy). É a única opção que custa ~15 linhas de HTML/JS e zero no servidor. Complementos baratos no mesmo card: idade do último frame ("há 2 s"), estado do pipeline de visão (IDLE/ACQUIRE/TRACK), `detector_available`, `adapter_connected`, e um `<canvas>` sobreposto desenhando o último face_box (os dados já trafegam — hoje ninguém os mostra). Se um dia quiser fluidez, MJPEG é o passo seguinte natural — mas só depois do básico existir.

---

## 4. Proposta from-scratch

Como eu construiria hoje, dado o hardware e a stack — não como patch:

### 4.1 Divisão de responsabilidades

```
FIRMWARE (mantém o que já funciona)
  camera_service          JPEG 240×240 sob demanda (sem mudança)
  vision_service          presença heurística sempre-ativa (sem mudança) —
                          é o "sensor de presença" oficial do produto
  gaze_service            ÚNICO dono da suavização: recebe alvos esparsos
                          (1–3 Hz) e interpola com easing próprio
  ui_overlay/preview      como está

SERVIDOR (substitui FaceLoop + analysis por um componente)
  vision_pipeline.py      máquina de estados IDLE→ACQUIRE→TRACK→LOST (§2.4)
                          + YuNet carregado UMA vez no __init__
                          + normalização usando width/height da observação real
                          + contadores/última detecção no StatusStore

DASHBOARD
  card "Visão"            <img> 1 Hz + canvas com face_box + estado do pipeline
```

### 4.2 Decisões e trade-offs explícitos

1. **YuNet no servidor, nada no firmware.** Troca: dependência do PC para gaze (aceito — visão já é conveniência por decisão de produto) em troca de preservar PSRAM/CPU do S3 e usar um detector 20 anos mais novo que o Haar. Modelo versionado em `server/resource/models/face_detection_yunet_2023mar.onnx` (~345 KB) — sem download em runtime.
2. **Visão como feature explícita que falha alto, não como degradação silenciosa.** `NOISEBOT_VISION=1` ⇒ `import cv2` no **startup** do pipeline; falha de import = erro de boot logado em ERROR com instrução (`pip install -e .[vision]`) e card do dashboard em vermelho — nunca mais `detector_available=False` por tick silencioso. Esse é o conserto estrutural do P1/P2: a classe de bug "parece funcionar mas não faz nada" deixa de ser expressável.
3. **Adapter por pull, não por injeção.** O `app.py` já tem `_get_adapter()` (`app.py` ~L79); o pipeline chama isso a cada envio em vez de depender da task `_wire_face_loop_adapter_on_connect` injetar uma referência. A categoria de falha P3 (wiring nunca acontece) deixa de existir; a task de wiring é deletada.
4. **Dimensões sempre da observação real.** Um `observe()` por transição IDLE→ACQUIRE (não por tick) fixa width/height da sessão; `_norm_coord(c, obs.width)`. Conserta P4 e fica correto para o experimento 640×480 documentado em `client.py:86–96`.
5. **Cadência adaptativa orientada pela heurística embarcada.** Custo zero em mesa vazia; <1 s para adquirir; 1–2 Hz em tracking com EMA (ex.: α=0,4) + zona morta de ~5% para não mandar micro-gaze. Trade-off: o robô não "percebe" rosto de alguém imóvel que entrou sem disparar motion — mitigado por um tick de ACQUIRE oportunista a cada ~30 s quando luma indica ambiente utilizável.
6. **Histerese de detecção:** rosto confirmado após 2 hits consecutivos; perdido após 3 misses. Mata o flicker de gaze que detector nenhum elimina sozinho.
7. **Observabilidade como requisito, não acessório:** contadores por estado (ticks, detecções, envios, erros de captura/envio), idade da última detecção e estado do adapter expostos em `/api/vision/pipeline/status` e no card. Um `log.info` por transição de estado; `log.debug` por tick.
8. **O que eu deliberadamente NÃO faria agora:** reconhecimento/identificação no caminho do gaze (caro e é outra feature — fica no fluxo de enrollment existente via Ollama); MJPEG (depois, se doer); detecção embarcada (§2.2); aumentar resolução da câmera como pré-requisito (YuNet em 240² já resolve o caso de mesa — só reavaliar se o alcance medido ficar < 1,2 m).

### 4.3 Por onde começar (ordem de menor risco)

1. Card de visão no dashboard com `<img>` 1 Hz — dá **olhos ao operador antes de mexer no detector** (inverte o erro histórico: o monitoramento que nunca existiu passa a existir primeiro).
2. Trocar Haar→YuNet dentro de `analyze_jpeg` mantendo a interface `VisionAnalysis` (mudança local; testes de golden-JPEG em `tests/`).
3. Substituir FaceLoop pela máquina de estados (pull de adapter, observação real, logs/contadores).
4. Calibração com protocolo simples: 3 distâncias (50/80/120 cm) × 3 iluminações (dia/luminária/só monitor) × 3 poses (frontal/30°/perfil), 20 amostras cada, registrando recall — vira tabela no `docs/` e define os defaults finais de cadência/histerese.

---

## Anexo — Findings estruturados (padrão da casa)

| ID | Achado | Evidência | Sev. | Categoria |
|----|--------|-----------|------|-----------|
| V-01 | Haar frontal default é inadequado para o cenário (pose/backlight/ruído/JPEG 240²) | `analysis.py:83–101`; §1.2 | P1 | erro real (de escolha técnica) |
| V-02 | Falha silenciosa tripla: import dentro da função, erros em `log.debug`, adapter opcional | `analysis.py:57–68`; `face_loop.py:67,89–102` | P1 | erro real |
| V-03 | Wiring do adapter por task de injeção pode nunca ocorrer | `app.py:109–117` | P1 | erro real |
| V-04 | Observação dummy + dimensões 240 hardcoded (2 lugares) | `face_loop.py:75,83–84,112–119` | P2 | funciona mas errado |
| V-05 | Cascade recarregado do disco a cada frame | `analysis.py:83–84` | P3 | funciona mas errado |
| V-06 | Poll fixo de 2 s: lento para gaze, desperdício para presença | `face_loop.py:22` | P2 | funciona mas errado |
| V-07 | Dashboard sem nenhuma seção de câmera apesar do endpoint pronto | `dashboard.py` (grep image/snapshot vazio); `contract.py` | P2 | inexistente |
| V-08 | Heurística de presença embarcada funcional e subaproveitada como gate | `vision_service.c:92–110`; `client.py:127–143` | — | acerto (evolutivo: usar como trigger do pipeline) |

**Menor caminho seguro consolidado:** (1) card `<img>` no dashboard; (2) YuNet em `analyze_jpeg` com import no startup e falha alta; (3) pull de adapter + observação real; (4) máquina de estados de cadência. Cada passo é independente, testável e reversível.

---

## 5. Adendo — Viabilidade de resolução maior

**Antes de tudo: a premissa do adendo está desatualizada.** "240×240 é o único modo funcional confirmado" deixou de ser verdade em 2026-06-07 — o próprio `docs/CAMERA_INTEGRATION.md` registra experimentos em hardware real que mudam o quadro. O resumo do que esta placa **já provou**:

| Modo (Kconfig `CAMERA_OV2640_DVP_*`) | Resultado em hardware | Evidência |
|---|---|---|
| `YUV422_240X240_25FPS` (atual) | **Funcional e validado em soak** — 1800 s, 61/61 capturas, `max_capture_ms=174`, `min_psram_free=7.13 MB`, `min_fps=24` no render | CAMERA_INTEGRATION.md §"Current diagnostic finding" |
| `JPEG_640X480_25FPS` | **NÃO-funcional** — negociação OK, mas captura retorna `bytesused=0` em 10–12 buffers consecutivos; num caso devolveu tamanho-lixo (~4 GB) que o camera_service aceitou | idem, experimento 2026-06-07 |
| `JPEG_320X240_50FPS` | **NÃO-funcional** — mesma assinatura de falha do JPEG 640×480 | idem |
| `YUV422_640X480_6FPS` | **FUNCIONAL** — `effective_width=640`, frame YUV de 614.400 bytes, JPEG válido de ~80 KB na saída (encode em software), `fail_count=0`; perfil pronto em `sdkconfig.experiment.ov2640-yuv422-640x480.defaults` | idem, experimento do perfil |

Três conclusões estruturais que o documento já provou (com rastreio até o fonte do driver vendorizado):

1. **Resolução é decisão de compile-time, ponto.** O sensor suporta 12 modos (até 1600×1200), mas o pareamento `esp_video` 1.3.1 + `esp_cam_sensor` não expõe **nenhum** caminho de runtime para trocar: `VIDIOC_S_FMT`/`TRY_FMT` recusam qualquer tamanho não-nativo com `EINVAL`, e `VIDIOC_S_SENSOR_FMT` exige ponteiro interno do driver inacessível à aplicação. 
2. **O caminho JPEG nativo em resolução alta está quebrado nesta placa/driver** — nas duas resoluções testadas, com a mesma assinatura. Não é budget de PSRAM nem struct mal inicializado (hipótese descartada no doc); é o caminho DVP/JPEG do pareamento vendorizado.
3. **O caminho YUV422 + encode JPEG em software funciona em VGA.** O custo se move do driver para a CPU.

### 5.1 Resoluções realistas para este pipeline

Dado o que está provado, o cardápio real tem **duas opções**, não doze:

| | 240×240 YUV @ 25 fps (atual) | 640×480 YUV @ 6 fps (provado) |
|---|---|---|
| Frame bruto | 115 KB | 614 KB |
| Buffers de driver (mmap ×N) | ~230–345 KB | ~1,2–1,8 MB |
| PSRAM livre após captura (medido/projetado) | 7,13 MB medido | ~5,5–6 MB projetado — folga enorme sobre a regra dos 300 KB |
| Latência de captura | ≤174 ms medido | período de frame 167 ms + dequeue → ~200–400 ms (estimativa; **medir**) |
| Encode JPEG software | sobre 115 KB | sobre 614 KB — ~4–5× o tempo de CPU por snapshot |
| Teto de cadência | 25 fps (preview 5 fps confortável) | 6 fps no sensor — preview e visão disputam o mesmo teto |
| Risco para render 45 fps / áudio | validado em soak (min_fps 24 medido no perfil de teste) | desconhecido — é exatamente o que o soak existente mede |

QVGA/HVGA intermediários **não são opções reais**: 320×240 só existe no caminho JPEG (quebrado), e não há modo YUV intermediário na tabela do sensor vendorizado. A escolha é binária.

### 5.2 Impacto na detecção de rosto (50–80 cm)

A pergunta certa não é "quanto a resolução melhora a detecção", e sim "a partir de que tamanho de rosto em pixels o detector é confiável":

| Detector | Rosto mínimo confiável | @240² (rosto 32–50 px a 50–80 cm) | @640×480 (rosto 85–135 px) |
|---|---|---|---|
| Haar default | ~36 px (config) e frágil | marginal — é o status quo que falhou | funcionaria razoável (frontal) |
| **YuNet** | ~10–12 px | **confortável** — 3–4× acima do mínimo | sobra absurda; alcance vai a ~3 m |
| MediaPipe | ~20 px | confortável | idem |

**Conclusão central: para o objetivo declarado (presença + gaze a 50–80 cm), trocar o detector resolve; trocar a resolução não é pré-requisito.** 240² + YuNet cobre a mesa com margem. Resolução maior compra três coisas que o gaze não precisa: alcance além de ~1,2 m, qualidade de **reconhecimento** (embeddings faciais pedem rosto ≥112 px — em 240² isso exige <45 cm; em VGA, ~1,2 m), e descrição de cena pelo LLM com mais detalhe.

### 5.3 "Testar via HTTP sem reflash" — não existe

A hipótese do adendo já foi refutada pelo próprio projeto: `/api/camera/mode` **não muda resolução** — desde a correção de 2026-06-07, `safe`/`better` só selecionam perfil de threshold de memória pré-init (documentado em CAMERA_INTEGRATION.md e no docstring de `client.py:86–96`). O único caminho para outra resolução é reflash com outro `choice` de Kconfig. O equivalente prático de "testar antes de comprometer" **já existe e é melhor que HTTP**: o perfil `sdkconfig.experiment.ov2640-yuv422-640x480.defaults` + o harness de soak (`vision_soak.py`) — flash do perfil experimental, rodar o soak de 30 min com os critérios de aceitação já escritos (bridge estável, sem `wifi:mem fail`, PSRAM >300 KB, render FPS, dropout de áudio), e o `sdkconfig.defaults` de produção não é tocado até os números saírem.

### 5.4 Trade-off explícito e recomendação

```
Ganho de ir a 640×480 YUV/6fps:  reconhecimento facial viável a distância de mesa;
                                  alcance de detecção ~3 m; cena mais rica p/ LLM.
Custo:                            teto de 6 fps (preview e visão disputam);
                                  4–5× CPU no encode JPEG por snapshot;
                                  latência de captura ~2× (a medir);
                                  1–1,5 MB a mais de PSRAM (irrelevante p/ regra 300 KB);
                                  risco de regressão render/áudio = desconhecido até o soak.
Ganho de ficar em 240²:           25 fps de teto, perfil já validado em soak de 30 min,
                                  zero risco novo — e o gaze funciona com YuNet.
```

**Recomendação em duas fases, sem mudar a conclusão do §4:**

1. **Agora:** manter 240×240 e trocar Haar→YuNet (§2). É a correção que ataca a causa real da falha; resolução não está no caminho crítico do gaze.
2. **Quando reconhecimento/alcance entrar no roadmap** (perfil de usuário por rosto a distância de mesa): rodar o experimento que o repo já deixou pronto — flash do perfil `YUV422_640X480_6FPS`, soak de 30 min com os critérios de aceitação existentes + medição nova de `capture_ms`, tempo de encode JPEG, FPS de render e dropout de áudio sob conversa ativa. Se os números fecharem, esse vira o "perception build"; se não, o reconhecimento fica condicionado a aproximação (<45 cm) em 240².

Item de segurança a registrar independente da decisão (o doc já flagou e merece virar finding): `camera_service` confia no `bytesused` do driver sem validar contra o tamanho do buffer alocado — o experimento JPEG produziu um "sucesso" com tamanho de ~4 GB que só a sanidade da camada HTTP barrou. Validar `0 < bytesused <= buf_size` no HAL (V-09, P2, erro real).

---

## 6. Adendo — Integração câmera ↔ resto do firmware: existe desenho melhor?

### 6.1 O que existe hoje (e o que está certo)

```
camera_hal (L1)            esp_video/V4L2, mmap, sessão explícita
camera_service (L4)        snapshot sob demanda, mutex, hot session c/ hold timer,
                           borrow/release do buffer, encode JPEG em software,
                           diagnóstico de memória antes/durante/depois de cada captura
vision_service (L5)        poll de presença 300 ms (heurística no frame)
vision_preview (L4/5)      task própria de captura a 5 FPS + render layer z=90,
                           face box via NB_EVT_BRIDGE_FACE_BOX (event bus ✓)
web_service                endpoints snapshot/mode/status chamando camera_service direto
```

**Acertos que devem sobreviver a qualquer redesign:** init lazy (câmera nunca no boot), modelo de sessão quente com timeout, política de memória orçada com medição por captura (`s_last_dma_*`/`s_last_psram_*` — instrumentação acima da média), gating "não captura com áudio ocupado" como política documentada, e o preview recebendo face box por evento, não por chamada.

### 6.2 O problema estrutural: três iniciadores de captura, nenhum árbitro

Hoje a câmera tem **três consumidores que iniciam capturas por conta própria**, serializados só por mutex, sem prioridade nem coordenação de cadência:

| Consumidor | Cadência | Conta de ocupação |
|---|---|---|
| `nb_vpreview_task` | 5 FPS (período 200 ms) | com `max_capture_ms=174` medido, **o preview sozinho ocupa até ~87% do tempo de câmera** |
| `vision_service` poll | 300 ms | disputa o resto |
| HTTP snapshot (server: gaze YuNet, enrollment, dashboard) | sob demanda | chega por último, com jitter de até ~1 captura inteira |

Consequências concretas: (a) com preview ligado, o poll de presença e o snapshot do servidor vivem de sobras — exatamente o snapshot que alimentará o gaze; (b) o `borrow/release` de buffer único significa que um handler HTTP lento segurando o borrow bloqueia todos os outros; (c) cada consumidor paga uma captura própria — três capturas do mesmo instante de mundo em vez de uma.

Há ainda um **desperdício de CPU em forma de U**: o pipeline atual faz YUV (sensor) → **encode JPEG em software** (camera_service) → buffer em PSRAM → **decode JPEG** (`drawJpg` na render layer) → RGB565 no display. O preview codifica e decodifica o mesmo frame a 5 FPS sem nenhuma necessidade — o JPEG só é necessário para consumidores HTTP.

### 6.3 Desenho melhor: frame pump único + consumidores leitores

O espelho exato do "frame broker" proposto para o servidor (§3), aplicado dentro do firmware:

```
camera_pump (L4, única task que toca camera_service/HAL)
  - cadência = max(demanda dos consumidores ativos):
      preview ON → 5 Hz · só presença → ~3 Hz · nenhum → 0 (sessão fecha via hold timer)
  - mantém UM frame atual em PSRAM no formato NATIVO (YUV422) + seq number
  - gating de áudio/estado verificado num único lugar
  - V4L2 streaming contínuo (DQBUF/QBUF) enquanto a sessão está quente,
    em vez de captura avulsa por pedido

consumidores (leitores do frame atual, nunca iniciadores):
  vision_service    → heurística direto no plano Y do YUV (luma/motion/contraste
                      ficam MAIS baratos que hoje — sem JPEG no caminho)
  vision_preview    → conversão YUV→RGB565 direto para a render layer
                      (elimina encode+decode JPEG do caminho do display)
  web/snapshot      → encode JPEG on-demand, com cache por seq number
                      (dois GETs do mesmo frame = um encode)
  futuro stream     → MJPEG vira trivial: encode do frame atual a N Hz
```

Ganhos: uma captura por instante atende todos; prioridade vira política do pump em vez de corrida de mutex; o encode JPEG sai do caminho quente do display (a 5 FPS isso é a maior economia de CPU disponível no subsistema); o borrow de buffer único morre (leitores copiam ou leem sob seq-check); o gating de áudio fica em um ponto; e o stream do dashboard (§3) e o pipeline do servidor passam a custar quase zero a mais.

Custo/risco: refactor médio (o camera_service vira backend do pump; preview e vision_service trocam "capturar" por "ler"), e a mudança para streaming contínuo no V4L2 precisa de validação no soak existente (o modelo atual de dequeue avulso é o que está provado). Dá para fazer em dois passos seguros: primeiro o pump com captura avulsa (sem mudar o HAL), depois o streaming contínuo se o soak aprovar.

### 6.4 Veredito parcial (integração)

O desenho atual é correto como **bring-up orçado** — foi o que permitiu integrar a câmera sem derrubar WiFi/SD/áudio, e a instrumentação de memória é exemplar. Mas ele escala mal no exato cenário para onde o produto está indo: preview + presença + gaze do servidor + dashboard simultâneos. A evolução natural é o pump único — que, não por acaso, é a mesma forma da solução do lado do servidor. Câmera passa a ter **um dono e N leitores nos dois lados do sistema**, e a pergunta "quem pode capturar agora?" deixa de existir.

---

## 7. Decisão recomendada — síntese final

Consolidando §§1–6 numa única decisão:

| Dimensão | Escolha | Por quê |
|---|---|---|
| Resolução | **240×240 YUV @ 25 fps** (manter) | único perfil validado em soak; com YuNet cobre gaze a 50–80 cm com folga; VGA 6 fps só se reconhecimento a distância virar requisito (§5.4) |
| Detector | **YuNet no servidor** | zero dependência nova, 345 KB, conserta a causa raiz; detecção embarcada descartada por PSRAM/CPU (§2.2) |
| Cadência | **Event-driven** (IDLE→ACQUIRE 3 Hz→TRACK 1–2 Hz) gateada pela heurística do firmware | custo zero em mesa vazia, aquisição <1 s, gaze suavizado pelo gaze_service (§2.4) |
| Firmware | **camera_pump** em 2 passos: pump com captura avulsa → streaming V4L2 se o soak aprovar | elimina os 3 iniciadores concorrentes e o encode+decode JPEG do display (§6.3) |
| Dashboard | **`<img>` refresh 1 s agora**; MJPEG 5 fps depois do pump (~30 linhas) | menor custo/risco primeiro; o stream vira subproduto do pump (§3) |

**Ordem de execução (cada passo independente e reversível):**

1. Card de visão no dashboard com `<img>` polling — visibilidade antes de mexer no detector.
2. Haar→YuNet em `analyze_jpeg` + import com falha alta no startup + pull de adapter — **é onde o sistema está de fato quebrado; se só um passo for feito, é este.**
3. Máquina de estados de cadência no servidor (substitui FaceLoop).
4. camera_pump no firmware (passo 1: captura avulsa; passo 2: streaming contínuo pós-soak).
5. MJPEG no dashboard (subproduto do passo 4).

Passos 1–3 não tocam o firmware e entregam o gaze funcionando; 4–5 são a evolução estrutural.
