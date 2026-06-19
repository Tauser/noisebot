# NoiseBot — Persistência

## Visão Geral

O sistema usa dois mecanismos de persistência com papeis distintos e
complementares. Na topologia final, o microSD é montado exclusivamente pelo
head-controller; o main-controller usa um cliente assíncrono sobre o enlace
inter-MCU.

| Mecanismo               | Capacidade       | Velocidade              | Uso                                             |
| ----------------------- | ---------------- | ----------------------- | ----------------------------------------------- |
| **NVS** (flash interna) | ~32KB utilizável | Rápido, síncrono        | Config crítica, flags de safety, estado de boot |
| **microSD** (FATFS)     | Gigabytes        | Variável (5–50ms/write) | Logs, assets, memória longa, backups            |

**Regra fundamental:** NVS é lido durante o boot antes do SD estar disponível. Nada que seja necessário no boot pode estar só no SD.

---

## Divisão NVS vs. microSD

### NVS — Namespace `nb_sys` (estado do sistema)

| Chave               | Tipo   | Descrição                                | Default |
| ------------------- | ------ | ---------------------------------------- | ------- |
| `boot_count`        | uint32 | Contagem de boots desde o último sucesso | 0       |
| `last_reset_reason` | uint8  | `esp_reset_reason_t` do último reset     | POWERON |
| `safe_mode_flag`    | uint8  | 1 = próximo boot em safe mode            | 0       |
| `boot_success`      | uint8  | 1 = último boot concluiu todas as fases  | 0       |
| `fw_version`        | string | Versão do firmware atual                 | "0.0.0" |

### NVS — Namespace `nb_cfg` (configuração do produto)

| Chave                | Tipo   | Descrição                              | Default  |
| -------------------- | ------ | -------------------------------------- | -------- |
| `servo_pan_min`      | int16  | Limite mínimo do servo PAN (unid. SCS) | 1638     |
| `servo_pan_max`      | int16  | Limite máximo do servo PAN             | 2458     |
| `servo_pan_center`   | int16  | Posição central PAN                    | 2048     |
| `servo_tilt_min`     | int16  | Limite mínimo do servo TILT            | 1843     |
| `servo_tilt_max`     | int16  | Limite máximo do servo TILT            | 2253     |
| `servo_tilt_center`  | int16  | Posição central TILT                   | 2048     |
| `servo_speed_max`    | uint16 | Velocidade máxima (unid. SCS)          | 200      |
| `volume_level`       | uint8  | Volume de 0 a 100                      | 70       |
| `display_brightness` | uint8  | Brilho dos LEDs de 0 a 255; tela atual não tem BL ajustável | 180      |
| `touch_sensitivity`  | uint8  | Sensibilidade touch em passos de 0,2% acima do baseline | 25       |
| `idle_timeout_s`     | uint32 | Segundos até entrar em SLEEPING        | 3600     |
| `log_level`          | uint8  | Nível mínimo de log (0=VERBOSE)        | 3 (INFO) |

### NVS — Namespace `nb_svc` (estado de serviços)

| Chave               | Tipo   | Descrição                                | Default         |
| ------------------- | ------ | ---------------------------------------- | --------------- |
| `persona_seed`      | uint32 | Seed de personalidade (imutável)         | rand no 1º boot |
| `last_emotion_val`  | float  | Última valência emocional                | 0.0             |
| `last_emotion_aro`  | float  | Último arousal emocional                 | 0.0             |
| `total_touch_count` | uint32 | Snapshot rápido de toques totais         | 0               |
| `total_hours_x100`  | uint32 | Horas totais × 100 (sem ponto flutuante) | 0               |

### NVS — Namespace `nb_persona` (persona e usuario atual)

| Chave         | Tipo   | Descricao                                      | Default       |
| ------------- | ------ | ---------------------------------------------- | ------------- |
| `p_warmth`    | uint32 | Float bit-cast: calor relacional emergente     | 0.0           |
| `p_energy`    | uint32 | Float bit-cast: energia conversacional         | 0.0           |
| `p_curiosity` | uint32 | Float bit-cast: curiosidade                    | 1.0           |
| `p_trust`     | uint32 | Float bit-cast: confianca/familiaridade        | 0.0           |
| `user_id`     | string | Identificador local do usuario atual           | `owner`       |
| `user_name`   | string | Nome de exibicao do usuario atual              | `Owner`       |
| `relation`    | string | Relacao com o robot (`owner`, `friend`, etc.)   | `owner`       |
| `language`    | string | Idioma preferido do usuario                    | `pt-BR`       |
| `robot_name`  | string | Apelido/nome do robot nesse perfil             | `NoiseBot`    |
| `mode`        | string | Modo de persona com esse usuario               | `companion`   |
| `style`       | string | Estilo de interacao                            | `direct_warm` |

