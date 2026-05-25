/*
 * persona_service.c — Personalidade emergente (Layer 7)
 *
 * NVS namespace "nb_persona", chaves: "p_warmth", "p_energy",
 * "p_curiosity", "p_trust" — float armazenado como uint32_t (bit-cast).
 *
 * Refresh só atualiza NVS quando sessions > 0 para preservar valores
 * anteriores quando o SD falha no boot.
 */

#include "persona_service.h"

#include "long_term_memory.h"
#include "nvs_hal.h"
#include "event_bus.h"
#include "nb_events.h"

#include "esp_log.h"

#include <math.h>
#include <string.h>

#define TAG "nb_persona"

#define PERSONA_NS    "nb_persona"
#define KEY_WARMTH    "p_warmth"
#define KEY_ENERGY    "p_energy"
#define KEY_CURIOSITY "p_curiosity"
#define KEY_TRUST     "p_trust"

/* ── Estado ──────────────────────────────────────────────────────────────── */

static nvs_handle_t s_h;
static bool         s_initialized;

static float s_warmth    = 0.0f;
static float s_energy    = 0.0f;
static float s_curiosity = 1.0f;   /* curioso por padrão */
static float s_trust     = 0.0f;
static float s_saved_warmth;
static float s_saved_energy;
static float s_saved_curiosity;
static float s_saved_trust;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static float clampf(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* float ↔ uint32 via bit-cast (sem UB em C17 com memcpy). */
static uint32_t f2u(float f)   { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    u2f(uint32_t u){ float f;    memcpy(&f, &u, 4); return f; }

static void load_from_nvs(void)
{
    s_warmth    = clampf(u2f(nvs_hal_get_u32(s_h, KEY_WARMTH,    f2u(s_warmth))));
    s_energy    = clampf(u2f(nvs_hal_get_u32(s_h, KEY_ENERGY,    f2u(s_energy))));
    s_curiosity = clampf(u2f(nvs_hal_get_u32(s_h, KEY_CURIOSITY, f2u(s_curiosity))));
    s_trust     = clampf(u2f(nvs_hal_get_u32(s_h, KEY_TRUST,     f2u(s_trust))));
    s_saved_warmth    = s_warmth;
    s_saved_energy    = s_energy;
    s_saved_curiosity = s_curiosity;
    s_saved_trust     = s_trust;
}

static void save_to_nvs(void)
{
    if (fabsf(s_warmth - s_saved_warmth) < 0.001f &&
        fabsf(s_energy - s_saved_energy) < 0.001f &&
        fabsf(s_curiosity - s_saved_curiosity) < 0.001f &&
        fabsf(s_trust - s_saved_trust) < 0.001f) {
        return;
    }

    nvs_hal_set_u32(s_h, KEY_WARMTH,    f2u(s_warmth));
    nvs_hal_set_u32(s_h, KEY_ENERGY,    f2u(s_energy));
    nvs_hal_set_u32(s_h, KEY_CURIOSITY, f2u(s_curiosity));
    nvs_hal_set_u32(s_h, KEY_TRUST,     f2u(s_trust));
    nvs_hal_commit(s_h);
    s_saved_warmth    = s_warmth;
    s_saved_energy    = s_energy;
    s_saved_curiosity = s_curiosity;
    s_saved_trust     = s_trust;
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t persona_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = nvs_hal_open(PERSONA_NS, NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open falhou (%s) — persona em RAM only", esp_err_to_name(err));
        s_initialized = true;
        return ESP_OK;
    }

    load_from_nvs();
    s_initialized = true;

    if (ltm_get_total_sessions() > 0u) {
        persona_service_refresh();
    }

    ESP_LOGI(TAG, "persona: W=%.2f E=%.2f C=%.2f T=%.2f",
             s_warmth, s_energy, s_curiosity, s_trust);
    return ESP_OK;
}

void persona_service_refresh(void)
{
    if (!s_initialized) return;

    uint32_t touches   = ltm_get_total_touch_count();
    uint32_t sessions  = ltm_get_total_sessions();
    if (sessions == 0u) return;   /* LTM vazio → manter NVS intacto */

    uint32_t total_voice = ltm_get_total_voice_count();
    uint32_t sleep_cnt   = ltm_count_iact(LTM_IACT_SLEEP);

    float total = (float)sessions;

    s_warmth = 1.0f - expf(-(float)touches / 50.0f);

    /* Energy: média cumulativa de voz por sessão.
     * Usa total_voice_count (acumulado permanente) em vez do ring buffer de 200
     * entradas, que era dominado por toques e zerava energy quando havia
     * períodos sem interação por voz.
     * Escala: 3 eventos de voz/sessão → energy = 1.0 */
    s_energy = clampf((float)total_voice / fmaxf(1.0f, (float)sessions * 3.0f));

    s_curiosity = clampf(1.0f - ((float)sleep_cnt / total) * 1.5f);
    s_trust     = fminf(s_warmth, clampf((float)sessions / 20.0f));

    save_to_nvs();

    nb_event_t ev = { .type = NB_EVT_PERSONA_REFRESHED };
    nb_event_publish_async(&ev);

    ESP_LOGD(TAG, "refresh: W=%.2f E=%.2f C=%.2f T=%.2f (touches=%lu sessions=%lu)",
             s_warmth, s_energy, s_curiosity, s_trust,
             (unsigned long)touches, (unsigned long)sessions);
}

float persona_get_warmth(void)    { return s_warmth;    }
float persona_get_energy(void)    { return s_energy;    }
float persona_get_curiosity(void) { return s_curiosity; }
float persona_get_trust(void)     { return s_trust;     }
