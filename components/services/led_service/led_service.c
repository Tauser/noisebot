/*
 * led_service.c — Serviço de LED do NoiseBot (Layer 4)
 *
 * Arquitetura interna:
 *
 *   Estado base (s_base):
 *     Array de flags por NB_LED_BASE_STATE. O estado de maior prioridade
 *     ativo determina a animação renderizada continuamente.
 *
 *   Overlay (s_overlay):
 *     Efeito temporário (touch, blink, fade, heartbeat) que sobrepõe o base.
 *     Tem timeout; ao expirar, s_overlay.type = OVERLAY_NONE e o base retoma.
 *     Ignorado se o base ativo tiver prioridade >= NB_OVERLAY_MAX_PRIORITY.
 *
 *   Dirty detection:
 *     s_last_frame[] guarda o último frame enviado. Só chama led_hal_flush()
 *     quando o frame calculado difere.
 *
 *   Gamma + brilho:
 *     Aplicados no momento de render, antes de enviar ao HAL.
 *     Tabela gamma 2.2 estática de 256 entradas.
 *     Brilho efetivo = min(s_brightness, NB_LED_MAX_BRIGHTNESS).
 */

#include "led_service.h"
#include "led_hal.h"
#include "nb_hw_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>
#include <stddef.h>

static const char *TAG = "nb_led_svc";

/* ── Constantes ──────────────────────────────────────────────────────────── */

/* Brilho máximo permitido para limitar corrente (~80% = 204/255). */
#define NB_LED_MAX_BRIGHTNESS   204U

/* Brilho padrão em operação normal (~25%). */
#define NB_LED_DEFAULT_BRIGHTNESS 64U

/* Brilho do modo noturno (40% do padrão). */
#define NB_LED_NIGHT_BRIGHTNESS 26U

/* Offset de fase entre LED[0] e LED[1] em breathe (ms). */
#define NB_LED_BREATHE_PHASE_OFFSET_MS  200U

/* Prioridade máxima de estado base que BLOQUEIA overlays. */
/* SAFE_MODE (2) e ERROR (3) bloqueiam TOUCH overlay (prioridade 2). */
#define NB_OVERLAY_BLOCK_PRIORITY  NB_LED_BASE_SAFE_MODE

/* ── Tabela gamma 2.2 (256 entradas) ─────────────────────────────────────── */
/*
 * Gerada offline: entry[i] = round(255 * (i/255)^2.2)
 * Corrige a resposta não-linear dos LEDs para brilho visualmente linear.
 */
static const uint8_t k_gamma[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      2,   3,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,
      5,   6,   6,   6,   6,   7,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,
     10,  10,  11,  11,  11,  12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,
     17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  23,  24,  24,  25,
     25,  26,  27,  27,  28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  35,  36,
     37,  38,  39,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  50,
     51,  52,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  66,  67,  68,
     69,  70,  72,  73,  74,  75,  77,  78,  79,  81,  82,  83,  85,  86,  87,  89,
     90,  92,  93,  95,  96,  98,  99, 101, 102, 104, 105, 107, 109, 110, 112, 114,
    115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
    144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175,
    177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
    215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};

/* ── Tipos internos ──────────────────────────────────────────────────────── */

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_FADE,
    OVERLAY_BLINK,
    OVERLAY_FLASH_DECAY,
    OVERLAY_HEARTBEAT,
} overlay_type_t;

typedef enum {
    BASE_ANIM_SOLID = 0,
    BASE_ANIM_BREATHE,
    BASE_ANIM_PULSE_FAST,   /* para ERROR */
} base_anim_type_t;

