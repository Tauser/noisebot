/*
 * persona_service.c — Personalidade emergente (Layer 7)
 *
 * NVS namespace "nb_persona", chaves: "p_warmth", "p_energy",
 * "p_curiosity", "p_trust" — float armazenado como uint32_t (bit-cast).
 * O mesmo namespace guarda o perfil offline-first do usuário atual.
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
#define KEY_USER_ID   "user_id"
#define KEY_USER_NAME "user_name"
#define KEY_RELATION  "relation"
#define KEY_LANGUAGE  "language"
#define KEY_ROBOT     "robot_name"
#define KEY_MODE      "mode"
#define KEY_STYLE     "style"

#define DEFAULT_USER_ID    "owner"
#define DEFAULT_USER_NAME  "Owner"
#define DEFAULT_RELATION   "owner"
#define DEFAULT_LANGUAGE   "pt-BR"
#define DEFAULT_ROBOT      "NoiseBot"
#define DEFAULT_MODE       "companion"
#define DEFAULT_STYLE      "direct_warm"

/* ── Estado ──────────────────────────────────────────────────────────────── */

static nvs_handle_t s_h;
static bool         s_initialized;
static bool         s_nvs_ready;

static float s_warmth    = 0.0f;
static float s_energy    = 0.0f;
static float s_curiosity = 1.0f;   /* curioso por padrão */
static float s_trust     = 0.0f;
static float s_saved_warmth;
static float s_saved_energy;
static float s_saved_curiosity;
static float s_saved_trust;
static nb_user_profile_t s_profile = {
    .user_id = DEFAULT_USER_ID,
    .display_name = DEFAULT_USER_NAME,
    .relationship = DEFAULT_RELATION,
    .language = DEFAULT_LANGUAGE,
    .robot_nickname = DEFAULT_ROBOT,
    .persona_mode = DEFAULT_MODE,
    .interaction_style = DEFAULT_STYLE,
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static float clampf(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static bool fits_field(const char *src, size_t max_len)
{
    if (src == NULL) {
        return false;
    }

    for (size_t i = 0; i < max_len; ++i) {
        if (src[i] == '\0') {
            return true;
        }
    }
    return false;
}

static esp_err_t copy_field(char *dst, size_t dst_len, const char *src)
{
    if (!fits_field(src, dst_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(dst, src, strlen(src) + 1U);
    return ESP_OK;
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

static void load_str_from_nvs(const char *key, char *dst, size_t dst_len, const char *default_val)
{
    size_t required = dst_len;
    esp_err_t err = nvs_get_str(s_h, key, dst, &required);
    if (err == ESP_OK) {
        return;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str(\"%s\"): %s", key, esp_err_to_name(err));
    }

    (void)copy_field(dst, dst_len, default_val);
}

static void load_profile_from_nvs(void)
{
    load_str_from_nvs(KEY_USER_ID,
                      s_profile.user_id,
                      sizeof(s_profile.user_id),
                      DEFAULT_USER_ID);
    load_str_from_nvs(KEY_USER_NAME,
                      s_profile.display_name,
                      sizeof(s_profile.display_name),
                      DEFAULT_USER_NAME);
    load_str_from_nvs(KEY_RELATION,
                      s_profile.relationship,
                      sizeof(s_profile.relationship),
                      DEFAULT_RELATION);
    load_str_from_nvs(KEY_LANGUAGE,
                      s_profile.language,
                      sizeof(s_profile.language),
                      DEFAULT_LANGUAGE);
    load_str_from_nvs(KEY_ROBOT,
                      s_profile.robot_nickname,
                      sizeof(s_profile.robot_nickname),
                      DEFAULT_ROBOT);
    load_str_from_nvs(KEY_MODE,
                      s_profile.persona_mode,
                      sizeof(s_profile.persona_mode),
                      DEFAULT_MODE);
    load_str_from_nvs(KEY_STYLE,
                      s_profile.interaction_style,
                      sizeof(s_profile.interaction_style),
                      DEFAULT_STYLE);
}

static void save_to_nvs(void)
{
    if (!s_nvs_ready) {
        return;
    }

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

static esp_err_t save_profile_to_nvs(const nb_user_profile_t *profile)
{
    if (!s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t err;
    if ((err = nvs_set_str(s_h, KEY_USER_ID, profile->user_id)) != ESP_OK) return err;
    if ((err = nvs_set_str(s_h, KEY_USER_NAME, profile->display_name)) != ESP_OK) return err;
    if ((err = nvs_set_str(s_h, KEY_RELATION, profile->relationship)) != ESP_OK) return err;
    if ((err = nvs_set_str(s_h, KEY_LANGUAGE, profile->language)) != ESP_OK) return err;
    if ((err = nvs_set_str(s_h, KEY_ROBOT, profile->robot_nickname)) != ESP_OK) return err;
    if ((err = nvs_set_str(s_h, KEY_MODE, profile->persona_mode)) != ESP_OK) return err;
    if ((err = nvs_set_str(s_h, KEY_STYLE, profile->interaction_style)) != ESP_OK) return err;
    return nvs_hal_commit(s_h);
}

static void publish_user_context_updated(void)
{
    nb_event_t ev = {
        .type = NB_EVT_USER_CONTEXT_UPDATED,
        .data.ptr = &s_profile,
    };
    nb_event_publish_async(&ev);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t persona_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = nvs_hal_open(PERSONA_NS, NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open falhou (%s) — persona em RAM only", esp_err_to_name(err));
        s_initialized = true;
        publish_user_context_updated();
        return ESP_OK;
    }

    s_nvs_ready = true;
    load_from_nvs();
    load_profile_from_nvs();
    s_initialized = true;

    if (ltm_get_total_sessions() > 0u) {
        persona_service_refresh();
    }

    publish_user_context_updated();

    ESP_LOGI(TAG, "persona: W=%.2f E=%.2f C=%.2f T=%.2f user=%s style=%s",
             s_warmth, s_energy, s_curiosity, s_trust,
             s_profile.display_name,
             s_profile.interaction_style);
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

const nb_user_profile_t *persona_get_current_user_profile(void)
{
    return &s_profile;
}

esp_err_t persona_set_current_user_profile(const nb_user_profile_t *profile)
{
    if (!s_initialized || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nb_user_profile_t next = s_profile;
    esp_err_t err;

    if ((err = copy_field(next.user_id,
                          sizeof(next.user_id),
                          profile->user_id)) != ESP_OK) return err;
    if ((err = copy_field(next.display_name,
                          sizeof(next.display_name),
                          profile->display_name)) != ESP_OK) return err;
    if ((err = copy_field(next.relationship,
                          sizeof(next.relationship),
                          profile->relationship)) != ESP_OK) return err;
    if ((err = copy_field(next.language,
                          sizeof(next.language),
                          profile->language)) != ESP_OK) return err;
    if ((err = copy_field(next.robot_nickname,
                          sizeof(next.robot_nickname),
                          profile->robot_nickname)) != ESP_OK) return err;
    if ((err = copy_field(next.persona_mode,
                          sizeof(next.persona_mode),
                          profile->persona_mode)) != ESP_OK) return err;
    if ((err = copy_field(next.interaction_style,
                          sizeof(next.interaction_style),
                          profile->interaction_style)) != ESP_OK) return err;

    err = save_profile_to_nvs(&next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_profile_to_nvs: %s", esp_err_to_name(err));
        return err;
    }

    s_profile = next;
    publish_user_context_updated();
    ESP_LOGI(TAG, "user profile: id=%s name=%s mode=%s style=%s",
             s_profile.user_id,
             s_profile.display_name,
             s_profile.persona_mode,
             s_profile.interaction_style);
    return ESP_OK;
}

esp_err_t persona_set_interaction_style(const char *persona_mode,
                                        const char *interaction_style)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nb_user_profile_t next = s_profile;
    esp_err_t err;

    if ((err = copy_field(next.persona_mode,
                          sizeof(next.persona_mode),
                          persona_mode)) != ESP_OK) return err;
    if ((err = copy_field(next.interaction_style,
                          sizeof(next.interaction_style),
                          interaction_style)) != ESP_OK) return err;

    return persona_set_current_user_profile(&next);
}
