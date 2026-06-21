# DM2 — Migração do display para o head-controller

## Objetivo

Transferir a autoridade física de display/render para a Freenove sem mover
decisão de comportamento para o head e sem remover o fallback local do main
antes da validação.

## Estado preparado

- contrato C17 `nb_display_command_t`, protocolo 1.3;
- comando semântico de 16 bytes, sem pixels ou tipos LovyanGFX;
- fila main→link limitada a 8 comandos, drenada somente pela task do enlace;
- receptor no head com validação de versão, tamanho, opcode, gaze e campos
  reservados;
- capability `NB_LINK_CAP_DISPLAY_SEMANTIC`;
- `CONFIG_NB_HEAD_DISPLAY_ENABLED=n` por padrão;
- nenhuma alteração de GPIO ou flash durante o soak DM1.

## DM2.1 — prova semântica sem display físico

Perfis isolados `sdkconfig.dm2.defaults` habilitam:

- main em boot mínimo, enviando uma única cena após `READY`;
- head anunciando `NB_LINK_CAP_DISPLAY_SEMANTIC`;
- receptor idempotente por `generation`, com contadores
  `accepted/rejected/ignored`;
- telemetria da última geração aplicada.

O perfil não inicializa LovyanGFX, SPI do ST7789, backlight ou framebuffer.
Ele existe para provar capability, fila, ACK, validação e aplicação ponta a
ponta antes do gate físico do display.

## Evidência DM2.1 — 2026-06-19

Perfis `build-dm2` gravados na Waveshare COM5 e Freenove COM12, com o enlace
mantido em 10 MHz. Resultado:

- ambos permaneceram em `READY`;
- main enfileirou `SET_SCENE`, geração 1;
- head aplicou uma única cena: `display=1/0/0 gen=1`
  (`accepted/rejected/ignored`);
- ACK RTT da main: 5 ms;
- `invalid=0`, `retry=0`, `timeout=0`, `spi_err=0`;
- LovyanGFX, ST7789, backlight e framebuffer permaneceram desativados.

A tentativa de reiniciar a main por RTS para repetir fisicamente a geração 1
não produziu reboot nessa execução. A rejeição de geração duplicada, stale e
o wrap-around permanecem aprovados nos testes host; repetição física fica para
o próximo gate.

Resultado DM2.1: **aprovado para rota semântica ponta a ponta**. Isso não
aprova ainda o display físico nem a paridade visual.

## DM2.2 — HAL físico preparado

- LovyanGFX movido para `firmware/shared/components/LovyanGFX`, evitando duas
  cópias durante o fallback main/head;
- HAL ST7789 exclusivo do head em `nb_head_display_hal`;
- SPI2 em 40 MHz no gate animado com jumpers: GPIO47 SCLK, GPIO21 MOSI e
  GPIO45 DC; rollback direto para 20 MHz em qualquer instabilidade;
- CS em GND, reset físico em GPIO3 e sem backlight controlável;
- framebuffer RGB565 de tela completa (~150 KB) alocado uma vez na PSRAM; o
  caminho de frame desenha fora da tela e faz um único `pushSprite`, sem
  alocação por quadro;
- telemetria de hardware, erros e PSRAM livre;
- headroom mínimo de 300 KB validado antes da inicialização;
- flag física separada `CONFIG_NB_HEAD_DISPLAY_HW_ENABLED`.

O perfil `build-dm2` continua sem tocar no painel. O primeiro teste elétrico e
visual usa exclusivamente `build-dm2-hw`.

O soak local de bancada foi removido após o gate. O perfil físico final inicia
o painel em preto e só desenha comandos semânticos recebidos da Waveshare.
O acesso ao LovyanGFX permanece serializado por mutex.

### Evidência de frequência — 2026-06-19

- 50 MHz: rejeitado por erros visuais observados em bancada;
- 40 MHz: aprovado com 3.000 frames em 5 minutos a 10 FPS;
- zero erros de HAL/enlace e PSRAM estável em 8.386.156 bytes;
- após resets isolados do head, a cena `generation=1` foi reaplicada
  automaticamente sem reiniciar a main;
- RST preso em 3,3 V foi rejeitado: o painel podia permanecer preto após
  reset/reflash; GPIO3 com sequência explícita alto→baixo→alto antes do
  `LovyanGFX::init()` restaurou o painel e a face sem intervenção manual;
- rotação final `0`, orientação física vertical 240×320;
- render direto no painel foi rejeitado por piscar a tela inteira entre
  quadros; framebuffer em PSRAM eliminou o piscar no probe animado;
- soak final limitado por decisão de bancada a 10 minutos: mais de 4.200
  quadros observados, enlace sempre `READY`, zero erro SPI/HAL e PSRAM estável;
- firmware final deixou 8.230.504 bytes livres de PSRAM após alocar o
  framebuffer RGB565;
- 20 MHz permanece como rollback elétrico conservador.

