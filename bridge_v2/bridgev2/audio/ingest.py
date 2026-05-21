"""bridgev2.audio.ingest — AUDIO_CHUNK → ring buffer + stream PCM (Fase 3/4).

Responsabilidade: receber chunks PCM do firmware, manter um ring buffer de
~320 ms de pré-roll e fornecer o stream ao STT parcial e ao VAD.
"""
from __future__ import annotations
# TODO Fase 3: AudioIngest com ring buffer e feed() / stream_pcm()
