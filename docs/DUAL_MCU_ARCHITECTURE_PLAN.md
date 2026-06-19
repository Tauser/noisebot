# Plano de Arquitetura Dual-MCU do NoiseBot

**Status:** arquitetura aprovada; migração incremental em andamento
**Controlador principal:** Waveshare ESP32-S3 N32R16
**Controlador de cabeça:** Freenove ESP32-S3-WROOM CAM N16R8

## 1. Objetivo

Separar controle crítico e multimídia sem introduzir dependência insegura entre
movimento, áudio e o copro­cessador de cabeça. O sistema deve continuar útil em
falha parcial, permitir rollback por fase e manter um único microSD.

## 2. Autoridade de cada MCU

### Main controller — autoridade do robô

- estado oficial, FSM, comportamento, emoção e persona;
- áudio, wake word, VAD, playback, codec e bridge;
- Wi-Fi, API, tempo e OTA;
- servos, `motion_safety`, LEDs, touch corporal e sensores;
- NVS de configuração, identidade e estado crítico;
- cliente do head link e do armazenamento remoto.

### Head controller — autoridade multimídia local

- display, renderização, gaze visual e overlays;
- touch do display;
- câmera, captura, preview e métricas visuais leves;
- microSD único e operações de arquivos;
- cache de assets e transferência bulk;
- manutenção de uma apresentação segura quando o main estiver indisponível.

O head controller não decide movimento, emoção, fala ou estado comportamental.
O main envia intenção semântica; o head renderiza e reporta eventos.

## 3. Topologia física

Enlace recomendado:

- SPI full-duplex, main como master, para controle e dados;
- GPIO `HEAD_IRQ` do head para o main;
- GPIO `HEAD_RESET` controlado pelo main;
- GND comum;
- UART independente somente para console de cada placa.

Começar em 10 MHz e promover para 20 MHz após teste de integridade. Cabos devem
ser curtos, com retorno de GND próximo. O protocolo não pode depender de
temporização implícita de GPIO.

## 4. Protocolo

O contrato canônico vive em
`firmware/shared/components/nb_inter_mcu_protocol`.

Cada frame contém magic, versão, canal, flags, tipo, sequência, tamanho, CRC do
payload e CRC do header. Canais lógicos:

- `CONTROL`: expressão, gaze, overlay, brilho e comandos de câmera;
- `EVENT`: touch, presença, falhas e mudanças de capability;
- `STORAGE`: requisições curtas de arquivo;
- `BULK`: JPEG, áudio, LTM e blocos grandes;
- `DIAGNOSTIC`: métricas, logs e health.

Regras:

- comandos mutadores possuem sequence e resposta idempotente;
- retries reutilizam a mesma sequence;
- timeout nunca implica que a operação não ocorreu;
- ACK confirma recebimento/aceitação pelo peer, não commit de domínio;
- timeout de ACK e reboot do peer são reportados ao chamador como resultado
  ambíguo para reconciliação explícita;
- payload máximo inicial de 4096 bytes;
- transferências maiores usam `BEGIN/CHUNK/END/ABORT`;
- versões major incompatíveis bloqueiam operação; minor usa negociação;
- payload externo é validado antes de alterar estado.

### 4.1 Transações SPI e controle de fluxo

Como o main é master, toda transferência é iniciada por ele:

- `HEAD_IRQ=1` significa que o head possui ao menos um frame pendente;
- o main executa `POLL` quando o IRQ sobe e também em intervalo de recuperação;
- resposta sem dados usa frame `IDLE`, nunca bytes indefinidos;
- cada lado anuncia créditos livres por canal;
- `CONTROL` e `EVENT` têm reserva própria e nunca consomem créditos de `BULK`;
- `BULK` usa janela inicial de 4 chunks e só avança após `CREDIT_UPDATE`;
- reboot/novo `boot_id` zera janelas, créditos, handles e transfers em curso.

Crédito significa capacidade de receber e enfileirar, não confirmação de
processamento ou persistência. ACK de frame, resposta da operação e confirmação
durável são estados diferentes.

### 4.2 Tempo e timestamps

O main é a fonte de tempo oficial:

- `TIME_SYNC` leva monotonic time e, quando disponível, Unix time;
- eventos do head carregam contador monotônico local e sequence;
- o main converte/carimba o evento no recebimento;
- decisões de comportamento, deadlines e logs canônicos usam tempo do main;
- p95 de touch/visual é medido por relógio monotônico do main, sem comparar
  diretamente clocks livres dos dois MCUs.

## 5. Boot e recuperação

1. Cada MCU inicializa watchdog, NVS e console independentemente.
2. Head monta SD e sobe display em tela neutra local.
3. Main inicializa safety, áudio e controle físico sem esperar o head.
4. Main envia `HELLO`; head responde versão, boot ID e capabilities.
5. Main envia snapshot completo do estado visual.
6. Só depois o link entra em `READY`.

Heartbeat recomendado: 250 ms. Após 1 s sem heartbeat:

