"""bridgev2.legacy.legacy_pipeline — Adapta o bridge atual como LegacyPipeline.

Selecionável via NOISEBOT_PIPELINE_MODE=legacy (ou --pipeline=legacy na CLI).
Permanece disponível como fallback até o v2 estar validado em produção.
"""
from __future__ import annotations
# TODO: LegacyPipeline que delega para noisebot_bridge.cli.main()
# import sys, bridge path adjustment necessário.