Resultado DM2.2: **gate físico aprovado**. A fase DM2 permanece em andamento
para portar o render real, expressões, gaze e overlays, mantendo fallback local
até a validação de paridade.

## DM2.3 — render procedural remoto

O primeiro renderer procedural do head cobre dez expressões, gaze nos dois
eixos e framebuffer RGB565 em PSRAM. O probe da main envia 80 cenas a 8 FPS
após o snapshot inicial, percorrendo todas as expressões e a faixa horizontal
de gaze.

O recebimento semântico e o desenho físico foram desacoplados:

- a task do enlace valida e substitui uma fila estática de um snapshot;
- o ACK não aguarda desenho nem `pushSprite`;
- a task de display roda no core 1, prioridade 7, abaixo da task de enlace;
- snapshot novo substitui o pendente, sem alocação por frame.

### Evidência DM2.3 — 2026-06-20

- `build-dm2` gravado na Waveshare COM5 e `build-dm2-hw` na Freenove COM12;
- duas execuções consecutivas chegaram a `READY` e concluíram o probe;
- head aplicou `display=81/0/0 gen=81`, sem rejeição ou geração ignorada;
- main terminou ambas com `retry=0`, `timeout=0` e `spi_err=0`;
- ACK médio de 8 ms e último ACK de 10 ms; o máximo acumulado de 54 ms ocorreu
  durante a subida inicial do enlace, antes do tráfego estável do probe;
- ST7789 permaneceu a 40 MHz e a PSRAM livre estável em 8.230.504 bytes;
- validação visual do operador: animação completa, sem piscar, corrupção ou
  deformação.

Resultado DM2.3: **aprovado para render procedural e animação remota**. O
próximo corte liga a facade remota ao estado visual real da main, incluindo
overlays, e valida fallback local antes de retirar qualquer caminho legado.

## DM2.4 — facade de estado visual

A main passou a manter um snapshot semântico único, independente do destino de
render. `expression_service`, gaze e overlays locais atualizam a
`visual_state_facade`; a facade publica no máximo a 8 Hz e injeta o snapshot no
enlace sem depender de `infra`. O render local continua recebendo exatamente as
mesmas chamadas e permanece como fallback.

O contrato ganhou flags compactas para listening, speaking, sleeping, alerta,
coração, blush, mensagem e timer. O head desenha os sinais essenciais sem
receber pixels, texto ou decisões de comportamento.

### Evidência técnica DM2.4 — 2026-06-20

- probe `build-dm2` alterado para usar a mesma facade do runtime, sem montar
  `nb_display_command_t` diretamente;
- sequência remota confirmou overlays `0x0001`, `0x0002`, `0x0010` e `0x0004`
  para listening, speaking, heart e sleeping;
- head aplicou `display=81/0/0`, geração final 90, brilho 180;
- main terminou com `retry=0`, `timeout=0`, `spi_err=0` e ACK
  último/médio/máximo de 5/7/10 ms;
- head terminou com `invalid=0`, `retry=0`, `timeout=0`, `spi_err=0`,
  `hw=1/0` e 8.230.504 bytes de PSRAM livres;
- fila visual pendente da main reduzida a um item com overwrite: estado novo
  substitui estado ainda não enviado;
- builds main/head sem warnings e testes host de protocolo verdes.

Resultado DM2.4: **gate técnico aprovado**. A confirmação visual do operador
para os quatro overlays permanece como evidência final de bancada. O próximo
corte valida reboot isolado do head, restauração do último snapshot e fallback
local quando o enlace fica indisponível.

## Evidência de recuperação — 2026-06-20

Com a geração 90 (`NEUTRAL`, gaze `-950,-350`, brilho 180 e sleeping) ativa, a
Freenove foi reiniciada isoladamente pela COM12:

- main transitou `READY → DEGRADED → SNAPSHOT → READY`;
- handshake de recuperação da main fechou em 25 ms;
- log `snapshot visual restaurado generation=90`;
- head aplicou uma única cena após o reboot, exatamente a geração 90;
- `display=1/0/0`, `retry=0`, `timeout=0`, `spi_err=0`, `hw=1/0`;
- PSRAM livre permaneceu em 8.230.504 bytes.

O gate técnico de restauração após reboot do head está aprovado. Permanece
pendente apenas confirmar visualmente que a tela voltou diretamente ao mesmo
estado sleeping, sem quadro semântico incorreto.

## DM2.5 — head indisponível desde o boot

O primeiro ensaio revelou que a main permanecia indefinidamente em
`HANDSHAKE` quando as 16 tentativas iniciais de HELLO terminavam antes de o
head subir. A FSM foi corrigida para entrar em `DEGRADED` após esse limite,
mantendo a main operacional e emitindo HELLO periódico sem limite até o peer
aparecer. Um teste host cobre explicitamente o head que inicia tarde.

### Evidência DM2.5 — 2026-06-20