Este namespace e suficiente para operacao offline-first. Reconhecimento de voz
ou face nao faz parte do cadastro inicial; fontes futuras podem apenas trocar o
perfil atual e reutilizar o mesmo contrato.

### Server — `app_state.json` (espelho offline do dashboard)

O server local tambem persiste um espelho do perfil em
`~/.noisebot-server/app_state.json`, no campo `device_persona`. Essa copia evita
que a tela `Perfil` volte aos defaults quando o firmware HTTP estiver offline,
sem IP configurado, ou indisponivel durante o boot. Quando o firmware responde,
`GET /api/device/persona` atualiza esse espelho a partir do NVS; quando o
dashboard salva, `PUT /api/device/persona` grava primeiro no `app_state.json` e
depois tenta aplicar no firmware.

### Server — `conversations.sqlite3` (histórico conversacional)

Conversas persistentes usam SQLite em
`~/.noisebot-server/conversations.sqlite3`, com override por
`NOISEBOT_CONVERSATIONS_DB_PATH`.

O schema v1 foi iniciado em 2026-06-18 e mantém:

- conversas separadas por usuário;
- conversa ativa por usuário;
- turnos com status `pending`, `complete`, `failed` ou `interrupted`;
- mensagens ordenadas por sequência;
- idempotência de requisições do dashboard;
- cascata de exclusão;
- migrations monotônicas.

SQLite é a fonte de verdade operacional. A futura projeção para Obsidian será
derivada e não substituirá o banco. O módulo é síncrono; chamadas feitas pelo
event loop devem usar `asyncio.to_thread`.

### microSD único no head-controller

Todos os dados com volume alto, append-only, ou que podem ser regenerados:

```
/sdcard/
├── logs/
│   ├── log_20260412.txt      # Log diário, flush a cada 60s
│   ├── log_20260413.txt
│   └── ...                    # Rotação: manter 7 dias, deletar mais antigos
│
├── assets/
│   └── audio/
│       ├── greet_01.wav       # 16kHz, mono, 16-bit PCM
│       ├── greet_02.wav
│       ├── greet_03.wav
│       ├── wake_up.wav
│       ├── sleep_enter.wav
│       ├── timer_done.wav
│       ├── reminder_due.wav
│       ├── alarm_due.wav
│       └── error_01.wav
│
├── memory/
│   ├── interactions.bin       # Ring buffer de últimas 200 interações
│   ├── persona.json           # Estado de persona, preferências detectadas
│   └── events.bin             # Event journal, últimas 1000 entradas
│
├── config/
│   └── config_backup.json     # Snapshot do NVS, atualizado na transição SLEEPING
│
└── snapshots/
    └── crash_20260412_143022.bin  # Coredump em crash, com timestamp
```

---

## Estrutura de Memória de Longo Prazo

### interactions.bin

Ring buffer binário compacto. Cada entrada = 24 bytes:

```c
typedef struct __attribute__((packed)) {
    uint32_t timestamp;       // Unix timestamp (segundos)
    uint8_t  type;            // nb_interaction_type_t
    uint8_t  duration_s;      // duração em segundos (0=instantâneo)
    uint8_t  emotion_val_u8;  // valência × 127 + 127 → uint8
    uint8_t  emotion_aro_u8;  // arousal × 127 + 127 → uint8
    uint8_t  response_id;     // ID da resposta dada (0=nenhuma)
    uint8_t  reserved[3];     // padding para alinhamento
    uint32_t checksum;        // CRC32 do registro
} nb_interaction_record_t;    // 16 bytes
```

Arquivo tem header fixo com magic, versão, count e write_head (índice do próximo slot).

### persona.json

```json
{
  "persona_seed": 2748291,
  "first_boot_ts": 1744573200,
  "total_boots": 47,
  "user_familiar": true,
  "detected_prefs": {
    "preferred_volume": 65,
    "touch_frequency_high": true,
    "prefers_calm_responses": false
  },
  "last_seen_ts": 1744659600,
  "checksum": "a3f2c1"
}
```

### events.bin

Event journal com entradas de tamanho variável (máx 64 bytes por entrada).
Header: magic, versão, entry_count, head_offset. Append-only com wrap-around.

Exemplos de entradas significativas:

- Primeiro boot
- Primeiro toque do usuário
- Temperatura máxima registrada nos servos
- Versão de firmware atualizada
- SD removido e reinserido
- Safe mode ativado

---

## Políticas de Escrita

### Escrita Assíncrona (padrão)

Toda escrita não-urgente segue o caminho final:

```
chamador main
  → persistence_mgr_enqueue()
  → fila local limitada
  → storage client/BULK
  → enlace SPI
  → storage worker do head
  → FATFS/SDMMC
  → resposta de commit ao main
```