/* Estado do overlay. */
typedef struct {
    overlay_type_t  type;

    /* FADE */
    nb_led_color_t  fade_from[NB_LED_COUNT];
    nb_led_color_t  fade_to;
    uint32_t        fade_total_ms;
    uint32_t        fade_elapsed_ms;

    /* BLINK */
    uint8_t         blink_count;
    uint8_t         blink_rem;
    bool            blink_on;
    uint32_t        blink_phase_ms;   /* tempo acumulado na fase atual */

    /* FLASH_DECAY */
    float           decay_peak;       /* 0.0–1.0 */
    nb_led_color_t  decay_color;
    uint32_t        decay_elapsed_ms;
    uint32_t        decay_total_ms;

    /* HEARTBEAT */
    uint32_t        hb_elapsed_ms;
} overlay_state_t;

/* Estado do serviço. */
typedef struct {
    bool            initialized;
    bool            base_active[NB_LED_BASE__COUNT];

    /* Animação base — fase de breathe/pulse acumulada por LED. */
    uint32_t        base_phase_ms[NB_LED_COUNT];
    uint32_t        base_period_ms;   /* período do breathe/pulse atual */

    /* Cor manual (usada quando nenhum estado base está ativo). */
    nb_led_color_t  manual_color[NB_LED_COUNT];
    bool            manual_active;

    overlay_state_t overlay;

    uint8_t         brightness;       /* 0–255 */
    bool            night_mode;

    /* Último frame enviado (dirty detection). */
    nb_led_color_t  last_frame[NB_LED_COUNT];

    SemaphoreHandle_t mutex;
} led_service_t;

static led_service_t s_svc = {0};

/* ── Helpers internos ────────────────────────────────────────────────────── */

static inline uint8_t apply_gamma(uint8_t v)
{
    return k_gamma[v];
}

static inline uint8_t scale_u8(uint8_t v, uint8_t scale)
{
    return (uint8_t)(((uint16_t)v * scale) / 255U);
}

/*
 * Aplica brilho efetivo (considera night_mode) e gamma a uma cor.
 * Resultado é o valor a enviar ao HAL.
 */
static nb_led_color_t render_color(nb_led_color_t c)
{
    uint8_t max_br = s_svc.night_mode ? NB_LED_NIGHT_BRIGHTNESS : NB_LED_MAX_BRIGHTNESS;
    uint8_t eff_br = (s_svc.brightness < max_br) ? s_svc.brightness : max_br;

    nb_led_color_t out = {
        .r = apply_gamma(scale_u8(c.r, eff_br)),
        .g = apply_gamma(scale_u8(c.g, eff_br)),
        .b = apply_gamma(scale_u8(c.b, eff_br)),
    };
    return out;
}

/*
 * Render do overlay — aplica apenas gamma, sem escala de brilho.
 * O overlay (touch flash, heartbeat) deve ser sempre visível independente
 * do brilho ambiente configurado. Em night_mode aplica NB_LED_MAX_BRIGHTNESS
 * como cap para não cegar em ambiente escuro.
 */
static nb_led_color_t render_color_overlay(nb_led_color_t c)
{
    if (s_svc.night_mode) {
        nb_led_color_t out = {
            .r = apply_gamma(scale_u8(c.r, NB_LED_NIGHT_BRIGHTNESS)),
            .g = apply_gamma(scale_u8(c.g, NB_LED_NIGHT_BRIGHTNESS)),
            .b = apply_gamma(scale_u8(c.b, NB_LED_NIGHT_BRIGHTNESS)),
        };
        return out;
    }
    nb_led_color_t out = {
        .r = apply_gamma(c.r),
        .g = apply_gamma(c.g),
        .b = apply_gamma(c.b),
    };
    return out;
}

/* Interpola linearmente entre duas cores (t = 0..255). */
static nb_led_color_t lerp_color(nb_led_color_t a, nb_led_color_t b, uint8_t t)
{
    nb_led_color_t out = {
        .r = (uint8_t)(a.r + ((int32_t)(b.r - a.r) * t) / 255),
        .g = (uint8_t)(a.g + ((int32_t)(b.g - a.g) * t) / 255),
        .b = (uint8_t)(a.b + ((int32_t)(b.b - a.b) * t) / 255),
    };
    return out;
}

