# NodeBot — Visao Geral do Projeto

## O que e o NodeBot

NodeBot e um companion robot desktop autonomo, construido sobre o ESP32-S3, capaz de:

- Exibir expressividade visual via display e LEDs
- Interagir por voz (microfone + speaker)
- Executar movimentos fisicos controlados (2 servos de bus serial)
- Processar sensores ambientais e de contato
- Operar offline-first, sem dependencia de nuvem para funcionalidade basica
- Evoluir progressivamente com novos comportamentos e servicos

## Filosofia do projeto

> "Primeiro consolidar uma base de firmware e infraestrutura confiavel.
> Somente depois integrar servicos como IMU, motion, touch, audio, camera e comportamento.
> Cada novo subsistema deve entrar sobre uma base ja validada, observavel e com risco controlado."

O NodeBot nao e um prototipo descartavel. E uma plataforma que deve crescer sem se tornar fragil.

## Objetivos tecnicos

- Firmware robusto, previsivel e com fail-safe em todas as camadas criticas
- Observabilidade real: logs estruturados, metricas de sistema, estado de energia visivel
- Seguranca fisica: servos com safety layer antes de qualquer movimento
- Seguranca energetica: politica de bateria desde o primeiro boot
- Arquitetura em camadas com event bus para desacoplamento
- Integracao progressiva de subsistemas sobre base validada

## Restricoes

- Hardware congelado — nenhum componente pode ser trocado salvo risco critico incontornavel
- Framework: ESP-IDF (nao Arduino) — necessario para controle de recursos e tempo real
- Linguagem: C (C17)
- Operacao offline-first — funcionalidade core nao depende de conectividade
- Bateria LiPo 1S — restricao real de energia com risco de dano por overdischarge

## Stack tecnologico

| Categoria        | Escolha                                 |
|------------------|-----------------------------------------|
| MCU              | ESP32-S3 (Xtensa LX7 dual-core 240MHz) |
| Framework        | ESP-IDF (versao estavel mais recente)   |
| RTOS             | FreeRTOS (integrado ao ESP-IDF)         |
| Linguagem        | C17                                     |
| Build system     | CMake via idf.py                        |
| Armazenamento    | NVS (config) + FAT32/VFS (microSD)     |
| Debug            | UART (921600 baud) + microSD log        |

## Estrutura de desenvolvimento

O projeto e desenvolvido em blocos sequenciais onde cada bloco estabelece pre-requisitos para o proximo:

```
BLOCO 0 — Fundacao (boot, log, config, power, event bus)
BLOCO 1 — Bring-up de hardware low-risk (display, LEDs, microSD)
BLOCO 2 — Validacao de power path (medicoes fisicas reais)
BLOCO 3 — Servos (comunicacao e safety layer antes de qualquer movimento)
BLOCO 4 — Sensores (touch, IMU)
BLOCO 5 — Audio (microfone, speaker)
BLOCO 6 — Camera
BLOCO 7 — Comportamento e persona
BLOCO 8 — Integracao total e validacao longa
```

Ver `docs/ROADMAP.md` para detalhamento de cada etapa.

## Documentacao tecnica

| Documento                     | Conteudo                                                  |
|-------------------------------|-----------------------------------------------------------|
| `docs/HARDWARE.md`            | Especificacao completa do hardware, barramentos, pinagem  |
| `docs/ARCHITECTURE.md`        | Camadas, event bus, boot phases, politica de erros        |
| `docs/ROADMAP.md`             | Roadmap macro e etapas detalhadas com criterios           |
| `docs/ENERGY.md`              | Power path, brownout, boot safety, degradacao             |
| `docs/SERVO_SAFETY.md`        | Motion safety, pre-requisitos, testes de seguranca        |
| `docs/RISKS.md`               | Riscos criticos e mitigacoes                              |
| `docs/INTEGRATION_STRATEGY.md`| Ordem de integracao progressiva dos subsistemas           |
