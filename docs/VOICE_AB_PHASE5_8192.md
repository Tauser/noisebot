# Voice A/B RAW vs AFE

| modo | turno | qualidade | no_speech | logprob | comp | stt_ms | samples | afe_chunks | fallbacks | overruns | transcript |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| raw | 4 | failed |  |  |  |  | 29184 | 0 | 0 | 0 |  |
| afe | 6 | good | 0.068 | -0.790 | 0.758 | 1161.400 | 83200 | 1202 | 16 | 0 | S.P., me conte uma piada. |
| raw | 12 | good | 0.358 | -0.971 | 0.600 | 1075.400 | 34816 | 0 | 0 | 0 | O que está? |
| afe | 15 | good | 0.029 | -0.476 | 0.704 | 1165.800 | 139008 | 1661 | 19 | 0 | Me conte uma piada. |

## Leitura

- RAW bom: 1/2.
- AFE bom: 2/2.
- no_speech RAW médio: 0.358.
- no_speech AFE médio: 0.049.
- AFE fallbacks totais: 35.
- AFE overruns totais: 0.
- Decisão: AFE candidata; coletar mais repetições antes de promover.