/* Escala uma cor por um fator 0.0–1.0. */
static nb_led_color_t scale_color(nb_led_color_t c, float f)
{
    if (f <= 0.0f) return NB_LED_COLOR_BLACK;
    if (f >= 1.0f) return c;
    nb_led_color_t out = {
        .r = (uint8_t)(c.r * f),
        .g = (uint8_t)(c.g * f),
        .b = (uint8_t)(c.b * f),
    };
    return out;
}

/* Retorna o estado base de maior prioridade ativo. -1 se nenhum. */
static int active_base_priority(void)
{
    for (int p = (int)NB_LED_BASE__COUNT - 1; p >= 0; p--) {
        if (s_svc.base_active[p]) return p;
    }
    return -1;
}

/* ── Render do estado base para o índice de LED dado ─────────────────────── */

/*
 * sin² normalizado: período = base_period_ms, fase = phase_ms.
 * Retorna 0.0–1.0.
 */
static float breathe_factor(uint32_t phase_ms, uint32_t period_ms)
{
    if (period_ms == 0) return 1.0f;
    float t = (float)(phase_ms % period_ms) / (float)period_ms;
    float s = sinf(3.14159265f * t);
    return s * s;   /* sin² = suave na entrada e saída */
}

/* Heartbeat base não usa essa função — apenas breathe e pulse. */

/*
 * Calcula a cor do estado base para o LED `led_idx`.
 * priority = resultado de active_base_priority().
 */
static nb_led_color_t compute_base_color(int priority, uint8_t led_idx)
{
    uint32_t phase = s_svc.base_phase_ms[led_idx];
    uint32_t period = s_svc.base_period_ms;

    switch ((nb_led_base_state_t)priority) {
    case NB_LED_BASE_IDLE: {
        /* Breathe quente lento, brilho 30–100% */
        float f = 0.3f + 0.7f * breathe_factor(phase, period);
        return scale_color(NB_LED_WARM_WHITE, f);
    }
    case NB_LED_BASE_BOOT: {
        /* Pulso branco suave, brilho 20–100% */
        float f = 0.2f + 0.8f * breathe_factor(phase, period);
        return scale_color(NB_LED_COOL_WHITE, f);
    }
    case NB_LED_BASE_SAFE_MODE: {
        /* Laranja pulsante lento, brilho 40–100% */
        float f = 0.4f + 0.6f * breathe_factor(phase, period);
        return scale_color(NB_LED_ORANGE, f);
    }
    case NB_LED_BASE_ERROR: {
        /* Vermelho pulsante rápido, brilho 0–100% */
        float f = breathe_factor(phase, period);
        return scale_color(NB_LED_RED, f);
    }
    case NB_LED_BASE_MEDITATION: {
        /* Âmbar muito lento, brilho 20–80% */
        float f = 0.2f + 0.6f * breathe_factor(phase, period);
        return scale_color(NB_LED_AMBER, f);
    }
    case NB_LED_BASE_SILENT_COMPANY: {
        /* Brasa quase apagada, brilho 10–30% */
        float f = 0.1f + 0.2f * breathe_factor(phase, period);
        return scale_color(NB_LED_EMBER, f);
    }
    default:
        return NB_LED_COLOR_BLACK;
    }
}

/* Período de breathe padrão por estado base. */
static uint32_t default_period_for_base(nb_led_base_state_t state)
{
    switch (state) {
    case NB_LED_BASE_IDLE:      return 4000U;
    case NB_LED_BASE_BOOT:      return 1200U;
    case NB_LED_BASE_SAFE_MODE: return 2500U;
    case NB_LED_BASE_ERROR:          return 600U;
    case NB_LED_BASE_MEDITATION:     return 6000U;
    case NB_LED_BASE_SILENT_COMPANY: return 8000U;
    default:                         return 2000U;
    }
}

