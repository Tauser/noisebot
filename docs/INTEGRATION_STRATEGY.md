# NodeBot — Estrategia de Integracao Progressiva

## Principio

> Cada subsistema entra somente depois que a base que o suporta esta validada, observavel e com risco controlado.

A ordem abaixo nao e arbitraria. E derivada de:
- **Risco fisico e eletrico:** componentes sem risco entram antes
- **Valor estrutural:** o que habilita debug e observabilidade entra antes
- **Dependencias tecnicas:** o que e pre-requisito de outro entra antes
- **Pressao de recursos:** componentes que consomem muito (PSRAM, DMA, CPU) entram depois que o baseline esta estabelecido

---

## Ordem de Integracao

### 1. Display ST7789

**Por que primeiro entre os perifericos:**
- Zero risco eletrico, zero risco mecanico
- SPI simples, sem competicao de barramento ainda (microSD nao esta ativo)
- Fornece feedback visual imediato para todo o resto do desenvolvimento
- Uma vez ativo, o display serve como output de diagnostico mais rico que UART puro
- Framebuffer em PSRAM — estabelece o primeiro uso de PSRAM, permitindo validar gestao de memoria cedo

**O que fica de fora:** LVGL, animacoes, conteudo de comportamento

---

### 2. LEDs WS2812

**Por que logo apos display:**
- Canal RMT dedicado — sem competicao com outros perifericos
- Consumo eletrico minimo e controlado
- Alto valor de debug: indica estados de sistema visualmente sem precisar de UART ou display
- Implementar estados (BOOTING, ERROR, LOW_BATTERY, SAFE_MODE) logo cedo multiplica valor do sistema de observabilidade

**O que fica de fora:** Efeitos expressivos, sincronizacao com audio ou movimento

---

### 3. Storage (microSD)

**Por que antes de qualquer periferico de risco:**
- Logging persistente e fundamental para depurar qualquer problema nas etapas seguintes
- Sem microSD, brownouts e crashes apagam todos os logs
- SPI com CS separado do display — sem conflito funcional
- Uma vez ativo, todos os crashes subsequentes ficam registrados

**O que fica de fora:** Banco de dados complexo, assets de audio (ainda nao ha pipeline)

---

### 4. Validacao de Power Path (medicoes fisicas)

**Por que e um marco separado, nao software:**
- Nao e possivel escrever codigo que detecte ripple de boost — precisa de osciloscópio
- Nao e possivel saber a corrente real dos servos sem medicao
- Toda a politica de energia foi calibrada com estimativas ate aqui — agora precisa de dados reais
- Este marco deve estar concluido e documentado antes que qualquer atuador seja ligado

**Resultado esperado:** Documento de caracterizacao com valores medidos que substituem as estimativas em ENERGY.md

---

### 5. Servo Driver (comunicacao e status — SEM movimento)

**Por que comunicacao antes de movimento:**
- O protocolo SCServo deve ser completamente estavel antes de qualquer comando de posicao
- Status (posicao, temperatura, load, error) deve ser legivel antes de mover
- Permite detectar problemas de comunicacao (timeout, colisao half-duplex) sem risco mecanico
- Testa que FE-TTLinker e wiring estao corretos sem risco de dano

**O que fica de fora:** Qualquer comando de posicao ou torque

---

### 6. Servo Motion Safety Layer

**Por que antes de qualquer movimento:**
- As protecoes devem existir na arquitetura antes de serem necessarias — nao apos um incidente
- Uma vez que o BehaviorFSM existe, sera tentador "testar rapido" sem safety layer completa
- Os 6 testes obrigatorios de seguranca validam que a camada funciona sob condicoes adversas

**O que fica de fora:** Trajetorias complexas, sincronizacao com comportamento

---

### 7. Touch Sensor

**Por que antes do IMU:**
- Completamente interno ao ESP32-S3 — zero risco externo
- Nao interfere com nenhum barramento (I2C, SPI, I2S)
- Simples de integrar e validar
- Fornece primeiro input de usuario antes de qualquer interacao de voz ou visao

**O que fica de fora:** Gestos complexos, integracao com comportamento

---

### 8. IMU (MPU-6050)

**Por que logo antes de liberar movimento:**
- I2C, sensor passivo — sem risco
- Deteccao de queda (EVT_IMU_FALL) deve existir antes que o robo faca movimentos mais complexos
- Fornece dado de seguranca critico: se o robo tomba ou cai, servos desligam automaticamente
- Vibracoes dos servos podem ser um problema — melhor avaliar com servos ja testados

**O que fica de fora:** Fusao de sensor completa, algoritmos de orientacao avancados

---

### 9. Microfone INMP441

**Por que apos sensores passivos, antes de camera:**
- I2S0 — sem risco fisico
- Independe de outros subsistemas pesados (exceto storage para logs)
- Pipeline de captura pode ser validado separadamente do pipeline de reconhecimento
- Captura de audio tem menor impacto em PSRAM do que camera — validar pipeline I2S antes de adicionar DVP camera

