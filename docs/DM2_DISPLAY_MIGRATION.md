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

## Fora deste scaffold

- inicialização física do ST7789;
- movimentação do submódulo LovyanGFX;
- touch do display;
- preview de câmera;
- remoção do render local no main.
