"""bridgev2.audio.playback — Output Scheduler: pacing de SAY para firmware (Fase 6).

A fila SAY do firmware tem apenas 4 chunks (~64 ms). O scheduler pagina
TtsAudioChunk de say_out_queue respeitando esse limite.
Cancelável via asyncio.Task.cancel() para barge-in.
"""
from __future__ import annotations
# TODO Fase 6: OutputScheduler(asyncio.Task) com pacing e barge-in cancel