- main marca head indisponível, não bloqueia áudio nem safety;
- head mantém último frame por curto período e depois mostra estado neutro;
- handles de arquivo remotos são invalidados pelo `boot_id`;
- reconexão exige novo handshake e snapshot completo.

O main pode resetar o head após três tentativas de recuperação malsucedidas,
com rate limit para evitar loop de energia.

## 6. Armazenamento único

O microSD pertence exclusivamente ao head. O main nunca acessa FATFS ou SDMMC
diretamente na arquitetura final.

Serviços do head:

- `stat`, `open`, `read`, `write`, `close`, `sync`;
- append de log;
- commit atômico via arquivo temporário + rename;
- consulta de espaço e estado do volume;
- leitura sequencial de assets;
- gravação de snapshots.

Políticas:

- NVS do main guarda safety, configuração e identidade;
- NVS do head guarda apenas configuração local e metadados de recuperação;
- LTM e logs usam fila no main com limite e backpressure;
- gravações críticas usam CRC, tamanho esperado e commit atômico;
- nenhum caminho vindo do link pode escapar dos diretórios permitidos;
- handles incluem geração/boot ID para impedir uso após reboot;
- operações SD nunca rodam em task de alta prioridade.

### 6.1 Áudio armazenado no head

I2S, wake, VAD e playback permanecem no main. Assets do SD nunca são
reproduzidos diretamente do link:

- leitura remota preenche um ring buffer no main antes de iniciar I2S;
- baseline inicial: 96KB de ring buffer em PSRAM e prebuffer de 64KB;
- blocos são copiados para buffers DMA em SRAM pela task de áudio;
- abaixo de 32KB livres, o cliente solicita refill com prioridade de bulk
  `AUDIO`;
- `CONTROL`/`EVENT` continuam preemptivos, mas bulk de áudio preempta JPEG,
  logs e LTM enquanto há playback;
- underrun encerra o asset com fade curto, publica diagnóstico e nunca bloqueia
  a task I2S;
- sons críticos pequenos de wake/erro devem ter fallback em flash no main.

Os tamanhos são baseline de F5 e podem mudar somente com medição registrada.

### 6.2 Backpressure de LTM e indisponibilidade prolongada

O main mantém uma fila limitada por bytes e prioridade:

1. alterações críticas de persona/configuração: espelhadas em NVS e nunca
   descartadas silenciosamente;
2. marcos de interação: preservados antes de telemetria repetitiva;
3. métricas/logs diagnósticos: descartáveis primeiro.

Baseline: 128KB ou 256 registros, o que ocorrer primeiro. Ao atingir 80%, o
main agrega eventos repetitivos. Em 100%, descarta o item de menor prioridade e
incrementa contador persistente em NVS. Após reconexão, replay é idempotente e
ordenado. Leituras de LTM indisponíveis retornam estado explícito; persona
continua com o snapshot NVS, sem inventar memória ausente.

### 6.3 Caminhos de visão

- câmera local do head é a fonte canônica de frames físicos do robô;
- preview, frame rate e overlays de captura ficam locais no head;
- análise semântica e reconhecimento continuam no server/bridge;
- o main solicita JPEG sob demanda e o encaminha, sem manter pipeline DVP;
- câmera de dashboard/upload é uma entrada separada e nunca substitui
  silenciosamente a câmera física;
- ausência de head/câmera desabilita presença visual, sem inferir ausência do
  usuário por falta de sensor.

## 7. Modos degradados

| Falha | Comportamento |
| --- | --- |
| Head desligado | Main mantém voz, comportamento, sensores e safety; UI indisponível |
| Main desligado | Head mostra neutro/offline; câmera e SD não executam ações autônomas |
| SD ausente | Display e câmera continuam; storage reporta capability degradada |
| CRC/timeout | Frame descartado, contador incrementado e retry limitado |
| Head reiniciado | Handles invalidados; handshake e snapshot obrigatórios |
| Main reiniciado | Head limpa comandos transitórios ao receber novo boot ID |
| Link saturado | Controle/eventos preemptam bulk; bulk recebe backpressure |

## 8. Tasks e prioridades

Main:

- safety/watchdog mantêm as maiores prioridades existentes;
- RX/TX do head ficam abaixo de safety e acima de persistência;
- storage remoto é assíncrono;
- callbacks do link não chamam behavior diretamente: publicam no event bus.

Head:

- watchdog;
- link RX/dispatcher;
- render e push;
- câmera sob demanda;
- storage worker;
- touch;
- bulk worker de baixa prioridade.

SPI ISR apenas sinaliza filas; parsing, CRC e filesystem ficam em tasks.

## 9. Atualização e compatibilidade

Cada MCU tem imagem, partições e versão independentes. O main é atualizado
primeiro somente quando aceita a versão antiga e nova do head. Depois o head é
atualizado. Remoção de compatibilidade ocorre em release posterior.

OTA coordenado futuro:

1. validar imagem e compatibilidade;
2. pausar bulk e sincronizar SD;
3. atualizar head;
4. confirmar boot e health;
5. atualizar main;
6. confirmar handshake;
7. rollback individual se necessário.

Nunca atualizar ambos sem uma versão intermediária compatível.

