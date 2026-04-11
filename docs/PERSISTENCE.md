# NoiseBot — Persistência e Memória de Longo Prazo

## Adendo Arquitetural

Este documento complementa `ARCHITECTURE.md` e `ROADMAP.md` sem reescrever o planejamento anterior.

O microSD onboard **não é periférico opcional**. É componente central da estratégia de persistência do sistema. A ausência dessa clareza na arquitetura levaria ao mesmo problema que se quer evitar: memória de longo prazo adicionada tardiamente como remendo, acoplada ao comportamento de formas que são difíceis de desfazer.

A fundação de persistência precisa ser desenhada antes de qualquer serviço de comportamento ser implementado, mesmo que a funcionalidade de memória de longo prazo seja parcialmente habitada em fases posteriores.

---

## 1. Estratégia de Persistência em Camadas

O sistema tem cinco camadas de persistência com características distintas. Cada dado deve residir na camada correta.

```
┌──────────────────────────────────────────────────────────────┐
│  CAMADA 0 — Memória Volátil de Runtime (SRAM / PSRAM)        │
│  Dura enquanto há energia. Perdida em qualquer reset.        │
│  Ex: estado atual de servos, frame de câmera, buffer de áudio│
├──────────────────────────────────────────────────────────────┤
│  CAMADA 1 — RTC Memory (survive brownout/WDT)                │
│  ~8KB. Sobrevive brownout e WDT reset. Perdida em power-off. │
│  Ex: crash_count, brownout_flag, safe_mode_flag              │
├──────────────────────────────────────────────────────────────┤
│  CAMADA 2 — NVS / Flash interna                              │
│  Persistente. Pequena (~16-32KB utilizável). ~100k ciclos.   │
│  Ex: config do sistema, calibrações, parâmetros              │
├──────────────────────────────────────────────────────────────┤
│  CAMADA 3 — microSD onboard (arquivos)                       │
│  Grande (~GBs). Pode falhar. FAT32, sem wear leveling.       │
│  Ex: logs, assets de áudio, histórico, snapshots             │
├──────────────────────────────────────────────────────────────┤
│  CAMADA 4 — microSD (memória de longo prazo do robô)         │
│  Estrutura viva. Cresce com o tempo. Informa comportamento.  │
│  Ex: preferências, episódios, traços de persona, contexto    │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. Divisão por Camada de Armazenamento

### O que fica na NVS / Flash Interna

**Critério de seleção:** Dado crítico, pequeno, que precisa estar disponível mesmo sem microSD.

| Dado                              | Namespace NVS       | Tipo    | Notas                                    |
|-----------------------------------|---------------------|---------|------------------------------------------|
| Boot counter                      | `nb_system`         | u32     | Duplicado em RTC memory para brownout    |
| Crash counter                     | `nb_system`         | u32     | Duplicado em RTC memory                  |
| Schema version                    | `nb_system`         | u8      | Para migração de NVS                     |
| Safe mode flag                    | `nb_system`         | u8      | Duplicado em RTC memory                  |
| Último reset reason persistido    | `nb_system`         | u8      | Complementa RTC memory                  |
| Thresholds de energia             | `nb_power`          | u16/u8  | LOW_PCT, CRITICAL_PCT, SHUTDOWN_MV       |
| Limites de posição dos servos     | `nb_servo`          | u16 x4  | min/max por servo                        |
| Thresholds de temperatura dos servos | `nb_servo`       | u8      | OVERTEMP_THRESHOLD                       |
| Heartbeat timeout de movimento    | `nb_servo`          | u16     | ms                                       |
| Calibração de touch baseline      | `nb_touch`          | u16     | Por pino de touch                        |
| Sample rate de áudio configurado  | `nb_audio`          | u16     | Hz                                       |
| Ganho de volume                   | `nb_audio`          | u8      | 0-100                                    |
| Preferências de boot display      | `nb_display`        | u8      | Brilho padrão, tema                      |
| Device ID / nome do robô          | `nb_identity`       | str     | Máx 16 chars, fixo após primeiro boot    |
| Flags de feature habilitada       | `nb_features`       | u32     | Bitmask de features ativas               |
| Versão de calibração do IMU       | `nb_imu`            | u8      | Indica se calibração no SD é válida      |
| Ponteiro para última memória ativa| `nb_memory`         | str     | Path relativo do arquivo de contexto ativo no SD |

**O que NÃO deve ir para NVS:**
- Logs (tamanho ilimitado — NVS é pequena)
- Assets de áudio ou imagem
- Histórico de interações
- Calibrações completas do IMU (offset de 6 eixos = 48 bytes — cabe em NVS, mas SD é mais adequado por legibilidade)
- Contexto detalhado de persona (pode crescer)

### O que fica no microSD

**Critério de seleção:** Dado grande, que cresce com o tempo, ou que pode ser regenerado/perdido sem comprometer a operação crítica.

#### Estrutura de diretórios

```
/noisebot/
  /logs/
    system_0001.log        # Log rotativo atual
    system_0002.log        # Arquivo anterior
    ...
  /assets/
    /audio/
      greet_01.wav         # Assets de áudio para playback
      idle_hum.wav
      ack_01.wav
    /display/              # Assets visuais se necessário
  /config/
    extended_config.json   # Config que não cabe em NVS (ex: mapeamentos complexos)
    imu_calibration.json   # Offsets de 6 eixos do MPU-6050
  /memory/
    /episodic/
      2026-04.jsonl        # Registro de interações (append-only, por mês)
      2026-05.jsonl
    /semantic/
      preferences.json     # Preferências aprendidas do usuário
      context.json         # Contexto persistente atual
    /persona/
      traits.json          # Traços de personalidade (parâmetros evolutivos)
    /snapshots/
      state_current.json   # Snapshot do estado atual do sistema
      state_prev.json      # Shadow copy do snapshot anterior
  /diagnostics/
    crash_reports/
      crash_20260411.json  # Stack trace e estado no momento do crash
    metrics/
      daily_20260411.json  # Métricas diárias de operação
