# NoiseBot - Roadmap Ativo

Este arquivo e o painel vivo do projeto. Ele deve mostrar o que esta decidido,
o que esta em andamento, o que esta bloqueado e o que saiu de escopo.

Regra principal: aqui ficam fases, prioridades e criterios de aceite. Logs,
historicos longos, experimentos e notas extensas ficam em arquivos de apoio.

## Painel Atual

| Campo | Decisao |
| --- | --- |
| Foco do ciclo | Limpar direcao do projeto e fechar itens pequenos que melhoram uso real |
| Trabalho principal agora | Touch, presence/camera, agenda local, status rail e organizacao de docs |
| Hardware que nao deve guiar trabalho agora | Servos, IMU, bateria e camera como produto final |
| Servos | Nao conectados; qualquer movimento continua bloqueado por `motion_safety` |
| Camera | Pinos DVP preservados; uso atual deve ser leve, observavel e sem travar firmware |
| Voice | Stack de voz/bridge esta funcional como base; proximas melhorias devem ser pontuais |
| Wake word customizada | Fora do ciclo atual |
| TTS HTTP no firmware | Removido do roadmap ativo; duplicava o server/bridge atual |
| Knowledge OS externo | Nao atualizar por enquanto, por decisao do usuario |
| Maior risco atual | Roadmap acumular itens antigos e perder poder de decisao |

## Como Ler

| Status | Significado |
| --- | --- |
| `AGORA` | Deve ser trabalhado neste ciclo |
| `PROXIMO` | Entra depois que pelo menos um item `AGORA` sair |
| `BACKLOG` | Desejavel, mas sem prioridade de implementacao |
| `ADIADO` | Importante, mas depende de hardware, decisao ou maturidade |
| `REMOVIDO` | Saiu do roadmap ativo |
| `FEITO` | Implementado ou consolidado o bastante para virar referencia |

## Fila De Trabalho

### P0 - Agora

| Ordem | Etapa | Resultado esperado | Criterio de saida |
| --- | --- | --- | --- |
| 1 | 2.2A - Touch: sensibilidade e confiabilidade | Touch deixa de disparar ou falhar de forma imprevisivel | Calibracao reproduzivel, debounce estavel e evento confiavel no firmware |
| 2 | 13.1 - Presence detection via camera | Captura leve e observavel, sem prometer visao avancada | Servico roda sem engasgar, publica estado e expoe evidencias de captura |
| 3 | 14.1 - Agenda local | Timers, alarmes e lembretes basicos funcionam de verdade | Criar, listar, disparar e cancelar itens locais |
| 4 | 16.2 - Status rail invisivel e status rapido | Icones persistentes organizados pelo overlay, sem disputa com a face | Mic e camera simultaneos aparecem alinhados; status rapido exibe WiFi/hora/energia sob demanda |
| 5 | D.1 - Limpeza de documentacao | Roadmap e docs centrais ficam legiveis e sem duplicacao obsoleta | Arquivos ativos apontam para fontes certas; historicos ficam fora do git |

### P1 - Proximo

| Etapa | Motivo para entrar depois |
| --- | --- |
| 5.x - Motion safety completo | Necessario antes de qualquer servo real |
| 8.x - Persistencia/SD operacional | Agenda, logs e memoria longa dependem de I/O confiavel |
| 9.6 - WiFi service | Util como conveniencia, mas produto continua offline-first |
| 12.x - Dashboard/observabilidade | Ajuda validar firmware, bridge e eventos sem depender de tentativa manual |
| 15.x - Voice polish | Melhorias pontuais em feedback, erros e telemetria apos estabilizar base |

### P2 - Backlog

| Tema | Condicao para voltar |
| --- | --- |
| AEC e audio duplex avancado | Quando houver meta clara de conversa full-duplex |
| Wake word customizada | Quando o fluxo atual justificar treino/integracao propria |
| Camera avancada | Quando presence leve estiver estavel e houver ganho real de produto |
| Memoria longa/persona | Quando persistencia e privacidade estiverem bem fechadas |
| Polish final de icones | Quando o status rail estabilizar estados reais em hardware |

### Fora Do Ciclo Atual

| Item | Decisao |
| --- | --- |
| `tts_service` no firmware via HTTP local | `REMOVIDO`; duplicava bridge/server |
| Servo expressivo real | `ADIADO`; servos nao conectados e safety vem antes |
| IMU | `ADIADO`; hardware futuro |
| Bateria/LiPo | `ADIADO`; hardware futuro |
| WiFi como dependencia | Nao entra; conectividade e conveniencia |

## Feito Consolidado