/* ── Render do overlay ───────────────────────────────────────────────────── */

/*
 * Avança o overlay em dt_ms.
 * Retorna false se o overlay expirou (type ← OVERLAY_NONE já feito).
 */
static bool advance_overlay(uint32_t dt_ms)
{
    overlay_state_t *ov = &s_svc.overlay;

    switch (ov->type) {
    case OVERLAY_NONE:
        return false;

    case OVERLAY_FADE:
        ov->fade_elapsed_ms += dt_ms;
        if (ov->fade_elapsed_ms >= ov->fade_total_ms) {
            ov->type = OVERLAY_NONE;
            return false;
        }
        return true;

    case OVERLAY_BLINK: {
        uint32_t half = 150U; /* 150ms on, 150ms off */
        ov->blink_phase_ms += dt_ms;
        while (ov->blink_phase_ms >= half) {
            ov->blink_phase_ms -= half;
            ov->blink_on = !ov->blink_on;
            if (!ov->blink_on && ov->blink_rem > 0) {
                ov->blink_rem--;
                if (ov->blink_rem == 0) {
                    ov->type = OVERLAY_NONE;
                    return false;
                }
            }
        }
        return true;
    }

    case OVERLAY_FLASH_DECAY:
        ov->decay_elapsed_ms += dt_ms;
        if (ov->decay_elapsed_ms >= ov->decay_total_ms) {
            ov->type = OVERLAY_NONE;
            return false;
        }
        /* Decaimento exponencial: peak * e^(-4 * t/total) */
        ov->decay_peak = expf(-4.0f * (float)ov->decay_elapsed_ms
                                     / (float)ov->decay_total_ms);
        return true;

    case OVERLAY_HEARTBEAT: {
        /*
         * Padrão: dois batimentos + pausa.
         * t=0–120ms:   subida/descida (1º beat)
         * t=120–300ms: subida/descida (2º beat)
         * t=300–900ms: pausa
         * total = 900ms, depois expira.
         */
        ov->hb_elapsed_ms += dt_ms;
        if (ov->hb_elapsed_ms >= 900U) {
            ov->type = OVERLAY_NONE;
            return false;
        }
        return true;
    }

    default:
        ov->type = OVERLAY_NONE;
        return false;
    }
}

/* Calcula cor do overlay para um LED (independente do índice). */
static nb_led_color_t compute_overlay_color(void)
{
    const overlay_state_t *ov = &s_svc.overlay;

    switch (ov->type) {
    case OVERLAY_FADE: {
        /* Usa fade_from do LED 0 como referência (fade_to é igual para ambos). */
        uint8_t t = (uint8_t)((ov->fade_elapsed_ms * 255U) / ov->fade_total_ms);
        return lerp_color(ov->fade_from[0], ov->fade_to, t);
    }

    case OVERLAY_BLINK:
        return ov->blink_on ? NB_LED_COOL_WHITE : NB_LED_COLOR_BLACK;

    case OVERLAY_FLASH_DECAY:
        return scale_color(ov->decay_color, ov->decay_peak);

    case OVERLAY_HEARTBEAT: {
        uint32_t t = ov->hb_elapsed_ms;
        float f = 0.0f;
        if (t < 120U) {
            /* 1º beat: sin(π * t / 120) */
            f = sinf(3.14159265f * (float)t / 120.0f);
        } else if (t < 300U) {
            /* 2º beat: sin(π * (t-120) / 180) — ligeiramente mais largo */
            f = 0.8f * sinf(3.14159265f * (float)(t - 120U) / 180.0f);
        }
        /* t >= 300 = pausa (f=0) */
        return scale_color(NB_LED_WARM_WHITE, f);
    }

    default:
        return NB_LED_COLOR_BLACK;
    }
}

/* ── update interno (já dentro do mutex) ────────────────────────────────── */

