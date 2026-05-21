"""bridgev2.tts.piper_server — Processo Piper persistente (Fase 5.5 / 6).

Mantém o processo piper rodando de longa duração via stdin/stdout.
Elimina o custo de spawn por turno do bridge v1.
"""
from __future__ import annotations
# TODO Fase 5.5 (spike): validar opções:
# (a) piper CLI persistente via stdin, (b) lib wrapper,
# (c) piper-phonemize + ONNX Runtime direto, (d) microserviço local.
# TODO Fase 6: PiperServerTTS(TTSProvider) com a opção escolhida no spike.
