/*
 * nb_config_keys.h — Chaves NVS e valores padrão do NoiseBot
 *
 * Fonte de verdade única para todos os namespaces, chaves, defaults e ranges.
 * Nenhum outro arquivo deve hardcodar strings de chave NVS ou valores default.
 *
 * Namespaces:
 *   NB_CFG_NS  ("nb_cfg") — configuração do produto: limites de servo, volume, etc.
 *   NB_SVC_NS  ("nb_svc") — estado de serviços: última emoção, persona seed.
 *   "nb_sys" é gerenciado diretamente pelo boot_manager (não via config_manager).
 *
 * Restrições NVS do ESP-IDF:
 *   - Nome de namespace: máx 15 caracteres.
 *   - Nome de chave: máx 15 caracteres.
 */

#ifndef NB_CONFIG_KEYS_H
#define NB_CONFIG_KEYS_H

/* ── Namespaces ───────────────────────────────────────────────────────────── */

#define NB_CFG_NS          "nb_cfg"
#define NB_SVC_NS          "nb_svc"
#define NB_WIFI_NS         "nb_wifi"    /* credenciais WiFi (Etapa 9.6) */

/* ── Chaves nb_wifi — nomes das chaves NVS (Etapa 9.6) ──────────────────── */
#define NB_WIFI_KEY_SSID   "ssid"       /* NVS key — string, máx 32 chars */
#define NB_WIFI_KEY_PASS   "pass"       /* NVS key — string, máx 64 chars */
#define NB_WIFI_KEY_EN     "enabled"    /* NVS key — u8: 1=ativo, 0=desabilitado */


/* ── Marcador de versão de configuração ──────────────────────────────────── */
/*
 * Quando o config_manager detecta que cfg_ver < NB_CFG_SCHEMA_VERSION (ou
 * ausente), reaplica todos os defaults. Incrementar NB_CFG_SCHEMA_VERSION
 * força re-aplicação dos defaults no próximo boot.
 */
#define NB_CFG_KEY_VERSION    "cfg_ver"         /* u8 */
#define NB_CFG_SCHEMA_VERSION  3U

/* ── Chaves nb_cfg — Servo pan (ID 1) ────────────────────────────────────── */
/*
 * Posições em unidades brutas do SCS0009 (0–1023, 10-bit).
 * Centro nominal = 512. 1 step ≈ 0.293° (300° / 1023).
 * Default ±30° ≈ ±102 steps. Expandir após calibração mecânica.
 */
#define NB_CFG_KEY_SRV1_MIN   "srv1_min"        /* i16 — posição mínima */
#define NB_CFG_KEY_SRV1_MAX   "srv1_max"        /* i16 — posição máxima */
#define NB_CFG_KEY_SRV1_CTR   "srv1_ctr"        /* i16 — posição central */

/* ── Chaves nb_cfg — Servo tilt (ID 2) ───────────────────────────────────── */
#define NB_CFG_KEY_SRV2_MIN   "srv2_min"        /* i16 */
#define NB_CFG_KEY_SRV2_MAX   "srv2_max"        /* i16 */
#define NB_CFG_KEY_SRV2_CTR   "srv2_ctr"        /* i16 */

/* ── Chaves nb_cfg — Áudio ────────────────────────────────────────────────── */
#define NB_CFG_KEY_VOLUME     "volume"           /* u8 — 0..100 */

/* ── Chaves nb_cfg — Display ─────────────────────────────────────────────── */
#define NB_CFG_KEY_BRIGHTNESS "brightness"       /* u8 — 0..255 */

/* ── Chaves nb_cfg — Touch ───────────────────────────────────────────────── */
#define NB_CFG_KEY_TOUCH_SENS "touch_sens"       /* u8 — 1..100 (fator de sensibilidade) */

/* ── Chaves nb_cfg — Comportamento ──────────────────────────────────────────*/
#define NB_CFG_KEY_IDLE_TMO   "idle_timeout"     /* u32 — segundos até entrar em SLEEPING */

/* ── Chaves nb_svc — Estado de serviços ─────────────────────────────────── */
#define NB_SVC_KEY_EMOTION    "last_emotion"     /* u8 — nb_emotion_t (Etapa 5.1) */
#define NB_SVC_KEY_PERSONA    "persona_seed"     /* u32 — seed aleatório no primeiro boot */

/* ── Defaults ─────────────────────────────────────────────────────────────── */

#define NB_CFG_DEFAULT_SRV_CTR          512     /* centro nominal SCS0009 */
#define NB_CFG_DEFAULT_SRV_MIN          410     /* centro − 102 steps (≈−30°) */
#define NB_CFG_DEFAULT_SRV_MAX          614     /* centro + 102 steps (≈+30°) */

#define NB_CFG_DEFAULT_VOLUME            25     /* % — audível mas não alto */
#define NB_CFG_DEFAULT_BRIGHTNESS       180     /* ~70% — confortável em ambiente escuro */
#define NB_CFG_DEFAULT_TOUCH_SENS         1     /* 1% acima do baseline (copper pad, ESP32-S3) */
#define NB_CFG_DEFAULT_IDLE_TIMEOUT_S   1200    /* 20 minutos */

#define NB_SVC_DEFAULT_EMOTION            0     /* NEUTRAL */

/* ── Ranges de validação ──────────────────────────────────────────────────── */

#define NB_CFG_SRV_POS_ABS_MIN          0
#define NB_CFG_SRV_POS_ABS_MAX       1023

#define NB_CFG_VOLUME_MIN               0
#define NB_CFG_VOLUME_MAX             100

#define NB_CFG_BRIGHTNESS_MIN           0
#define NB_CFG_BRIGHTNESS_MAX         255

#define NB_CFG_TOUCH_SENS_MIN           1
#define NB_CFG_TOUCH_SENS_MAX         100

#define NB_CFG_IDLE_TMO_MIN_S          10       /* mínimo 10 segundos */
#define NB_CFG_IDLE_TMO_MAX_S        3600       /* máximo 1 hora */

#endif /* NB_CONFIG_KEYS_H */