| Area | Status | Referencia ativa |
| --- | --- | --- |
| Display/render | `FEITO` como base; ainda pode receber polish | `docs/ARCHITECTURE.md` |
| Event bus e camadas | `FEITO` como diretriz estrutural | `docs/ARCHITECTURE.md` |
| Voice/bridge base | `FEITO` como base funcional | `bridge/`, `server/`, `docs/PROJECT.md` |
| Feedback visual de voice/bridge | `FEITO` como base de produto | `components/services/ui_overlay_service/` |
| Camera inicial | `FEITO` como infraestrutura parcial; ainda falta presence real | `components/services/camera_service/` |
| Organizacao documental inicial | `EM ANDAMENTO` | `docs/README.md`, `docs/PROJECT_CLEANUP_AUDIT.md` |

## Etapas Ativas

### Etapa 2.2A - Touch: Sensibilidade e Confiabilidade

Status: `AGORA`

Objetivo: fazer o touch virar uma entrada confiavel para produto, sem falsos
positivos e sem exigir varias tentativas do usuario.

Escopo:

- Revisar calibracao, thresholds e filtragem.
- Conferir debounce e publicacao de eventos.
- Validar diferenca entre toque curto, toque longo e ruido.
- Documentar qualquer limitacao fisica encontrada.

Criterios de aceite:

- Toque intencional gera evento de forma consistente.
- Ruido ou flutuacao nao dispara acao indevida.
- O servico continua respeitando camadas e event bus.
- O comportamento fica reproduzivel apos reboot.

Nao entra:

- Gestos complexos.
- Dependencia de servo ou conectividade.

### Etapa 13.1 - Presence Detection Via Camera

Status: `AGORA`

Objetivo: usar a camera como sensor leve de presenca, com resolucao baixa o
bastante para o firmware ler sem engasgos.

Escopo:

- Manter a implementacao coerente com referencias StackChan/Xiao quando houver
  ganho real.
- Usar captura reduzida para processos sem preview para o usuario.
- Publicar estado por servico/evento, sem outros modulos desenharem direto.
- Separar claramente "camera ativa" de "presenca detectada".

Criterios de aceite:

- Firmware compila sem warnings.
- Captura roda em baixa resolucao sem travar render, audio ou event bus.
- Camera ativa aparece no status rail quando aplicavel.
- Falha de camera gera estado observavel e nao derruba o robo.

Nao entra:

- Reconhecimento facial.
- Streaming continuo de alta resolucao.
- Dependencia de cloud.

### Etapa 14.1 - Agenda Local: Timers, Lembretes e Alarmes

Status: `AGORA`

Objetivo: tornar timers, lembretes e alarmes uma funcionalidade local basica e
util, sem depender de internet.

Escopo:

- Criar, listar, cancelar e disparar timer.
- Criar lembrete simples com horario local.
- Criar alarme basico.
- Publicar eventos de disparo para overlay/audio/conductor.
- Definir persistencia minima segura.

Criterios de aceite:

- Um timer criado dispara no tempo esperado.
- Um alarme/lembrete pode ser cancelado antes de disparar.
- Disparo aparece de forma clara no feedback visual/sonoro.
- Reboot nao causa comportamento perigoso ou duplicado.

Nao entra:

- Calendario externo.
- Sincronizacao cloud.
- Agenda recorrente complexa.

### Etapa 16.2 - Status Rail Invisivel e Status Rapido

Status: `AGORA`

Objetivo: criar um trilho invisivel no `ui_overlay_service` para organizar icones
pequenos e persistentes sem disputar espaco com a face do robo, e adicionar uma
barra temporaria de status quando o usuario pedir um resumo do sistema.

Decisoes de design:

- O trilho normal e invisivel: nao desenha fundo, apenas aloca slots.
- O tamanho alvo dos slots e 28x28 px, inspirado na barra de 28 px do StackChan,
  mas sem painel permanente.
- O StackChan usa LVGL, barra 320x28, imagens `.bin` e recolor; o NoiseBot usa
  LovyanGFX direto, entao o equivalente leve e mascara 1-bit recolorida.
- Nao usar SVG em runtime no ESP32. SVG fica como fonte editavel; PBM/header C
  fica como formato de execucao.
- Sem touch de tela por enquanto. A barra de status aparece por evento/comando,
  nao por gesto de arrastar.

Escopo inicial:

- Mic ouvindo ou capturando.
- Mic bloqueado por modo silencio.
- Camera ativa.
- Speaker/fala ativa.
- Bridge conectado ou ocupado.
- WiFi conectado ou instavel.
- Alertas persistentes: SD indisponivel, safety/motion fault ou power warning.
- Status rapido sob demanda com WiFi/bridge, hora e energia/bateria.

Assets e pipeline:

- Fontes editaveis: `assets/ui/icons/*.svg`.
- Mascaras runtime: `assets/ui/icons/28x28/*.pbm`, formato PBM P1 28x28.
- Header gerado: `components/services/ui_overlay_service/icons/generated/nb_ui_overlay_icons.h`.
- Gerador: `components/services/ui_overlay_service/icons/tools/generate_overlay_icons.py`.
- O gerador usa `assets/ui/icons/28x28` como fonte padrao e aceita `--source`
  para casos especiais.
