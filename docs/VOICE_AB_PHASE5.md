# Voice A/B RAW vs AFE

| modo | turno | qualidade | no_speech | logprob | comp | stt_ms | samples | afe_chunks | fallbacks | overruns | transcript |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| raw | 24 | no_speech | 0.963 | -0.610 | 2.065 | 1734.100 | 37120 | 2923 | 38 | 0 |  |
| afe | 1 | good | 0.024 | -0.696 | 0.704 | 4985.800 | 82176 | 673 | 20 | 256 | Me conte uma piada. |

## Leitura

- RAW bom: 0/1.
- AFE bom: 1/1.
- no_speech RAW médio: 0.963.
- no_speech AFE médio: 0.024.
- AFE fallbacks totais: 20.
- AFE overruns totais: 256.
- Decisão: AFE reprovada por overrun; não promover.
