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
#define NB_CFG_KEY_V2_CAP_EN  "v2cap_en"         /* u8 — capture v2 real opt-in */
#define NB_CFG_KEY_V2_CAP_TX  "v2cap_tx_en"      /* u8 — capture v2 TX handoff opt-in */
#define NB_CFG_KEY_V2_ACT_DEC "v2act_dec"        /* u8 — activity v2 decision opt-in */
#define NB_CFG_KEY_V2_ACT_MIG "v2act_mig"        /* u8 — one-shot activity v2 default migration */

/* ── Chaves nb_cfg — Display ─────────────────────────────────────────────── */
#define NB_CFG_KEY_BRIGHTNESS "brightness"       /* u8 — 0..255 */

/* ── Chaves nb_cfg — Touch ───────────────────────────────────────────────── */
#define NB_CFG_KEY_TOUCH_SENS "touch_sens"       /* u8 — 1..100 (passos de 0,2%) */

/* ── Chaves nb_cfg — Comportamento ──────────────────────────────────────────*/
#define NB_CFG_KEY_IDLE_TMO   "idle_timeout"     /* u32 — segundos até entrar em SLEEPING */

/* ── Chaves nb_cfg — Presença Social (SPEC_PRESENCA_SOCIAL §2.3) ─────────── */
#define NB_CFG_KEY_PRES_HOLD  "pres_hold_ms"     /* u32 — hold antes de LEFT_RECENTLY  */
#define NB_CFG_KEY_PRES_MAYBE "pres_maybe_ms"    /* u32 — MAYBE_SOMEONE timeout         */
#define NB_CFG_KEY_PRES_HEUR  "pres_heur_ms"     /* u32 — heurística offline → PRESENT  */
#define NB_CFG_KEY_PRES_LREC  "pres_lrec_ms"     /* u32 — LEFT_RECENTLY → AWAY          */
#define NB_CFG_KEY_PRES_AWAY  "pres_away_ms"     /* u32 — AWAY → ALONE_SETTLED          */
#define NB_CFG_KEY_PRES_ENGD  "pres_engd_ms"     /* u32 — PRESENT → ENGAGED             */
#define NB_CFG_KEY_PRES_RTN   "pres_rtn_ms"      /* u32 — ausência mínima p/ RETURNED   */

/* ── Chaves nb_svc — Estado de serviços ─────────────────────────────────── */
#define NB_SVC_KEY_EMOTION    "last_emotion"     /* u8 — nb_emotion_t (Etapa 5.1) */
#define NB_SVC_KEY_PERSONA    "persona_seed"     /* u32 — seed aleatório no primeiro boot */

/* ── Defaults ─────────────────────────────────────────────────────────────── */

#define NB_CFG_DEFAULT_SRV_CTR          512     /* centro nominal SCS0009 */
#define NB_CFG_DEFAULT_SRV_MIN          410     /* centro − 102 steps (≈−30°) */
#define NB_CFG_DEFAULT_SRV_MAX          614     /* centro + 102 steps (≈+30°) */

#define NB_CFG_DEFAULT_VOLUME            25     /* % — audível mas não alto */
#define NB_CFG_DEFAULT_V2_CAP_EN          0     /* off por padrao: v1 continua ativo */
#define NB_CFG_DEFAULT_V2_CAP_TX          0     /* off por padrao: bridge TX legado */
#define NB_CFG_DEFAULT_V2_ACT_DEC         1     /* on por padrao: Activity v2 decide com rollback */
#define NB_CFG_DEFAULT_V2_ACT_MIG         0     /* off ate aplicar a migracao one-shot */
#define NB_CFG_DEFAULT_BRIGHTNESS       180     /* ~70% — confortável em ambiente escuro */
#define NB_CFG_DEFAULT_TOUCH_SENS       100     /* 20% acima do baseline. Medido em hardware:
                                                 * proximidade (fio/fita como antena) → fraw converge
                                                 * a ~6% acima do baseline; toque real (dedo no cobre)
                                                 * → fraw chega a 130–200% acima do baseline já no
                                                 * primeiro tick de EMA. Gap enorme; 20% separa com
                                                 * margem confortável. Valores 5, 8 e 15 eram baixos. */
#define NB_CFG_DEFAULT_IDLE_TIMEOUT_S   3600    /* 60 minutos */

#define NB_CFG_DEFAULT_PRES_HOLD_MS     5000U   /* 5 s  — hold antes de LEFT_RECENTLY  */
#define NB_CFG_DEFAULT_PRES_MAYBE_MS    5000U   /* 5 s  — MAYBE_SOMEONE sem confirmação */
#define NB_CFG_DEFAULT_PRES_HEUR_MS    10000U   /* 10 s — heurística offline → PRESENT  */
#define NB_CFG_DEFAULT_PRES_LREC_MS    30000U   /* 30 s — LEFT_RECENTLY → AWAY          */
#define NB_CFG_DEFAULT_PRES_AWAY_MS   120000U   /* 2 min — AWAY → ALONE_SETTLED         */
#define NB_CFG_DEFAULT_PRES_ENGD_MS   180000U   /* 3 min — PRESENT → ENGAGED            */
#define NB_CFG_DEFAULT_PRES_RTN_MS     60000U   /* 1 min — ausência mínima p/ RETURNED  */

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
