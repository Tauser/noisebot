"""bridgev2.audio.vad — VAD secundário / confirmação de end-of-turn (Fase 7).

Debounce de ~500–700 ms de energia de voz para robustez contra VOICE_END espúrio.
Não é condição de commit — só confirmação adicional.
"""
from __future__ import annotations
# TODO Fase 7: VadConfirmer com energy threshold e debounce
