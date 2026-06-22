#include "nb_head_blink.hpp"

#include <cmath>

namespace {

/* Mesmas constantes de expression_service.cpp (main) -- ver comentário em
 * nb_head_blink.hpp sobre a decisão de manter isso local ao head. */
constexpr float BLINK_MEAN_MS = 5000.0f;
constexpr float BLINK_MIN_MS = 1800.0f;
constexpr int64_t BLINK_CLOSE_US = 55000LL;
constexpr int64_t BLINK_HOLD_US = 25000LL;
constexpr int64_t BLINK_OPEN_US = 80000LL;
constexpr uint32_t BLINK_ASYM_THRESH = 52u;       /* ~20% de 256 */
constexpr int64_t BLINK_ASYM_MIN_US = 20000LL;
constexpr int64_t BLINK_ASYM_RANGE_US = 80000LL;
constexpr int64_t DOUBLE_BLINK_MIN_US = 400000LL;
constexpr int64_t DOUBLE_BLINK_RANGE_US = 200000LL;
constexpr uint32_t DOUBLE_BLINK_THRESH = 31u;     /* ~12% de 256 */

enum class EyeState { Idle, Closing, Closed, Opening };

struct EyeBlink {
    EyeState state = EyeState::Idle;
    float phase = 0.0f;
    int64_t t0 = 0;
};

EyeBlink s_eye[2]; /* 0=esquerdo, 1=direito */
int64_t s_next_blink_us = 0;
bool s_double_blink_pending = false;
int64_t s_double_blink_us = 0;
bool s_initialized = false;
uint32_t s_rng_state = 0x12345678u;

uint32_t xorshift_next()
{
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    return s_rng_state;
}

int64_t poisson_blink_delay_us()
{
    const float u = (float)(xorshift_next() % 100000u) / 100000.0f;
    const float delay_ms = -BLINK_MEAN_MS * std::log(1.0f - u * 0.999f);
    const float clipped = (delay_ms < BLINK_MIN_MS) ? BLINK_MIN_MS : delay_ms;
    return (int64_t)(clipped * 1000.0f);
}

void trigger_bilateral(int64_t now_us)
{
    s_eye[0].state = EyeState::Closing;
    s_eye[0].t0 = now_us;
    s_eye[1].state = EyeState::Closing;
    s_eye[1].t0 = now_us;

    if ((xorshift_next() & 0xffu) < BLINK_ASYM_THRESH) {
        const int late_eye = (int)(xorshift_next() & 1u);
        const int64_t delay = BLINK_ASYM_MIN_US +
            (int64_t)(xorshift_next() % (uint32_t)BLINK_ASYM_RANGE_US);
        s_eye[late_eye].t0 = now_us + delay;
    }
}

void update_eye(EyeBlink &eye, int64_t now_us, bool is_left)
{
    switch (eye.state) {
        case EyeState::Idle:
            eye.phase = 0.0f;
            break;
        case EyeState::Closing: {
            const int64_t dt = now_us - eye.t0;
            if (dt < 0) {
                eye.phase = 0.0f;
                break;
            }
            eye.phase = (dt >= BLINK_CLOSE_US)
                            ? 1.0f
                            : (float)dt / (float)BLINK_CLOSE_US;
            if (dt >= BLINK_CLOSE_US) {
                eye.state = EyeState::Closed;
                eye.t0 = now_us;
            }
            break;
        }
        case EyeState::Closed: {
            eye.phase = 1.0f;
            if (now_us - eye.t0 >= BLINK_HOLD_US) {
                eye.state = EyeState::Opening;
                eye.t0 = now_us;
            }
            break;
        }
        case EyeState::Opening: {
            const int64_t dt = now_us - eye.t0;
            eye.phase = (dt >= BLINK_OPEN_US)
                            ? 0.0f
                            : 1.0f - (float)dt / (float)BLINK_OPEN_US;
            if (dt >= BLINK_OPEN_US) {
                eye.state = EyeState::Idle;
                eye.phase = 0.0f;
                if (is_left) {
                    s_next_blink_us = now_us + poisson_blink_delay_us();
                    if (!s_double_blink_pending &&
                        (xorshift_next() & 0xffu) < DOUBLE_BLINK_THRESH) {
                        s_double_blink_pending = true;
                        s_double_blink_us =
                            now_us + DOUBLE_BLINK_MIN_US +
                            (int64_t)(xorshift_next() %
                                      (uint32_t)DOUBLE_BLINK_RANGE_US);
                    }
                }
            }
            break;
        }
    }
}

}  // namespace

void nb_head_blink_init(int64_t now_us)
{
    s_eye[0] = EyeBlink{};
    s_eye[1] = EyeBlink{};
    s_next_blink_us = now_us + poisson_blink_delay_us();
    s_double_blink_pending = false;
    s_initialized = true;
}

nb_head_blink_mult_t nb_head_blink_tick(int64_t now_us)
{
    if (!s_initialized) {
        nb_head_blink_init(now_us);
    }

    if (s_eye[0].state == EyeState::Idle &&
        s_eye[1].state == EyeState::Idle && now_us >= s_next_blink_us) {
        trigger_bilateral(now_us);
    }
    if (s_double_blink_pending && s_eye[0].state == EyeState::Idle &&
        s_eye[1].state == EyeState::Idle && now_us >= s_double_blink_us) {
        trigger_bilateral(now_us);
        s_double_blink_pending = false;
    }

    update_eye(s_eye[0], now_us, true);
    update_eye(s_eye[1], now_us, false);

    const bool active = s_eye[0].state != EyeState::Idle ||
                        s_eye[1].state != EyeState::Idle;
    return {1.0f - s_eye[0].phase, 1.0f - s_eye[1].phase, active};
}
