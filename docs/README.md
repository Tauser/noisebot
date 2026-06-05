# NoiseBot - Indice de Documentacao

Este diretorio mistura documentos vivos, referencias tecnicas e historico de
validacao. Use este indice para saber por onde comecar.

## Docs Vivos

| Arquivo | Uso |
| ------- | --- |
| `PROJECT.md` | Visao de produto, principios e contexto geral. |
| `ROADMAP.md` | Painel ativo: decisoes atuais, fila P0/P1/P2, feito consolidado e criterios de aceite. |
| `ARCHITECTURE.md` | Arquitetura do firmware/server, camadas, event bus e tasks. |
| `HARDWARE.md` | Mapa de hardware, pinos e restricoes fisicas. |
| `PROJECT_CLEANUP_AUDIT.md` | Auditoria da reorganizacao e decisoes de limpeza. |

## Referencias Tecnicas Mantidas

| Arquivo | Uso |
| ------- | --- |
| `CAMERA_INTEGRATION.md` | Estrategia, achados e validacoes de camera/visao. |
| `VOICE_AUDIO_V2_ARCHITECTURE.md` | Contrato consolidado de Voice Audio v2. |
| `BRIDGE_V2.md` | Arquitetura e contexto do bridge/server. |
| `PERSISTENCE.md` | Politica de NVS, SD e persistencia. |
| `ENERGY.md` | Energia, brownout e limites eletricos. |
| `SERVO_SAFETY.md` | Protocolo de safety antes de liberar servos. |
| `IDLE_REFERENCE.md` | Referencia visual/comportamental de idle. |
| `GPIO_REORGANIZATION.md` | Historico e justificativa de GPIOs. |

## Historico Pesado Local

Estes arquivos contem evidencia util, mas nao devem ser usados como roadmap
ativo. Nesta rodada eles foram movidos para `docs/history/`, pasta local
ignorada pelo Git.

- `docs/history/VOICE_PIPELINE.md`
- `docs/history/VOICE_AUDIO_V2_NEXT_PHASES.md`
- `docs/history/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`
- `docs/history/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md`
- `docs/history/VOICE_OPUS_QUALITY.md`
- `docs/history/VOICE_SAMPLES_PHASE4.md`
- `docs/history/VOICE_AB_PHASE5.md`
- `docs/history/VOICE_AB_PHASE5_8192.md`
- `docs/history/VOICE_AB_PHASE5_2026_05_28.md`
- `docs/history/VOICE_REPLAY_BASELINE.json`
- `docs/history/BRIDGE_V2_TTS_LOCAL.md`
- `docs/history/REFERENCE_ARCHITECTURES.md`
- `docs/history/REFERENCE_ADOPTION_MATRIX.md`

## Pastas Locais Ignoradas

Durante limpezas grandes, use estas pastas localmente se precisar separar
historico e rascunhos sem versionar:

- `docs/archive/`
- `docs/history/`
- `docs/modules/`

Essas pastas estao no `.gitignore` por decisao de produto nesta rodada.
