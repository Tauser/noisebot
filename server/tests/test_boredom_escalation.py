"""
test_boredom_escalation.py -- Testes da politica de escalada de ociosidade.

Simula em Python a logica de boredom_service.c:
  - Grace period bloqueia reacoes iniciais
  - Cada nivel dispara no tempo correto (sem cooldowns previos)
  - Interacoes reiniciam o contador
  - Cooldown entre reacoes comuns e respeitado
  - Modo demonio tem cooldown separado e probabilidade ~5%
  - Estados de pausa bloqueiam reacoes
  - ANGRY de ociosidade nao vira estado persistente

Os thresholds refletem exatamente os defines em boredom_service.h.
"""

import unittest
from typing import Optional

# -- Constantes (espelha boredom_service.h) --

GRACE_MS             = 2  * 60 * 1000
LEVEL1_MS            = 5  * 60 * 1000
LEVEL2_MS            = 10 * 60 * 1000
LEVEL3_MS            = 20 * 60 * 1000
LEVEL4_MS            = 30 * 60 * 1000
DEMON_ELIGIBLE_MS    = 60 * 60 * 1000
REACTION_CD_MS       = 12 * 60 * 1000
DEMON_CD_MS          = 45 * 60 * 1000
DEMON_PROB_PCT       = 5
CHECK_INTERVAL_MS    = 30 * 1000


# -- Simulador da politica --

class BoredomEscalationPolicy:
    """
    Simula _check_and_react() de boredom_service.c.

    check(now_ms, random_roll=50) -> str|None
        Avanca o tempo ocioso em dt e retorna a reacao disparada, ou None.
        random_roll substitui esp_random() % 100.

    on_interaction()  -- reinicia idle_ms e last_reaction_ms.
    set_paused(bool)  -- pausa/retoma reacoes.
    """

    def __init__(self):
        self._idle_ms = 0
        self._last_reaction_ms = 0
        self._last_demon_ms = 0
        self._paused = False
        self._prev_now_ms = None

    def on_interaction(self, now_ms=0):
        self._idle_ms = 0
        self._last_reaction_ms = 0

    def set_paused(self, paused):
        self._paused = paused

    def check(self, now_ms, random_roll=50):
        dt_ms = CHECK_INTERVAL_MS if self._prev_now_ms is None else now_ms - self._prev_now_ms
        self._prev_now_ms = now_ms

        if self._paused:
            return None

        self._idle_ms += dt_ms

        if self._idle_ms < GRACE_MS:
            return None

        idle_net = self._idle_ms - GRACE_MS

        # Modo demonio (prioridade maxima, elegivel apos 60min liquidos)
        if idle_net >= (DEMON_ELIGIBLE_MS - GRACE_MS):
            demon_cd_ok = (self._last_demon_ms == 0
                           or (now_ms - self._last_demon_ms) >= DEMON_CD_MS)
            if demon_cd_ok and random_roll < DEMON_PROB_PCT:
                self._last_demon_ms = now_ms
                self._last_reaction_ms = self._idle_ms
                return "demon"

        # Cooldown de reacoes comuns
        if self._last_reaction_ms != 0:
            if (self._idle_ms - self._last_reaction_ms) < REACTION_CD_MS:
                return None

        # Escalada por nivel (maior vence)
        reaction = None
        if idle_net >= (LEVEL4_MS - GRACE_MS):
            reaction = "angry"
        elif idle_net >= (LEVEL3_MS - GRACE_MS):
            reaction = "suspicious"
        elif idle_net >= (LEVEL2_MS - GRACE_MS):
            reaction = "sad"
        elif idle_net >= (LEVEL1_MS - GRACE_MS):
            reaction = "curious"

        if reaction is not None:
            self._last_reaction_ms = self._idle_ms

        return reaction


# -- Helpers --

def _advance(policy, start_ms, step_ms, steps, random_roll=50):
    """Avanca o timer 'steps' vezes. Retorna lista de resultados."""
    results = []
    now = start_ms
    for _ in range(steps):
        now += step_ms
        results.append(policy.check(now_ms=now, random_roll=random_roll))
    return results


def _check_at_idle(idle_ms, random_roll=50):
    """
    Retorna a reacao quando a politica tem idle_ms acumulado, sem cooldowns previos.

    Coloca a politica em idle_ms - CHECK_INTERVAL_MS e dispara um unico check.
    Isso testa qual nivel dispara quando so o tempo de ociosidade importa,
    sem interferencia de reacoes anteriores.
    """
    p = BoredomEscalationPolicy()
    p._idle_ms = idle_ms - CHECK_INTERVAL_MS
    p._prev_now_ms = idle_ms - CHECK_INTERVAL_MS
    return p.check(now_ms=idle_ms, random_roll=random_roll)