**O que fica de fora:** ASR, wake word, processamento de linguagem

---

### 10. Speaker MAX98357A

**Por que logo apos microfone:**
- I2S1 — sem risco fisico
- Depende de storage (arquivos WAV no microSD) — ja disponivel
- Testar I2S0 e I2S1 simultaneos antes de integrar camera (valida coexistencia de DMA)
- Feedback auditivo aumenta muito o valor de debug e testes de comportamento futuro

**O que fica de fora:** TTS, MP3, streaming

---

### 11. Camera OV2640

**Por que por ultimo entre os perifericos:**
- Maior consumidor de PSRAM (200-400KB por frame buffer)
- DVP trava ~12 pinos de GPIO permanentemente
- DMA de alta largura de banda — potencial conflito com I2S
- Deve entrar somente depois que:
  - Gestao de PSRAM esta estavel com display + audio buffers
  - DMA de I2S0 e I2S1 esta validado e estavel
  - Heap disponivel foi medido e documentado
  - Qualquer problema de fragmentacao de PSRAM ja foi identificado pelos outros subsistemas

**O que fica de fora:** Visao computacional, tracking, streaming

---

### 12. Comportamento e Persona

**Por que absolutamente por ultimo:**
- Orquestra todos os outros subsistemas
- Bug no comportamento pode acionar qualquer subsistema de forma inesperada
- Somente faz sentido implementar quando ha confianca que cada peca individual funciona corretamente
- O BehaviorFSM e a camada mais dificil de testar isoladamente — precisa que todo o resto seja confiavel

---

## O que Adiar de Proposito

### Adiar ate comportamento estar maduro

| Feature                          | Motivo para adiar                                            |
|----------------------------------|--------------------------------------------------------------|
| Reconhecimento de voz (ASR)      | Depende de audio pipeline estavel + possivelmente nuvem     |
| Sintese de voz (TTS)             | Depende de audio playback + servico externo ou modelo local  |
| Wake word detection              | Depende de audio capture + modelo otimizado para ESP32       |
| Expressividade visual animada    | Depende de display + comportamento definido                  |
| Sincronizacao mov. + audio       | Depende de motion + audio simultaneos testados               |
| Personalidade e respostas        | Depende de toda a integracao previa                          |

### Adiar ate camera estavel

| Feature                          | Motivo para adiar                                            |
|----------------------------------|--------------------------------------------------------------|
| Visao computacional              | Camera deve estar estavel e PSRAM gerenciada                 |
| Face/object detection            | Requer camera + potencialmente modelo de ML                  |
| Tracking de gestos               | Requer camera + visao computacional                         |

### Adiar ate arquitetura consolidada

| Feature                          | Motivo para adiar                                            |
|----------------------------------|--------------------------------------------------------------|
| WiFi para features de produto    | Core 0 afetado — planejar antes de habilitar                |
| MQTT / HTTP client / cloud       | Adiciona complexidade de estado e reconnection               |
| OTA update                       | Requer particao de OTA configurada e processo seguro         |

---

## Erros Estrategicos a Evitar

| Erro                                   | Por que e errado                                                     |
|----------------------------------------|----------------------------------------------------------------------|
| Comecar pelo comportamento ou persona  | Construir em cima de base nao validada → bugs silenciosos, retrabalho|
| Integrar servos antes de power path validado | Risco de brownout e dano mecanico sem dados reais de corrente |
| Habilitar WiFi antes de planejar impacto no Core 0 | Degrada previsibilidade de todas as tasks de aplicacao |
| Adicionar camera cedo para "demo"      | Fragmentacao de PSRAM impossivel de depurar com outros subsistemas  |
| Pular Etapa 2.1 (medicoes fisicas)    | Todas as estimativas de energia sao hipoteticas sem medicao real     |
| Implementar feature nova em cima de etapa nao concluida | Criterios de aceitacao pendentes = base fragil |

---

## Sequencia Recomendada de Execucao

### Menor caminho robusto ate "Base Solida"

```
0.1 → 0.2 → 0.3 → 1.1 → 1.2 → 1.3
                         ↓
              VALIDACAO: 24h soak test de sistema base
                         ↓
                  2.1 (medicoes fisicas)
                         ↓
              MARCO: BASE SOLIDA CONCLUIDA
```

### Pos-base: ordem de integracao de servicos

```
3.1 → 3.2 (servos: comunicacao e safety)
  ↓
  VALIDACAO: 6 testes de seguranca de servo aprovados
  ↓
4.1 → 4.2 (touch e IMU)
  ↓
5.1 → 5.2 (audio)
  ↓
  VALIDACAO: audio + servos sem conflito
  ↓
6.1 (camera)
  ↓
  VALIDACAO: camera + audio sem conflito de DMA
  ↓
7.1 (comportamento e persona)
  ↓
8.1 (integracao total e validacao longa)
```

Cada seta representa uma validacao dos criterios de aceitacao da etapa anterior antes de avancar. Nao pular validacoes.