static void service_tick(uint32_t dt_ms)
{
    int priority = active_base_priority();
    bool overlay_active = advance_overlay(dt_ms);

    /* Determina se o overlay tem permissão de sobrepor o estado base atual. */
    bool overlay_blocked = (priority >= (int)NB_OVERLAY_BLOCK_PRIORITY)
                         && (s_svc.overlay.type == OVERLAY_FLASH_DECAY);
    if (overlay_blocked) {
        overlay_active = false;
    }

    /* Avança fase de breathe para cada LED. */
    if (priority >= 0) {
        uint32_t period = s_svc.base_period_ms;
        for (int i = 0; i < NB_LED_COUNT; i++) {
            s_svc.base_phase_ms[i] = (s_svc.base_phase_ms[i] + dt_ms) % (period + 1U);
        }
    }

    /* Computa frame final. */
    nb_led_color_t frame[NB_LED_COUNT];
    bool dirty = false;

    for (int i = 0; i < NB_LED_COUNT; i++) {
        if (overlay_active) {
            frame[i] = render_color_overlay(compute_overlay_color());
        } else {
            nb_led_color_t c;
            if (priority >= 0) {
                c = compute_base_color(priority, (uint8_t)i);
            } else if (s_svc.manual_active) {
                c = s_svc.manual_color[i];
            } else {
                c = NB_LED_COLOR_BLACK;
            }
            frame[i] = render_color(c);
        }

        if (frame[i].r != s_svc.last_frame[i].r ||
            frame[i].g != s_svc.last_frame[i].g ||
            frame[i].b != s_svc.last_frame[i].b) {
            dirty = true;
        }
    }

    if (!dirty) return;

    for (int i = 0; i < NB_LED_COUNT; i++) {
        led_hal_set_pixel((uint8_t)i, frame[i]);
        s_svc.last_frame[i] = frame[i];
    }
    led_hal_flush();
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t led_service_init(void)
{
    if (s_svc.initialized) {
        ESP_LOGW(TAG, "led_service_init chamado mais de uma vez");
        return ESP_OK;
    }

    s_svc.mutex = xSemaphoreCreateMutex();
    if (s_svc.mutex == NULL) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = led_hal_init();
    if (err != ESP_OK) return err;

    s_svc.brightness = NB_LED_DEFAULT_BRIGHTNESS;
    s_svc.night_mode = false;

    /* Estado inicial: BOOT ativo. */
    s_svc.base_active[NB_LED_BASE_BOOT] = true;
    s_svc.base_period_ms = default_period_for_base(NB_LED_BASE_BOOT);

    /* Offset de fase entre LEDs para breathe. */
    s_svc.base_phase_ms[0] = 0;
    s_svc.base_phase_ms[1] = NB_LED_BREATHE_PHASE_OFFSET_MS;

    memset(&s_svc.overlay, 0, sizeof(s_svc.overlay));
    memset(s_svc.last_frame, 0xFF, sizeof(s_svc.last_frame)); /* força primeiro flush */

    s_svc.initialized = true;
    ESP_LOGI(TAG, "led_service OK");
    return ESP_OK;
}

void led_service_update(uint32_t dt_ms)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    service_tick(dt_ms);
    xSemaphoreGive(s_svc.mutex);
}

void led_set_color(uint8_t idx, nb_led_color_t color)
{
    if (!s_svc.initialized || idx >= NB_LED_COUNT) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    s_svc.manual_color[idx] = color;
    s_svc.manual_active = true;
    s_svc.overlay.type = OVERLAY_NONE;

    xSemaphoreGive(s_svc.mutex);
}

void led_set_all(nb_led_color_t color)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    for (int i = 0; i < NB_LED_COUNT; i++) {
        s_svc.manual_color[i] = color;
    }
    s_svc.manual_active = true;
    s_svc.overlay.type = OVERLAY_NONE;

    xSemaphoreGive(s_svc.mutex);
}

void led_set_brightness(uint8_t brightness)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_svc.brightness = brightness;
    xSemaphoreGive(s_svc.mutex);
}