- Freenove mantida em reset durante todo o boot e probe visual da Waveshare;
- main transitou `RESET → HANDSHAKE → DEGRADED` em aproximadamente 4,3 s;
- probe da facade completou expressão, gaze e overlays enquanto o head estava
  ausente, sem panic, abort ou reboot da main;
- após 23 s, o head foi liberado e o enlace transitou
  `DEGRADED → SNAPSHOT → READY`;
- recuperação da main em 20 ms;
- snapshot final restaurado diretamente: expressão neutral, gaze `-950,-350`,
  brilho 180 e sleeping (`Zzz`);
- head aplicou gerações 192/193 com o mesmo estado final;
- `retry=0`, `timeout=0`, `spi_err=0`, `invalid=0`, `hw=1/0`;
- PSRAM do head permaneceu em 8.230.504 bytes;
- testes host: 3/3 verdes, incluindo peer ausente no boot.

Resultado DM2.5: **fallback e recuperação tardia tecnicamente aprovados**. A
main continua operando sem head, preserva o estado mais recente e converge ao
snapshot atual quando o head retorna. O render local legado permanece
compilado e não foi removido.

### Gate de bancada DM2.2

1. Manter a main no perfil `build-dm2`.
2. Gravar somente o head com `build-dm2-hw`.
3. Reiniciar a main para reenviar o snapshot semântico.
4. Confirmar no head:
   - log `ST7789 pronto`;
   - telemetria `hw=1/0`;
   - PSRAM livre maior ou igual a 300 KB;
   - cena simples visível e estável.
5. Observar por 10 minutos: zero corrupção, piscar, erro de hardware ou impacto
   no enlace.
6. Em qualquer anomalia, gravar novamente o perfil semântico `build-dm2`, que
   não inicializa o painel.

## Ordem de implementação

1. Fechar DM1: soak, E5 e E6.
2. Adicionar LovyanGFX ao projeto head como dependência própria.
3. Portar `display_hal` usando exclusivamente `nb_hw_config_head.h`.
4. Portar `render_service`, expression, gaze visual e overlays.
5. Aplicar o último snapshot válido ao entrar em `READY`.
6. Adicionar flag de rota no main:
   - local: render atual;
   - remoto: facade semântica;
   - fallback: local se head indisponível.
7. Validar paridade visual, heap, FPS e recuperação de reboot.
8. Só depois remover LovyanGFX/display do main em DM6.

## Invariantes

- main é a autoridade de expressão, gaze e overlays;
- head não executa comportamento;
- comandos são idempotentes por `generation`;
- fila cheia não bloqueia tasks de comportamento;
- novo snapshot substitui comandos visuais pendentes antigos;
- nenhum framebuffer em SRAM;
- mínimo de 300 KB de PSRAM livre no head além dos buffers ativos;
- falha do display ou head nunca afeta `motion_safety`.

## Gates

- builds main/head com `-Werror`;
- protocolo host verde;
- tela neutra local no boot do head;
- primeiro snapshot após handshake reproduz o estado do main;
- p95 comando→frame menor que 20 ms sem bulk;
- zero corrupção ou piscar em 10 minutos de animação;
- reboot isolado do head restaura snapshot sem piscar estado incorreto;
- desconexão do head mantém main operacional e fallback coerente.

## Resultado do primeiro corte técnico DM2 — 2026-06-20

DM2.1–DM2.5 estão **feitas**: facade visual, render procedural remoto, oito
overlays, fallback local, recuperação de reboot isolado e head ausente no boot
foram aprovados nos gates técnicos descritos acima.

Isso não encerra a autoridade visual. Em 2026-06-20, a revisão do programa
reabriu DM2 até incluir paridade de faces, animações, todos os estados, texto,
status rail, assets, recovery de transientes, soak e cutover. A sequência
canônica agora está em `docs/DUAL_MCU_MIGRATION_ROADMAP.md`, DM2.6–DM2.15.

## Decisão de escopo — paridade de DM2 (2026-06-20)

O contrato remoto (`nb_display_command_t`) cobre expressão, gaze, brilho e oito
flags de overlay (`listening/speaking/sleeping/alert/heart/blush/message/timer`).
O `ui_overlay_service` local desenha, além disso, cerca de trinta ícones de
status (wifi, bateria, volume, mic, câmera, bridge, alarme etc.), texto livre e
a barra de status rápido — nenhum desses chega ao head pela facade hoje.

Decisão anterior, superada em 2026-06-20: DM2 seria fechada com oito overlays e
a paridade restante seria empurrada para 16.2/DM6. Isso foi rejeitado porque
DM6 deve remover legado, não descobrir ou implementar a autoridade visual que
faltou. Ícones, texto e status rail agora pertencem a DM2.11; a Etapa 16.2
continua sendo a especificação de produto correspondente.

## Fora de DM2.1–DM2.5, mas dentro da fase DM2 reaberta

- touch do display;
- preview de câmera;
- paridade completa do render legado, tratada em DM2.6–DM2.14;
- cutover do render padrão, tratado em DM2.15;
- remoção física do render local no main, que continua em DM6 após o cutover.
