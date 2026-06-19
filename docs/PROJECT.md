# NoiseBot — Documento de Projeto

## Visão Geral

NoiseBot é um companion robot desktop expressivo. O objetivo central não é um gadget com features de vitrine: é um robô que parece vivo, que responde ao toque, que olha para você, que se move com intenção, e que tem uma persona recognoscível mesmo em repouso.

Com dois servos, um display de 2", dois LEDs, microfone, speaker e fita de cobre, o NoiseBot entrega presença real — desde que a base técnica seja sólida, o comportamento seja disciplinado, e cada grau de liberdade seja usado com intenção.

## Objetivos do Produto

1. **Presença:** o robot parece vivo mesmo quando está em repouso.
2. **Carisma:** personalidade consistente e recognoscível.
3. **Expressividade visual:** face procedural que comunica estado emocional.
4. **Linguagem corporal:** movimentos físicos curtos, intencionais e econômicos.
5. **Interação por voz:** reação ao som, playback de áudio expressivo.
6. **Touch como vínculo:** fita de cobre é o canal físico de intimidade com o usuário.
7. **Robustez:** firmware estável para operação contínua, sem crashes.
8. **Evolução:** arquitetura que cresce sem reescrita — câmera, IMU e bateria são expansões planejadas.
9. **Agente local privado:** o servidor amplia o NoiseBot com workspace
   multimodal, documentos, pesquisa, memória controlável e automações sem
   transferir processamento pesado ao firmware.

A direção estratégica dessa integração está registrada em
[`LOCAL_AGENT_PRODUCT_VISION.md`](./LOCAL_AGENT_PRODUCT_VISION.md).

## Princípios de Comportamento

| Princípio             | Descrição                                                                   |
| --------------------- | --------------------------------------------------------------------------- |
| Presença em repouso   | Idle não é estático. É movimento mínimo vivo.                               |
| Gaze como atenção     | Direção do olhar precede qualquer expressão.                                |
| Neutral forte         | A posição de repouso da face é desenhada com intenção.                      |
| Movimento econômico   | Cada ação tem início claro, destino claro, retorno limpo ao neutral.        |
| Touch é vínculo       | Resposta imediata (<80ms percebido), calorosa, contextual.                  |
| Timing social         | Pausas, pre-speech signals, post-speech settle são parte do produto.        |
| Coordenação de output | Face, motion e áudio são orquestrados pelo conductor — nunca independentes. |
| Estado interno modula | Emotion model (valência × ativação) influi sutilmente em toda expressão.    |
| Identidade local      | O robot mantém um perfil offline-first do usuário atual; reconhecimento de voz/face não é requisito para saber com quem está interagindo. |

## Hardware e migração dual-MCU

Ver `docs/HARDWARE.md` para especificações completas e mapa de pinos.

**Estado-alvo:** Waveshare ESP32-S3 N32R16 como main-controller para áudio,
comportamento, safety, servos, LEDs e touch corporal; Freenove ESP32-S3 CAM
N16R8 como head-controller para ST7789, touchscreen futuro, OV2640 e o único
microSD.

**Migração:** o baseline monolítico ainda funciona na Freenove. Periféricos são
movidos por DM1-DM6, com rollback por fase. A OV2640 entra em DM4; análise
semântica e reconhecimento continuam no server.

**Adiados:** MPU-6050 IMU, LiPo + bq25185 + MAX17048 + TPS61088.

## Plataforma Técnica

- **MCUs:** Waveshare ESP32-S3 N32R16 + Freenove ESP32-S3-WROOM CAM N16R8
- **Framework:** ESP-IDF (não Arduino)
- **Linguagem:** C17 (exceto display/render LovyanGFX no head)
- **RTOS:** FreeRTOS (incluso no ESP-IDF)
- **Stack gráfica:** LovyanGFX (contrato arquitetural, ver `docs/ARCHITECTURE.md`)
- **Build:** CMake via `idf.py`
- **Alimentação:** 5V desktop, fonte oficial Raspberry Pi 4 (3A)

## Definição de "Base Sólida Concluída"

O `docs/ROADMAP.md` atual funciona como painel vivo de produto: mostra decisoes,
fila P0/P1/P2, itens feitos e criterios de aceite. Logs longos e criterios
antigos completos podem ser mantidos em arquivos locais de limpeza, mas nao
precisam ser versionados.

Nenhuma feature de expressividade ou comportamento é liberada antes deste marco.

## Roadmap De Produto

| Horizonte | Resultado |
| --- | --- |
| Base consolidada | Display/render, event bus, camadas, voice/bridge e feedback visual |
| Agora | Touch confiavel, presence/camera leve, agenda local, status rail, tela de perfil/persona local e documentacao limpa |
| Proximo | Motion safety, persistencia/SD, WiFi offline-first, observabilidade e voice polish |
| Adiado | Servos reais, IMU, bateria e camera avancada |

Ver `docs/ROADMAP.md` para o roadmap ativo.
