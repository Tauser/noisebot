#pragma once

#include <stdint.h>

/*
 * Blink autônomo do head (DM2.9): porte fiel do estado/timing de
 * expression_service.cpp (main) -- mesmas constantes (BLINK_MEAN_MS,
 * BLINK_MIN_MS, close/hold/open, assimetria, duplo blink).
 *
 * Decisão de arquitetura: blink fica inteiramente local ao head (não
 * atravessa o link), mesma lógica de "main decide o quê (expressão),
 * head decide como renderiza" já usada pela interpolação de DM2.8. Não
 * precisa de sincronismo exato com o blink local do main (que é o
 * fallback legado, sem display físico no perfil dual-MCU alvo) -- só
 * precisa ser plausível e usar a mesma distribuição de timing.
 *
 * Uso: chamar nb_head_blink_tick(now_us) uma vez por frame e multiplicar
 * open_l/open_r da face resolvida pelos multiplicadores retornados
 * (1.0 = totalmente aberto, 0.0 = totalmente fechado).
 */
struct nb_head_blink_mult_t {
    float open_mult_l;
    float open_mult_r;
    bool active; /* true enquanto qualquer olho nao esta em IDLE -- usado
                  * pra decidir se vale a pena redesenhar/empurrar SPI. */
};

void nb_head_blink_init(int64_t now_us);
nb_head_blink_mult_t nb_head_blink_tick(int64_t now_us);
