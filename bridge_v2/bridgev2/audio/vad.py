"""bridgev2.audio.vad — VAD secundário de barge-in (Fase 7).

Monitora energia de áudio durante SPEAKING/THINKING para detectar quando o
usuário interrompe a resposta do robô sem precisar de VOICE_ACTIVITY_START
do firmware (útil quando o firmware está em modo half-duplex com playback mute).

Lógica: RMS do chunk acima de `threshold_rms` por `sustain_chunks` consecutivos
→ barge-in confirmado. Um único spike é ignorado para evitar falsos positivos
por ruído impulsivo.
"""
from __future__ import annotations

import math
import struct
import logging

log = logging.getLogger(__name__)

# Valor padrão: ~200 em int16 ≈ 0.6% de full-scale — acima do ruído de fundo
# mas bem abaixo de qualquer fala real.
DEFAULT_THRESHOLD_RMS: float = 200.0

# ~150 ms @ 16 ms/chunk (256 amostras, 16 kHz)
DEFAULT_SUSTAIN_CHUNKS: int = 10


def _compute_rms(pcm: bytes) -> float:
    """Calcula RMS do chunk PCM (int16 LE)."""
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack_from(f"<{n}h", pcm)
    sq_sum = sum(s * s for s in samples)
    return math.sqrt(sq_sum / n)


class BargeInMonitor:
    """Detector de barge-in por limiar de energia sustentada.

    Usage:
        monitor = BargeInMonitor()
        for chunk in audio_chunks:
            if monitor.feed(chunk):
                # barge-in detectado
                break
        monitor.reset()  # ao voltar para IDLE / LISTENING
    """

    def __init__(
        self,
        threshold_rms: float = DEFAULT_THRESHOLD_RMS,
        sustain_chunks: int = DEFAULT_SUSTAIN_CHUNKS,
    ) -> None:
        self._threshold = threshold_rms
        self._sustain = sustain_chunks
        self._above_count: int = 0

    @property
    def above_count(self) -> int:
        """Número de chunks consecutivos acima do limiar (diagnóstico)."""
        return self._above_count

    def feed(self, pcm: bytes) -> bool:
        """Processa um chunk PCM. Retorna True se barge-in confirmado."""
        rms = _compute_rms(pcm)
        if rms >= self._threshold:
            self._above_count += 1
            log.debug("VAD barge-in: rms=%.0f above_count=%d/%d", rms, self._above_count, self._sustain)
        else:
            self._above_count = 0
        return self._above_count >= self._sustain

    def reset(self) -> None:
        """Reinicia o contador. Chamar ao sair de SPEAKING/THINKING."""
        self._above_count = 0