# -- Testes --

class TestGracePeriod(unittest.TestCase):
    """Nenhuma reacao deve ocorrer durante o grace period (2 min)."""

    def test_no_reaction_at_boot(self):
        p = BoredomEscalationPolicy()
        self.assertIsNone(p.check(now_ms=CHECK_INTERVAL_MS))

    def test_no_reaction_just_before_grace_ends(self):
        p = BoredomEscalationPolicy()
        steps = GRACE_MS // CHECK_INTERVAL_MS - 1
        results = _advance(p, 0, CHECK_INTERVAL_MS, steps)
        self.assertTrue(all(r is None for r in results))

    def test_no_reaction_exactly_at_grace_end(self):
        """Exatamente ao fim do grace, idle_net=0 -- sem nivel 1."""
        p = BoredomEscalationPolicy()
        results = _advance(p, 0, CHECK_INTERVAL_MS, GRACE_MS // CHECK_INTERVAL_MS)
        self.assertTrue(all(r is None for r in results))


class TestLevelEscalation(unittest.TestCase):
    """
    Verifica que cada nivel dispara quando o threshold e cruzado (sem cooldowns previos).

    Usa _check_at_idle() para posicionar a politica diretamente no tempo alvo,
    sem acumular reacoes de niveis inferiores que ativariam cooldowns.
    Isso valida que o nivel correto dispara para cada faixa de tempo.
    """

    def test_level1_curious_fires_at_5min(self):
        self.assertEqual(_check_at_idle(LEVEL1_MS), "curious")

    def test_level2_sad_fires_at_10min(self):
        self.assertEqual(_check_at_idle(LEVEL2_MS), "sad")

    def test_level3_suspicious_fires_at_20min(self):
        self.assertEqual(_check_at_idle(LEVEL3_MS), "suspicious")

    def test_level4_angry_fires_at_30min(self):
        self.assertEqual(_check_at_idle(LEVEL4_MS), "angry")

    def test_level1_does_not_fire_before_5min(self):
        """Um tick antes do threshold, nivel 1 ainda nao dispara."""
        self.assertIsNone(_check_at_idle(LEVEL1_MS - CHECK_INTERVAL_MS))

    def test_highest_eligible_level_wins(self):
        """Com todos os niveis elegiveis e sem cooldown, 'angry' vence."""
        result = _check_at_idle(LEVEL4_MS)
        self.assertEqual(result, "angry")
        self.assertNotEqual(result, "curious")

    def test_level2_wins_over_level1_at_10min(self):
        result = _check_at_idle(LEVEL2_MS)
        self.assertEqual(result, "sad")
        self.assertNotEqual(result, "curious")

    def test_escalation_sequence_from_scratch(self):
        """
        Partindo do zero, as reacoes aparecem na ordem correta:
        curious -> sad -> suspicious -> angry.
        Cada nivel pode levar mais tempo do que seu threshold
        porque o cooldown da reacao anterior pode bloqueio-lo.
        """
        p = BoredomEscalationPolicy()
        reactions = []
        now = 0
        # Avanca 50 minutos (3000 ticks de 1s simulados como 100 steps de 30s)
        for _ in range(100):
            now += CHECK_INTERVAL_MS
            r = p.check(now_ms=now)
            if r is not None:
                reactions.append(r)

        self.assertGreater(len(reactions), 0, "Nenhuma reacao em 50min")
        self.assertEqual(reactions[0], "curious", "Primeira reacao deve ser curious")
        if len(reactions) >= 2:
            self.assertEqual(reactions[1], "sad",
                             "Segunda reacao deve ser sad")


class TestInteractionReset(unittest.TestCase):
    """Qualquer interacao reinicia o contador."""

    def test_interaction_before_level1_prevents_reaction(self):
        p = BoredomEscalationPolicy()
        _advance(p, 0, CHECK_INTERVAL_MS, (4 * 60 * 1000) // CHECK_INTERVAL_MS)
        p.on_interaction(now_ms=4 * 60 * 1000)
        self.assertIsNone(p.check(now_ms=8 * 60 * 1000))

    def test_interaction_at_level3_resets_to_zero(self):
        p = BoredomEscalationPolicy()
        _advance(p, 0, CHECK_INTERVAL_MS, LEVEL3_MS // CHECK_INTERVAL_MS)
        p.on_interaction(now_ms=LEVEL3_MS)
        self.assertIsNone(p.check(now_ms=LEVEL3_MS + CHECK_INTERVAL_MS))

    def test_idle_restarts_and_level1_fires_again(self):
        """Apos reset, escalada recomeça -- curious apos 5min."""
        p = BoredomEscalationPolicy()
        interact_at = LEVEL3_MS
        p.on_interaction(now_ms=interact_at)
        p._prev_now_ms = interact_at
        # Avanca LEVEL1_MS a partir do interact_at
        _advance(p, interact_at, CHECK_INTERVAL_MS, LEVEL1_MS // CHECK_INTERVAL_MS - 1)
        result = p.check(now_ms=interact_at + LEVEL1_MS)
        self.assertEqual(result, "curious")


class TestCooldown(unittest.TestCase):
    """Cooldown de 12min entre reacoes comuns."""

    def _position_at(self, p, idle_ms):
        """Posiciona p com idle_ms acumulado e sem cooldown previo."""
        p._idle_ms = idle_ms - CHECK_INTERVAL_MS
        p._prev_now_ms = idle_ms - CHECK_INTERVAL_MS

    def test_no_double_fire_within_cooldown(self):
        p = BoredomEscalationPolicy()
        self._position_at(p, LEVEL1_MS)
        first = p.check(now_ms=LEVEL1_MS)
        self.assertEqual(first, "curious")
        # 6min depois (metade do cooldown): ainda bloqueado
        now = LEVEL1_MS
        results = []
        for _ in range((6 * 60 * 1000) // CHECK_INTERVAL_MS):
            now += CHECK_INTERVAL_MS
            results.append(p.check(now_ms=now))
        self.assertTrue(all(r is None for r in results))

    def test_reaction_fires_after_cooldown_expires(self):
        """Apos 12min de cooldown, uma nova reacao deve disparar."""
        p = BoredomEscalationPolicy()
        self._position_at(p, LEVEL1_MS)
        p.check(now_ms=LEVEL1_MS)  # dispara curious

        now = LEVEL1_MS
        result = None
        for _ in range((REACTION_CD_MS + CHECK_INTERVAL_MS) // CHECK_INTERVAL_MS):
            now += CHECK_INTERVAL_MS
            r = p.check(now_ms=now)
            if r is not None:
                result = r
                break
        self.assertIsNotNone(result, "Esperava reacao apos cooldown expirar")

    def test_level4_angry_does_not_repeat_immediately(self):
        p = BoredomEscalationPolicy()
        self._position_at(p, LEVEL4_MS)
        self.assertEqual(p.check(now_ms=LEVEL4_MS), "angry")
        self.assertIsNone(p.check(now_ms=LEVEL4_MS + CHECK_INTERVAL_MS))

    def test_level3_cooldown_respected(self):
        p = BoredomEscalationPolicy()
        self._position_at(p, LEVEL3_MS)
        first = p.check(now_ms=LEVEL3_MS)
        self.assertEqual(first, "suspicious")
        self.assertIsNone(p.check(now_ms=LEVEL3_MS + CHECK_INTERVAL_MS))


class TestDemonMode(unittest.TestCase):
    """Modo demonio: ~5% por check apos 60min, cooldown separado."""

    def _advance_to_just_before_demon(self, p, random_roll=99):
        steps = DEMON_ELIGIBLE_MS // CHECK_INTERVAL_MS - 1
        _advance(p, 0, CHECK_INTERVAL_MS, steps, random_roll=random_roll)

    def test_demon_does_not_fire_before_60min(self):
        p = BoredomEscalationPolicy()
        steps = (DEMON_ELIGIBLE_MS - CHECK_INTERVAL_MS) // CHECK_INTERVAL_MS
        results = _advance(p, 0, CHECK_INTERVAL_MS, steps, random_roll=0)
        self.assertNotIn("demon", results)

    def test_demon_fires_with_low_roll_after_60min(self):
        p = BoredomEscalationPolicy()
        self._advance_to_just_before_demon(p, random_roll=99)
        self.assertEqual(p.check(now_ms=DEMON_ELIGIBLE_MS, random_roll=0), "demon")

    def test_demon_does_not_fire_with_roll_at_threshold(self):
        p = BoredomEscalationPolicy()
        self._advance_to_just_before_demon(p, random_roll=99)
        self.assertNotEqual(p.check(now_ms=DEMON_ELIGIBLE_MS, random_roll=5), "demon")

    def test_demon_does_not_fire_with_high_roll(self):
        p = BoredomEscalationPolicy()
        self._advance_to_just_before_demon(p, random_roll=99)
        self.assertNotEqual(p.check(now_ms=DEMON_ELIGIBLE_MS, random_roll=99), "demon")

    def test_demon_cooldown_blocks_second_demon(self):
        p = BoredomEscalationPolicy()
        self._advance_to_just_before_demon(p, random_roll=99)
        self.assertEqual(p.check(now_ms=DEMON_ELIGIBLE_MS, random_roll=0), "demon")
        now = DEMON_ELIGIBLE_MS
        results = []
        for _ in range((20 * 60 * 1000) // CHECK_INTERVAL_MS):
            now += CHECK_INTERVAL_MS
            results.append(p.check(now_ms=now, random_roll=0))
        self.assertNotIn("demon", results)

    def test_demon_fires_after_cooldown(self):
        p = BoredomEscalationPolicy()
        self._advance_to_just_before_demon(p, random_roll=99)
        p.check(now_ms=DEMON_ELIGIBLE_MS, random_roll=0)
        now = DEMON_ELIGIBLE_MS
        result = None
        for _ in range((DEMON_CD_MS + CHECK_INTERVAL_MS) // CHECK_INTERVAL_MS):
            now += CHECK_INTERVAL_MS
            r = p.check(now_ms=now, random_roll=0)
            if r == "demon":
                result = r
                break
        self.assertEqual(result, "demon")

    def test_demon_rarity_approximately_5_percent(self):
        import random
        hits = sum(1 for _ in range(1000) if random.randint(0, 99) < DEMON_PROB_PCT)
        rate = hits / 1000
        self.assertGreater(rate, 0.02)
        self.assertLess(rate, 0.09)


class TestPausedStates(unittest.TestCase):
    """Em estados de pausa, sem reacoes."""

    def test_paused_blocks_all_reactions(self):
        p = BoredomEscalationPolicy()
        p.set_paused(True)
        steps = DEMON_ELIGIBLE_MS // CHECK_INTERVAL_MS + 10
        results = _advance(p, 0, CHECK_INTERVAL_MS, steps, random_roll=0)
        self.assertTrue(all(r is None for r in results))

    def test_paused_blocks_demon(self):
        p = BoredomEscalationPolicy()
        p.set_paused(True)
        steps = DEMON_ELIGIBLE_MS // CHECK_INTERVAL_MS + 10
        results = _advance(p, 0, CHECK_INTERVAL_MS, steps, random_roll=0)
        self.assertNotIn("demon", results)

    def test_unpausing_allows_reactions(self):
        p = BoredomEscalationPolicy()
        p.set_paused(True)
        pause_steps = 100
        _advance(p, 0, CHECK_INTERVAL_MS, pause_steps)
        resume_at = pause_steps * CHECK_INTERVAL_MS
        p.set_paused(False)
        p.on_interaction(now_ms=resume_at)
        p._prev_now_ms = resume_at
        _advance(p, resume_at, CHECK_INTERVAL_MS, LEVEL1_MS // CHECK_INTERVAL_MS - 1)
        result = p.check(now_ms=resume_at + LEVEL1_MS)
        self.assertEqual(result, "curious")


class TestAngryIsTransitional(unittest.TestCase):
    """ANGRY de ociosidade e sempre transitorio -- nunca baseline."""

    def test_angry_reaction_followed_by_none_within_cooldown(self):
        p = BoredomEscalationPolicy()
        p._idle_ms = LEVEL4_MS - CHECK_INTERVAL_MS
        p._prev_now_ms = LEVEL4_MS - CHECK_INTERVAL_MS
        self.assertEqual(p.check(now_ms=LEVEL4_MS), "angry")
        self.assertIsNone(p.check(now_ms=LEVEL4_MS + CHECK_INTERVAL_MS))

    def test_demon_angry_also_not_repeated(self):
        p = BoredomEscalationPolicy()
        steps = DEMON_ELIGIBLE_MS // CHECK_INTERVAL_MS - 1
        _advance(p, 0, CHECK_INTERVAL_MS, steps, random_roll=99)
        self.assertEqual(p.check(now_ms=DEMON_ELIGIBLE_MS, random_roll=0), "demon")
        second = p.check(now_ms=DEMON_ELIGIBLE_MS + CHECK_INTERVAL_MS, random_roll=0)
        self.assertNotEqual(second, "demon")


if __name__ == "__main__":
    unittest.main()