## 10. Fases de migração

### F0 — estrutura e baseline

- reorganizar repositório;
- criar dois projetos ESP-IDF;
- versionar contrato compartilhado;
- builds independentes;
- nenhuma mudança funcional no robô.
- alinhar `CLAUDE.md`, hardware, persistência, arquitetura e roadmap;
- fechar no contrato os tipos de poll, crédito, tempo e storage status.

Status: **concluída em software**. O gate host cobre CRC-16/CRC-32, corrupção,
truncamento, sequence idempotente, reboot/`boot_id`, wrap de sequence e
créditos. Isso não autoriza conectar fisicamente o enlace.

### F1 — enlace

- SPI, IRQ, reset, framing, CRC e heartbeat;
- fault injection para bit flip, timeout e reboot;
- telemetria de latência e retries.

Procedimento físico, gates elétricos, fault injection, soak e rollback:
`docs/DM1_BRINGUP.md`.

Status: **núcleo lógico em andamento**. A FSM C17 testável no host implementa
`HELLO/HELLO_ACK`, snapshot confirmado, heartbeat, `READY/DEGRADED`,
retransmissão idempotente, ACK, timeout explícito, prioridade de
`CONTROL/EVENT` sobre `BULK` e aborto de pendências após novo `boot_id`.
As tasks Layer 2 de main/head já envolvem a FSM, executam polling/serviço SPI
e preservam frames pendentes quando uma transação falha ou expira. Ativação
controlada e validação elétrica continuam pendentes.

A telemetria DM1 registra duração do handshake, RTT de ACK
(`last/avg/max`), latência ponta a ponta incluindo retries, frames TX/RX,
CRC/frame inválido, retries, timeouts e erros/timeouts do transporte. Main e
head emitem um resumo a cada 5 segundos somente quando o enlace está habilitado.

Adaptadores ESP-IDF master/slave já compilam atrás de
`CONFIG_NB_INTER_MCU_SPI_ENABLED=n`. Eles usam uma transação física fixa de
4.124 bytes, buffers DMA estáticos e fila DM1 limitada a frames de 256 bytes.
Isso cobre controle, handshake e diagnóstico; BULK grande ainda não está
liberado e receberá pool dedicado em DM5. O boot chama os serviços, mas com a
flag desligada eles retornam `ESP_ERR_NOT_SUPPORTED` antes de tocar GPIO/SPI.

### F2 — display remoto

- criar facade visual no main;
- migrar display, render, expression e overlays para o head;
- manter fallback local por flag até validação;
- mover LovyanGFX para o head ao final.

Scaffold preparado em `docs/DM2_DISPLAY_MIGRATION.md`: contrato visual
semântico, fila limitada no main e receptor validado no head. O driver físico
permanece desabilitado até o fechamento dos gates de DM1.

### F3 — touch do display

- head publica eventos crus normalizados;
- main decide ações;
- debounce e calibração permanecem locais ao head.

### F4 — câmera

- migrar camera HAL, camera service e preview;
- preview permanece local;
- main recebe métricas e JPEG apenas sob demanda.

### F5 — storage remoto

- storage server no head e client assíncrono no main;
- migrar logs, LTM, assets de áudio e diagnósticos;
- validar power-loss e recuperação.
- validar prebuffer/refill de áudio e política de overflow da LTM.

### F6 — remoção do legado

- remover display, câmera, SD e LovyanGFX do main;
- remover flags temporárias;
- fechar documentação e matriz de testes.

## 11. Gates de aceitação

Cada fase exige:

- build limpo dos dois firmwares com `-Werror`;
- testes host do protocolo;
- soak mínimo de 8 horas sem perda de heartbeat;
- reboot isolado de cada MCU;
- desconexão e reconexão física do link;
- CRC corrompido e frame truncado;
- SD removido, cheio e com erro de escrita;
- bulk concorrente com touch e comandos visuais;
- playback remoto sob link saturado: zero underrun em 30 minutos de assets
  contínuos, com JPEG, logs e LTM concorrentes;
- latência até o primeiro som: p95 menor que 150 ms para asset em cache e menor
  que 500 ms para asset frio no SD;
- fila LTM saturada e head ausente por 8 horas, comprovando descarte por
  prioridade e continuidade da persona via NVS;
- exaustão/restituição de créditos por canal sem deadlock;
- confirmação de que falha do head não interfere em `motion_safety`;
- rollback documentado e testado.

Metas iniciais:

- comando visual p95 menor que 20 ms;
- evento de touch p95 menor que 30 ms;
- zero bloqueio de safety por link ou SD;
- zero corrupção após power-loss durante commit;
- recuperação automática do link em até 3 s.

## 12. Limpeza permitida

Podem ser removidos sem perda de produto:

- `build/`, `build_*`, caches e logs gerados;
- `sdkconfig` e `sdkconfig.old` gerados;
- `managed_components/` regenerável;
- outputs de testes e arquivos temporários.

Código funcional, documentos de decisão, testes e assets versionados só são
removidos quando uma fase tiver substituição validada e commit de rollback.
