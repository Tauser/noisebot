# Voice A/B RAW vs AFE — 2026-05-28

Rodada curta feita em hardware real após o ajuste de runtime do
`audio_processor_service`. AFE ficou ativo apenas por endpoint de bancada.

## Status AFE

| métrica | valor |
| --- | ---: |
| shadow_active | true |
| processed_bridge_enabled | true |
| shadow_psram_start_kb | 7249 |
| shadow_psram_current_kb | 7124 |
| processed_bridge_chunks | 3922 |
| processed_bridge_fallbacks | 36 |
| processed_output_overruns | 0 |
| shadow_fetch_nulls | 0 |

## STT

| modo | turno | qualidade | no_speech | logprob | comp | duração | transcript |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| afe | 58 | GOOD | 0.01 | -0.65 | 0.67 | 2.656s | Vale algo curto. |
| afe | 59 | GOOD | 0.00 | -0.48 | 0.75 | 4.352s | Me diga uma curiosidade. |
| afe | 60 | GOOD | 0.01 | -0.49 | 0.75 | 2.944s | Conte minha piada curta. |
| raw | 62 | GOOD | 0.01 | -0.60 | 0.72 | 3.072s | Faça-lhe algo curto. |
| raw | 63 | GOOD | 0.00 | -0.24 | 0.75 | 3.344s | Me diga uma curiosidade. |
| raw | 64 | GOOD | 0.01 | -0.28 | 0.70 | 3.808s | Me conte uma piada. |

## Leitura

- AFE: 3/3 turnos `GOOD`, sem overrun, fallback ~0.9%.
- RAW: 3/3 turnos `GOOD`.
- `no_speech` médio empatado em ~0.007.
- `logprob` médio AFE: -0.54.
- `logprob` médio RAW: -0.37.
- Decisão: AFE está operacional como opt-in, mas não há ganho de STT suficiente
  para virar padrão. Manter RAW como padrão e repetir A/B maior antes de
  promover.