```

---

## 3. Arquitetura para Memória de Longo Prazo

### Princípio

A memória de longo prazo do robô não precisa ser totalmente implementada no início — mas a **estrutura de dados e a API** devem ser definidas antes da Etapa 7.1 (BehaviorFSM). Isso evita que o comportamento cresça dependente de nenhuma memória e depois precise ser refatorado para consumir uma memória adicionada como remendo.

### Subsistemas de Memória

#### 3.1 Memória Episódica

Registro cronológico de interações relevantes. Formato JSONL (JSON Lines): um registro JSON por linha, append-only. Nunca sobrescrever — apenas acrescentar e rotacionar por mês.

```jsonl
{"ts":1744329600,"type":"touch","duration_ms":1200,"context":"idle","response":"greeting"}
{"ts":1744329800,"type":"voice","vad_energy":0.72,"response":"ack","soc_pct":78}
{"ts":1744330100,"type":"motion","servo":0,"pos":512,"trigger":"touch"}
```

**Campos mínimos por registro:**
- `ts`: timestamp Unix (segundos)
- `type`: categoria do evento (`touch`, `voice`, `motion`, `sleep`, `wake`, `error`, `charge`)
- `context`: estado do FSM no momento
- `response`: ação tomada

**Limites de retenção:** Configurável. Default: 90 dias. Arquivos mais antigos deletados automaticamente no boot.

**Custo de escrita:** Baixo — apenas append de ~100-200 bytes por evento relevante. Não logar cada frame de sensor, apenas eventos de interação.

#### 3.2 Memória Semântica

Dados que representam "o que o robô aprendeu" — sem estrutura temporal. Arquivo JSON reescrito periodicamente (não append-only).

```json
{
  "preferences": {
    "touch_response_style": "gentle",
    "voice_sensitivity": 0.65,
    "idle_behavior": "calm",
    "preferred_greeting": "greet_02.wav"
  },
  "interaction_stats": {
    "total_interactions": 247,
    "avg_touch_duration_ms": 1450,
    "most_common_trigger": "touch",
    "peak_activity_hour": 19
  },
  "learned_patterns": {
    "user_usually_active": ["18:00", "22:00"],
    "frequent_touch_zones": ["front_left"]
  },
  "_schema": 1,
  "_updated_ts": 1744329600
}
```

**Estratégia de escrita:** Escrita periódica (a cada 30 min de operação ou no shutdown), não a cada interação. Usar escrita atômica (`.tmp` + rename).

#### 3.3 Traços de Persona

Parâmetros que definem o "caráter" do robô e podem derivar lentamente com o tempo baseado nas interações. Distintos de preferências do usuário.

```json
{
  "energy_level": 0.72,
  "curiosity": 0.81,
  "expressiveness": 0.65,
  "response_latency_bias": 0.4,
  "calibrated_interactions": 247,
  "_schema": 1,
  "_updated_ts": 1744329600
}
```

Estes valores são inicializados com defaults e derivam lentamente — não podem ser resetados por um evento único. São o "estado emocional de longo prazo" do robô.

#### 3.4 Contexto Persistente

Estado de curto-médio prazo que sobrevive entre sessões mas não é histórico completo. Exemplo: o robô lembrar o que foi dito na última sessão.

```json
{
  "last_session_ts": 1744329600,
  "last_session_duration_s": 3240,
  "last_trigger": "touch",
  "last_response": "greet_02",
  "session_count_today": 3,
  "context_summary": "",
  "_schema": 1
}
```

#### 3.5 Snapshots de Estado do Sistema

Snapshot periódico do estado do sistema para recuperação e diagnóstico. Duas cópias (current + prev) para sobreviver a falha de escrita.

```json
{
  "ts": 1744329600,
  "soc_pct": 72,
  "charger_state": "discharging",
  "boot_count": 47,
  "crash_count_session": 0,
  "uptime_s": 3240,
  "active_features": ["display", "led", "touch", "imu", "audio"],
  "last_error": null,
  "firmware_version": "0.1.0",
  "_schema": 1
}
```

Escrito a cada 5 minutos de operação e no shutdown controlado.

### API do PersistenceManager

```c
// infra/persistence_manager.h