void led_fade_to(nb_led_color_t color, uint32_t ms)
{
    if (!s_svc.initialized || ms == 0) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    overlay_state_t *ov = &s_svc.overlay;
    /* Captura cor atual como ponto de partida. */
    for (int i = 0; i < NB_LED_COUNT; i++) {
        ov->fade_from[i] = s_svc.last_frame[i];
    }
    ov->fade_to          = color;
    ov->fade_total_ms    = ms;
    ov->fade_elapsed_ms  = 0;
    ov->type             = OVERLAY_FADE;

    xSemaphoreGive(s_svc.mutex);
}

void led_blink(uint8_t count)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    if (count == 0) {
        s_svc.overlay.type = OVERLAY_NONE;
    } else {
        s_svc.overlay.type          = OVERLAY_BLINK;
        s_svc.overlay.blink_count   = count;
        s_svc.overlay.blink_rem     = count;
        s_svc.overlay.blink_on      = true;
        s_svc.overlay.blink_phase_ms = 0;
    }

    xSemaphoreGive(s_svc.mutex);
}

void led_breathe(uint32_t period_ms)
{
    if (!s_svc.initialized || period_ms == 0) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    s_svc.base_period_ms = period_ms;
    /* Garante que IDLE está ativo para usar o breathe. */
    s_svc.base_active[NB_LED_BASE_IDLE] = true;
    s_svc.manual_active = false;

    xSemaphoreGive(s_svc.mutex);
}

void led_base_set(nb_led_base_state_t state, bool active)
{
    if (!s_svc.initialized || state >= NB_LED_BASE__COUNT) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    s_svc.base_active[state] = active;
    s_svc.manual_active = false;

    /* Atualiza período para o novo estado de maior prioridade. */
    int p = active_base_priority();
    if (p >= 0) {
        s_svc.base_period_ms = default_period_for_base((nb_led_base_state_t)p);
        /* Reseta fase ao trocar de estado base para evitar artefatos visuais. */
        if (active) {
            s_svc.base_phase_ms[0] = 0;
            s_svc.base_phase_ms[1] = NB_LED_BREATHE_PHASE_OFFSET_MS;
        }
    }

    xSemaphoreGive(s_svc.mutex);
}

void led_effect_touch(void)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    /* Ignorado se estado de alta prioridade ativo. */
    int p = active_base_priority();
    if (p < (int)NB_OVERLAY_BLOCK_PRIORITY) {
        s_svc.overlay.type             = OVERLAY_FLASH_DECAY;
        s_svc.overlay.decay_color      = NB_LED_TOUCH_WARM;
        s_svc.overlay.decay_peak       = 1.0f;
        s_svc.overlay.decay_elapsed_ms = 0;
        s_svc.overlay.decay_total_ms   = 600U;
    }

    xSemaphoreGive(s_svc.mutex);
}

void led_effect_heartbeat(void)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    int p = active_base_priority();
    if (p < (int)NB_OVERLAY_BLOCK_PRIORITY) {
        s_svc.overlay.type           = OVERLAY_HEARTBEAT;
        s_svc.overlay.hb_elapsed_ms  = 0;
    }

    xSemaphoreGive(s_svc.mutex);
}

void led_effect_beat(void)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    int p = active_base_priority();
    if (p < (int)NB_OVERLAY_BLOCK_PRIORITY) {
        s_svc.overlay.type             = OVERLAY_FLASH_DECAY;
        s_svc.overlay.decay_color      = NB_LED_BEAT_BLUE;
        s_svc.overlay.decay_peak       = 0.55f;
        s_svc.overlay.decay_elapsed_ms = 0;
        s_svc.overlay.decay_total_ms   = 150U;
    }

    xSemaphoreGive(s_svc.mutex);
}

void led_set_night_mode(bool enable)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_svc.night_mode = enable;
    xSemaphoreGive(s_svc.mutex);
}