- O firmware tinge os icones no draw; cor nao fica embutida no asset.

Conjunto inicial de icones:

- WiFi: `wifi-1`, `wi-fi-2`, `wi-fi-3`, `wi-fi-alerta`, `wi-fi-indisponivel`.
- Volume: `volume-baixo`, `volume`, `volume-mudo`, `volume-desligado`,
  `barra-de-volume`.
- Mic: `microfone`, `microfone-bloqueado`.
- Bateria/energia: `bateria-ausente`, `bateria-vazia`, `bateria-um-quarto`,
  `bateria-metade`, `bateria-tres-quartos`, `bateria-cheia`, `bateria-100`,
  `bateria-carregando`.
- Extras disponiveis: `camera`, `despertador`, `relogio`, `trancar`,
  `desbloquear`, `identificando-usuario`, `senha-do-wifi`, `cafe`, `vento`,
  `alerta-de-alta-temperatura`, `relogio-calendario`.

Regras:

- `IDLE` continua sendo o baseline visual.
- Nenhum servico desenha icones diretamente.
- Servicos apenas publicam estado ou chamam API do overlay.
- `ui_overlay_service` decide posicao, prioridade, overflow e remocao.
- Status persistente usa `ui_overlay_status_icon_set(icon, enabled)`.
- Status rapido usa `ui_overlay_show_quick_status(...)` com labels/icones ja
  resolvidos pelo chamador; o overlay nao depende diretamente de WiFi, hora ou
  bateria.
- Wiring inicial: modo silencioso (`MEDITATION`/`SILENT_COMPANY`) liga mic
  bloqueado; playback liga speaker ativo; bridge offline liga `server-off`;
  bridge online nao aparece no trilho persistente; volume 0 liga volume
  desligado.
- Status rapido inicial e disparado por evento de sessao do bridge com
  `event:"STATUS_COMMAND"`, `action:"status"` ou `action:"quick_status"`.
- Intents locais `local_status` e `local_network_status` no server/bridge
  tambem emitem `STATUS_COMMAND`, para o pedido por voz mostrar a barra.
- Estados temporarios somem ao final do processo.
- Estados intencionais persistentes permanecem visiveis ate segunda ordem.
- Mic ativo e mic bloqueado usam icones diferentes.
- Volume aparece quando muda, quando esta mudo/desligado, ou quando o usuario
  pede status; nao precisa ficar sempre visivel.
- Bateria ausente representa o estado honesto enquanto LiPo/circuito seguem
  adiados.
- Alertas criticos podem usar cor de alerta, mas a silhueta continua
  monocromatica.

Criterios de aceite:

- Mic e camera ativos ao mesmo tempo aparecem alinhados, sem sobreposicao.
- Modo silencio mostra mic bloqueado enquanto estiver ativo.
- Com zero status ativos, nada aparece na tela.
- Alertas criticos tem prioridade sobre status informativos.
- O trilho nao cobre olhos, boca, texto principal ou overlays transitorios.
- Status rapido aparece por comando/evento, mostra WiFi/bridge, hora e energia,
  e some sozinho apos alguns segundos.
- O header de icones e gerado a partir de `assets/ui/icons/28x28` sem duplicar
  PBMs dentro do componente.

Nao entra:

- Parser SVG, PNG ou JPG no firmware para esses icones.
- Gesto de puxar barra pela tela enquanto nao houver touch no display.
- Painel permanente no topo durante `IDLE`.

### Etapa D.1 - Limpeza de Roadmap e Documentacao

Status: `AGORA`

Objetivo: separar decisao ativa de historico, para o projeto voltar a ser facil
de navegar no VS Code e no git.

Escopo:

- Manter `docs/ROADMAP.md` como painel ativo.
- Manter `docs/README.md` como indice curto de documentacao.
- Consolidar achados em `docs/PROJECT_CLEANUP_AUDIT.md`.
- Remover duplicacoes obsoletas do git quando virarem historico local.
- Preservar evidencias importantes sem poluir a estrutura versionada.

Criterios de aceite:

- A raiz de `docs/` mostra poucos arquivos de decisao.
- Roadmap antigo e logs nao competem com o roadmap ativo.
- Itens feitos aparecem consolidados, nao repetidos como pendencia.
- O usuario consegue abrir o roadmap e entender o proximo passo em menos de um minuto.

## Regras De Atualizacao

- Ao concluir uma etapa `AGORA`, mover para `Feito Consolidado` ou transformar em
  item `P1/P2` se ainda houver extensoes.
- Ao adicionar uma etapa, preencher sempre: objetivo, escopo, criterios de aceite
  e o que nao entra.
- Nao adicionar logs longos neste arquivo.
- Nao reativar itens removidos sem explicar a decisao.
- Nao colocar dependencias de hardware que ainda nao esta conectado como trabalho
  principal do ciclo.
