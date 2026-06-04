# NoiseBot — Persistência

## Visão Geral

O sistema usa dois mecanismos de persistência com papeis distintos e complementares:

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
| `touch_sensitivity`  | uint8  | Sensibilidade touch em passos de 0,2% acima do baseline | 10       |
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

### microSD

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

Toda escrita não-urgente segue o caminho:

```
chamador → persistence_mgr_enqueue() → [fila FreeRTOS] → persistence_task → SD
```

- `persistence_task`: Core 0, prioridade 5. Processa a fila continuamente.
- Fila: 32 slots de mensagens (mensagem = tipo de escrita + payload pequeno).
- Timeout de flush: se fila parada por >60s, força flush.
- Ao entrar em SLEEPING: flush síncrono antes de reduzir atividade.

### Escrita Síncrona (exceções)

Usada apenas quando:

- Crash dump (sistema já em falha, não há task de fila)
- Config backup ao entrar em SLEEPING (bloqueante intencional)

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
| Config de NVS          | ✅     | ✅                                     |
| Expressão facial       | ✅     | ✅                                     |
| Motion                 | ✅     | ✅                                     |
| Touch                  | ✅     | ✅                                     |

SD-degradado é loggado em UART. Nenhum comportamento crítico depende do SD.

### Re-mount Periódico

Se SD falhou ao montar, `sd_hal` tenta re-mount a cada 30s.
Se SD for inserido após boot sem SD, sistema detecta e monta automaticamente.