- Enfileiramento local não confirma durabilidade.
- `persistence_task` do main nunca chama FATFS/SDMMC no estado final.
- `storage_worker` do head opera em prioridade baixa e serializa FATFS.
- Fila baseline: 128KB ou 256 registros, o que ocorrer primeiro.
- Em 80%, eventos repetitivos são agregados; em 100%, descarta-se primeiro
  telemetria/log repetitivo, depois eventos comuns. Configuração/persona crítica
  é espelhada em NVS e nunca descartada silenciosamente.
- Timeout de flush: se fila parada por >60s, força flush.
- Ao entrar em SLEEPING: solicitar `sync` e aguardar resposta com deadline; se
  head estiver ausente, registrar pendência em NVS sem bloquear indefinidamente.

Retries reutilizam a mesma sequence e são idempotentes. Reboot do head invalida
handles e transferências; o main reabre e repete apenas operações cujo estado
durável foi reconciliado.

### Escrita Síncrona (exceções)

No main, escrita síncrona direta no SD deixa de existir após F5. Operações
bloqueantes são permitidas apenas quando:

- crash dump local em partição/flash reservada, se implementado;
- espera limitada pela confirmação de `sync` ao entrar em SLEEPING.

O crash dump não pode depender do head estar operacional.

### Throttling

- Log: máximo 1 flush/minuto para SD (acumular em buffer de SRAM/PSRAM entre flushes)
- Interaction history: 1 flush/5min ou ao dormir
- Nunca fazer write amplification: usar append-only e não reescrever arquivo inteiro para 1 registro

---

## Proteção Contra Corrupção

### NVS

ESP-IDF usa journaling interno com CRC. Resistente a power-off.
Risco mínimo. Monitorar `nvs_get_stats()` para detectar namespace cheio.

### microSD / FATFS

Vulnerável a power-off durante escrita. Mitigações:

1. **Write-then-rename para arquivos críticos:**
   - Escrever em arquivo temporário (`persona.json.tmp`)
   - Fechar/flush completamente
   - Renomear para nome final (`persona.json`) — operação mais atômica no FATFS

2. **Validação de integridade no boot:**
   - Arquivos de memória têm checksum. Se inválido: logar erro, criar arquivo fresh.
   - Não panic por arquivo de memória corrompido.

3. **Nunca deixar arquivo aberto por longo período:**
   - Abrir → escrever → fechar em uma operação atômica do ponto de vista do FATFS.

4. **Logs:**
   - Append-only. Corrupção de fim de arquivo por power-off = aceitável (último registro pode estar incompleto).
   - Verificar tamanho do arquivo no boot, truncar se necessário.

---

## Boot sem SD

O sistema deve funcionar em modo degradado sem SD:

| Feature                | Com SD | Sem SD (modo degradado)                |
| ---------------------- | ------ | -------------------------------------- |
| Logging UART           | ✅     | ✅                                     |
| Logging SD             | ✅     | ❌ (buffered, dropped)                 |
| Assets de áudio        | ✅     | ❌ (silêncio)                          |
| Memória de longo prazo | ✅     | ❌ (operações em memória, descartadas) |
| Perfil/persona local   | ✅     | ✅ (NVS, independente do SD)           |
| Config de NVS          | ✅     | ✅                                     |
| Expressão facial       | ✅     | ✅                                     |
| Motion                 | ✅     | ✅                                     |
| Touch                  | ✅     | ✅                                     |

SD-degradado é loggado em UART. Nenhum comportamento crítico depende do SD.

### Head indisponível e LTM

- Persona continua pelo snapshot NVS do main.
- Leituras de LTM retornam `UNAVAILABLE`; ausência de dados não equivale a
  “nenhuma memória”.
- Escritas entram na fila limitada e seguem a política de prioridade acima.
- Contadores de descarte ficam em NVS e são enviados aos diagnósticos após
  reconexão.
- Replay é ordenado e idempotente.

### Assets de áudio remotos

Áudio continua fisicamente no main. Assets no SD do head são lidos antes e
durante o playback para um ring buffer na PSRAM do main:

- baseline: 96KB de buffer, prebuffer de 64KB e refill abaixo de 32KB;
- I2S DMA usa buffers em SRAM;
- bulk de áudio tem prioridade sobre JPEG, logs e LTM, abaixo de
  `CONTROL`/`EVENT`;
- wake/error essenciais possuem cópia curta em flash;
- underrun publica diagnóstico e encerra com fade, sem bloquear I2S.

### Re-mount Periódico

Se SD falhou ao montar, `sd_hal` tenta re-mount a cada 30s.
Se SD for inserido após boot sem SD, sistema detecta e monta automaticamente.