// Inicializacao
esp_err_t persistence_init(void);
bool      persistence_sd_available(void);
bool      persistence_sd_healthy(void);

// Memória episódica
esp_err_t persistence_record_episode(const nb_episode_t *ep);
esp_err_t persistence_rotate_episodes(uint32_t max_age_days);

// Memória semântica
esp_err_t persistence_load_preferences(nb_preferences_t *prefs);
esp_err_t persistence_save_preferences(const nb_preferences_t *prefs);

// Traços de persona
esp_err_t persistence_load_persona_traits(nb_persona_traits_t *traits);
esp_err_t persistence_save_persona_traits(const nb_persona_traits_t *traits);

// Contexto persistente
esp_err_t persistence_load_context(nb_context_t *ctx);
esp_err_t persistence_save_context(const nb_context_t *ctx);

// Snapshots
esp_err_t persistence_write_snapshot(const nb_system_snapshot_t *snap);
esp_err_t persistence_load_last_snapshot(nb_system_snapshot_t *snap);
```

**Regra de degradação:** Se o microSD não estiver disponível, `persistence_init()` retorna `ESP_OK` com `persistence_sd_available() == false`. Todas as operações de memória de longo prazo retornam `ESP_ERR_NOT_SUPPORTED` silenciosamente (logadas, não fatais). O sistema opera em modo amnésico.

---

## 4. Cuidados de Engenharia

### 4.1 Corrupção de Dados

**Problema:** FAT32 não é transacional. Corte de energia durante write pode corromper o arquivo. Não há journaling.

**Mitigação por tipo de dado:**

| Tipo de dado         | Estratégia de escrita            | Proteção adicional            |
|----------------------|----------------------------------|-------------------------------|
| Logs (episódicos)    | Append-only JSONL — seguro       | Linha incompleta ignorada ao ler |
| Preferências/persona | Write to `.tmp` + `rename()`     | Shadow copy (prev + current)  |
| Snapshots            | Write to `.tmp` + `rename()`     | Duas cópias (current + prev)  |
| Config estendido     | Write to `.tmp` + `rename()`     | CRC32 no campo `_crc`         |
| Calibrações          | Write to `.tmp` + `rename()`     | CRC32, fallback para NVS      |

**Padrão de escrita atômica:**
```c
// SEMPRE usar este padrão para arquivos críticos
esp_err_t atomic_write_json(const char *final_path, const cJSON *obj) {
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    // 1. Escrever no arquivo temporário
    FILE *f = fopen(tmp_path, "w");
    if (!f) return ESP_ERR_NOT_FOUND;
    cJSON_print_to_file(f, obj);
    fflush(f);
    fsync(fileno(f));   // Garante flush para o cartão
    fclose(f);

    // 2. Renomear atomicamente
    return (rename(tmp_path, final_path) == 0) ? ESP_OK : ESP_FAIL;
}
```

### 4.2 Remoção e Falha do microSD

**Problema:** Usuário pode remover o cartão sem unmount. Ou o cartão pode falhar eletricamente.

**Estratégia:**
- Monitoramento periódico de saúde do SD (a cada 60s): tentar abrir arquivo de health check
- Se falha detectada em operação: fechar todos os handles abertos, publicar `EVT_STORAGE_DEGRADED`, entrar em modo amnésico
- **Boot crítico não depende do SD.** Se SD ausente ou com erro: `BOOT_PHASE_3` segue em modo degradado (log apenas UART, sem memória de longo prazo)
- Tentar reinit do SD periodicamente (a cada 30s) quando degradado — pode ter sido reinserido

```c
// Em storage_service.c
static void sd_health_monitor_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!check_sd_health()) {
            ESP_LOGE(TAG, "SD health check failed — entering degraded mode");
            storage_service_set_degraded();
            event_bus_publish(&(nb_event_t){.type = EVT_STORAGE_DEGRADED});
        }
    }
}
```

### 4.3 Escrita Excessiva e Desgaste do Cartão

**Problema:** FAT32 sem wear leveling. Escritas frequentes no mesmo cluster desgastam o cartão. O FAT e o diretório raiz são especialmente vulneráveis.

**Regras de escrita:**

| Tipo              | Frequência máxima de escrita       | Estratégia                        |
|-------------------|------------------------------------|-----------------------------------|
| Logs              | 1 flush a cada 60s ou 4KB acumulados | Buffer em RAM, flush periódico   |
| Episódios         | 1 append por evento de interação   | Append direto — OK (arquivo cresce) |
| Preferências      | 1 rewrite a cada 30min de operação | Buffer em RAM, flush periódico    |
| Persona traits    | 1 rewrite a cada hora              | Buffer em RAM, flush no shutdown  |
| Snapshots         | 1 write a cada 5min                | Write + shadow copy               |
| Contexto          | 1 rewrite por sessão               | Flush no shutdown                 |

**Regra geral:** Nunca escrever no SD em resposta a cada evento sensor. Sempre bufferizar em RAM e fazer flush periódico ou por shutdown.

### 4.4 Boot Dependente Demais do Cartão

**Problema:** Se o boot block no microSD, o sistema inteiro fica indisponível.

**Regras inegociáveis:**
1. `BOOT_PHASE_0` (watchdog, logger, NVS, power): **zero dependência de SD**
2. `BOOT_PHASE_1` (ConfigManager, EventBus): **zero dependência de SD**
3. `BOOT_PHASE_2` (PowerManager): **zero dependência de SD**
4. SD é inicializado em `BOOT_PHASE_3` com timeout de 3s — se não montar, continua em modo degradado
5. Nenhuma função de `main()` ou de boot crítico pode chamar função de SD sem verificar `persistence_sd_available()` primeiro

### 4.5 Consistência entre NVS e microSD

**Problema:** Configuração em NVS pode estar em versão diferente dos dados no SD (ex: calibração IMU no SD foi gerada por firmware mais antigo).

**Estratégia:**
- Cada arquivo no SD tem campo `_schema` (versão do schema) e `_fw_version` (versão do firmware que escreveu)
- Ao carregar arquivo do SD, verificar se `_schema` é compatível com a versão atual
- Se incompatível: logar aviso, usar defaults e reagendar regravação do arquivo com schema novo
- NVS tem campo `nb_imu/cal_version` que referencia a versão da calibração salva no SD — se versão não coincide, recalibrar

### 4.6 Recuperação após Falha

**Sequência de recovery ao detectar dado corrompido no SD:**

```
1. Tentar ler arquivo principal
   ├── OK → usar
   └── Falhou/CRC inválido
       ↓
