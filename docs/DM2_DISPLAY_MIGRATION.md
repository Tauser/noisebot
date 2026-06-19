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
- SPI2 a 60 MHz: GPIO47 SCLK, GPIO21 MOSI e GPIO45 DC;
- CS em GND, reset por software e sem backlight controlável;
- render mínimo sem framebuffer ou alocação dinâmica no caminho de frame;
- telemetria de hardware, erros e PSRAM livre;
- headroom mínimo de 300 KB validado antes da inicialização;
- flag física separada `CONFIG_NB_HEAD_DISPLAY_HW_ENABLED`.

O perfil `build-dm2` continua sem tocar no painel. O primeiro teste elétrico e
visual usa exclusivamente `build-dm2-hw`.

### Gate de bancada DM2.2

1. Manter a main no perfil `build-dm2`.
2. Gravar somente o head com `build-dm2-hw`.
3. Reiniciar a main para reenviar o snapshot semântico.
4. Confirmar no head:
   - log `ST7789 pronto`;
   - telemetria `hw=1/0`;
   - PSRAM livre maior ou igual a 300 KB;
   - cena simples visível e estável.
5. Observar por 30 minutos: zero corrupção, erro de hardware ou impacto no
   enlace.
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
- zero corrupção em 30 minutos de animação;
- reboot isolado do head restaura snapshot sem piscar estado incorreto;
- desconexão do head mantém main operacional e fallback coerente.

## Fora do corte DM2.2

- touch do display;
- preview de câmera;
- paridade completa do render legado;
- remoção do render local no main.