2. Tentar ler shadow copy (.prev ou _prev.json)
   ├── OK → usar, logar aviso "usando backup"
   └── Falhou também
       ↓
3. Usar defaults em RAM
   Logar: "arquivo {path} corrompido, usando defaults"
   Publicar EVT_STORAGE_DEGRADED
   Reagendar regravação com defaults + timestamp
```

### 4.7 Degradação Graciosa sem microSD

Quando `persistence_sd_available() == false`, o sistema opera no **modo amnésico**:

| Funcionalidade       | Com SD                         | Sem SD (amnésico)                  |
|----------------------|--------------------------------|------------------------------------|
| Logging              | UART + arquivo rotativo        | Apenas UART                        |
| Assets de áudio      | Playback de WAV                | Sem playback (modo silencioso)      |
| Memória episódica    | Registra eventos               | Eventos descartados (não falha)     |
| Preferências         | Carregadas e salvas            | Usa defaults (não persiste)         |
| Persona traits       | Carregadas e evoluem           | Inicializa com defaults fixos       |
| Contexto persistente | Carregado entre sessões        | Contexto perdido entre boots        |
| Snapshots            | Periódicos                     | Sem snapshots                       |
| Config estendida     | Do arquivo no SD               | Do NVS (fallback)                   |
| Calibração IMU       | Do arquivo no SD               | Valor do NVS ou re-calibrar         |

O robô **funciona** sem SD — apenas sem memória, sem assets e sem logs persistentes.

---

## 5. Impacto no Roadmap Original

### Mudanças de posição

**A Etapa 1.3 (Storage microSD) não muda de posição**, mas sua definição de escopo é expandida:

**Antes (escopo original):**
> Init microSD, API de arquivo sobre VFS, log rotation, fallback se ausente.

**Depois (escopo expandido):**
> Init microSD, criação da estrutura de diretórios `/noisebot/...` no primeiro boot, API de arquivo sobre VFS, log rotation, `PersistenceManager` inicializado (mesmo que operações de memória de longo prazo retornem NOT_SUPPORTED sem implementação), health monitor task, fallback gracioso se SD ausente.

**Schema dos arquivos de memória deve ser definido e commitado como código antes da Etapa 7.1 (BehaviorFSM)**, mesmo que a leitura/escrita desses arquivos não esteja implementada ainda. O FSM de comportamento é o primeiro consumidor da memória — ele precisa conhecer a interface antes de ser escrito.

### Adição: Etapa 1.3b — PersistenceManager (sem reordenar roadmap)

Imediatamente após a Etapa 1.3 (Storage), antes da Etapa 2.1 (Power Path):

**ETAPA 1.3b — PersistenceManager e Estrutura de Memória**

**Objetivo:** Estabelecer a camada de persistência que sustentará todo o sistema de memória do robô, mesmo que a maioria das funcionalidades esteja vazia inicialmente.

**Escopo que entra:**
- Criação da estrutura de diretórios `/noisebot/` no microSD na primeira inicialização
- Health check file: `/noisebot/.health` escrito e lido no boot para verificar SD funcional
- `infra/persistence_manager.h/c`: API completa com stubs — funções implementadas mas retornando dados defaults / operações vazias onde a funcionalidade ainda não existe
- Structs definidas e versionadas: `nb_episode_t`, `nb_preferences_t`, `nb_persona_traits_t`, `nb_context_t`, `nb_system_snapshot_t`
- Snapshot de sistema implementado completamente (é simples e de alto valor imediato)
- Log rotation completamente integrado ao `PersistenceManager`
- SD health monitor task ativa
- Evento `EVT_STORAGE_DEGRADED` publicado quando SD falha

**Fora do escopo:**
- Leitura e escrita de preferências (sem comportamento para consumir)
- Registros episódicos (sem comportamento para gerar)
- Evolução de persona traits (sem BehaviorFSM)

**Entregável:**
- `infra/persistence_manager.h` com API completa e contratos documentados
- `infra/persistence_manager.c` com snapshots funcionais e stubs para memória de longo prazo
- Schema de todos os arquivos JSON definido e documentado (campos, tipos, versão)
- Diretório `/noisebot/` criado no SD com estrutura completa e arquivo `.health`

**Critério de aceitação:**
- Boot com SD presente: estrutura de diretórios criada, health check OK, snapshot gravado
- Boot sem SD: modo amnésico ativo, log em UART, nenhum crash
- Remoção de SD em operação: EVT_STORAGE_DEGRADED publicado em < 120s, sistema continua operando

---

## Referência Rápida: Onde Salvar Cada Tipo de Dado

| Dado                            | Camada         | Localização                            |
|---------------------------------|----------------|----------------------------------------|
| Flags de boot, crash counter    | RTC memory + NVS | `nb_rtc_state_t` + `nb_system/`       |
| Calibração de touch baseline    | NVS            | `nb_touch/baseline_TN`                 |
| Calibração completa do IMU      | SD             | `/noisebot/config/imu_calibration.json` |
| Parâmetros do sistema (thresholds)| NVS          | `nb_power/`, `nb_servo/`, etc.         |
| Config estendida (mappings)     | SD             | `/noisebot/config/extended_config.json` |
| Logs de sistema                 | SD             | `/noisebot/logs/system_NNNN.log`        |
| Assets de áudio                 | SD             | `/noisebot/assets/audio/*.wav`          |
| Histórico de interações         | SD             | `/noisebot/memory/episodic/YYYY-MM.jsonl` |
| Preferências aprendidas         | SD             | `/noisebot/memory/semantic/preferences.json` |
| Traços de persona               | SD             | `/noisebot/memory/persona/traits.json`  |
| Contexto entre sessões          | SD             | `/noisebot/memory/semantic/context.json`|
| Snapshot de estado              | SD             | `/noisebot/memory/snapshots/state_*.json` |
| Crash reports                   | SD             | `/noisebot/diagnostics/crash_reports/`  |
| Métricas diárias                | SD             | `/noisebot/diagnostics/metrics/`        |
| Device ID / nome do robô        | NVS            | `nb_identity/device_id`                |
| Ponteiro para contexto ativo    | NVS            | `nb_memory/ctx_path`                   |
