# NoiseBot

NoiseBot e um companion robot desktop expressivo baseado em ESP32-S3. O objetivo
nao e apenas ligar perifericos: o projeto busca criar um robo pequeno que parece
presente, responde ao toque, reage a voz, exibe uma face procedural viva e
mantem uma personalidade consistente mesmo em repouso.

O produto e **offline-first**. O firmware precisa funcionar sem internet, sem
cloud e sem depender de dashboard externo. Recursos mais pesados, como LLM,
STT, ferramentas de operacao e dashboard, ficam no `server/` local e no
dashboard `app/`.

## Estado Atual

| Area | Estado |
| --- | --- |
| Firmware ESP-IDF | Base ativa em C17, com componentes organizados por camadas |
| Display/face | Pipeline LovyanGFX, render service e overlays visuais em evolucao |
| Voz/server | Base funcional com server local e pipeline de voz em refinamento |
| Touch | Ativo, mas em revisao de sensibilidade e confiabilidade |
| Camera | Infraestrutura inicial; presence detection leve esta no roadmap atual |
| Servos | Nao conectados; movimento real bloqueado ate `motion_safety` estar verde |
| WiFi | Conveniencia local, nunca dependencia do produto |
| Documentacao | Roadmap ativo reorganizado em `docs/ROADMAP.md` |

## Principios Do Projeto

- **Presenca antes de features:** idle, face, timing e resposta ao usuario sao
  parte central do produto.
- **Firmware enxuto:** sem dashboard embarcado, sem WebSocket de UI no ESP32 e
  sem gerenciador generico de arquivos SD no firmware.
- **Camadas respeitadas:** comportamento nao chama HAL diretamente; comunicacao
  entre camadas distantes passa pelo event bus.
- **Safety primeiro:** nenhum movimento de servo e liberado antes do gate de
  `motion_safety`.
- **Offline-first:** WiFi e o server local ajudam, mas o robo deve continuar
  operando sem rede.
- **Sem Arduino:** toda a base embarcada usa ESP-IDF, FreeRTOS e APIs `esp_*`.

## Stack

| Camada | Tecnologias |
| --- | --- |
| Firmware | ESP-IDF, FreeRTOS, C17, CMake, LovyanGFX no display |
| Hardware | ESP32-S3 N16R8, ST7789, WS2812, INMP441, MAX98357A, touch capacitivo, microSD |
| Server | Python 3.10+, `aiohttp`, adapters locais para voz/LLM/operacao |
| Dashboard | React, TypeScript, Vite, Tailwind, lucide-react |
| Testes | Pytest para server; `idf.py build` para firmware |

## Estrutura Do Repositorio

```text
Noisebot/
+-- app/                 # Dashboard externo em React/Vite
+-- assets/              # Assets do produto e recursos visuais/sonoros
+-- bridge/              # [legado] pre-server, candidato a remocao
+-- bridge_v2/           # [legado] absorvido por server/, candidato a remocao
+-- components/          # Componentes ESP-IDF do firmware
|   +-- infra/           # Boot, config, event bus, persistence, watchdog, safety
|   +-- hal/          # HAL de display, audio, servo, LED, touch, SD
|   +-- services/        # Render, audio, touch, camera, agenda, overlay, voice
|   +-- behavior/        # State machine, behavior engine, emotion model
|   +-- persona/         # Persona e memoria de longo prazo
+-- docs/                # Projeto, arquitetura, hardware, roadmap e referencias
+-- main/                # app_main do firmware ESP-IDF
+-- models/              # Modelos locais quando aplicavel
+-- server/              # Companion server local e API operacional
+-- tools/               # Scripts auxiliares
+-- CMakeLists.txt       # Projeto ESP-IDF
+-- dev.ps1              # Sobe server + dashboard local
+-- partitions.csv       # Tabela de particoes do firmware
+-- sdkconfig.defaults   # Configuracao base do ESP-IDF
```

Pastas locais como `docs/archive/`, `docs/history/` e `docs/modules/` podem ser
usadas durante limpezas ou auditorias, mas nao sao versionadas por padrao.

## Arquitetura Em Camadas

```text
Layer 0: ESP-IDF / FreeRTOS / Hardware
Layer 1: HAL
Layer 2: Infra
Layer 3: Safety
Layer 4: Services
Layer 5: Core Services
Layer 6: Behavior
Layer 7: Persona
Layer 8: Expansoes futuras
```

Regras importantes:

- HAL nao publica diretamente no event bus.
- Services orquestram HAL e publicam estados relevantes.
- Behavior/Core nao acessam hardware diretamente.
- Safety tem autoridade de veto sobre movimento.
- `IDLE` e o baseline visual/comportamental persistente.

## Desenvolvimento Local

### Server e dashboard

Para subir o companion server e o dashboard em modo desenvolvimento:

```powershell
.\dev.ps1
```

Por padrao, o script usa:

| Servico | URL |
| --- | --- |
| Dashboard Vite | `http://127.0.0.1:5173` |
| NoiseBot Server | `http://127.0.0.1:8765` |
| Robo TCP | `192.168.1.30:9000` |
| Robo HTTP | `http://192.168.1.30` |

Para apontar para outro IP do robo:

```powershell
.\dev.ps1 -RobotHost 192.168.1.50
```

O dashboard oficial roda apenas pelo Vite em `http://127.0.0.1:5173`. A porta
`8765` e reservada para a API operacional do `noisebot_server`; a raiz `/`
retorna apenas informacoes da API e nao serve mais a interface web.

### App

```powershell
cd app
pnpm install
pnpm dev
```

Build de producao:

```powershell
cd app
pnpm build
```

### Server

```powershell
cd server
python -m pip install -e ".[dev]"
python -m noisebot_server --host 192.168.1.30 --port 9000 --env .env
```

### Firmware

O firmware usa ESP-IDF. Antes de compilar, carregue o ambiente do ESP-IDF da sua
instalacao local.

```cmd
call C:\esp\v5.5.4\esp-idf\export.bat
idf.py build
```

Flash, ajustando a porta conforme o dispositivo:

```cmd
idf.py -p COM12 flash monitor
```

## Validacao

Testes do server:

```powershell
cd server
python -m pytest
```

Testes do bridge legado (`bridge/`/`bridge_v2/` — candidatos a remocao, ja sem
uso em runtime; `server/` roda de forma autocontida):

```powershell
$env:PYTHONPATH = "D:\Projetos\Noisebot\bridge"
python -m pytest bridge\tests
```

Build do firmware:

```cmd
idf.py build
```

O firmware deve compilar sem warnings relevantes. O projeto trabalha com a meta
de `-Wall -Wextra -Werror`.

## Documentacao

| Documento | Conteudo |
| --- | --- |
| `docs/README.md` | Indice dos documentos vivos e referencias mantidas |
| `docs/PROJECT.md` | Visao de produto, principios e plataforma tecnica |
| `docs/ROADMAP.md` | Painel ativo: decisoes atuais, fila P0/P1/P2 e criterios |
| `docs/ARCHITECTURE.md` | Camadas, contratos, event bus, services e memoria |
| `docs/HARDWARE.md` | Pinos, perifericos e restricoes fisicas |
| `docs/SERVO_SAFETY.md` | Protocolo para liberar movimento real |
| `docs/CAMERA_INTEGRATION.md` | Estrategia e achados de camera/visao |
| `docs/VOICE_AUDIO_V2_ARCHITECTURE.md` | Contrato consolidado do pipeline de voz v2 |

## Roadmap Atual

O roadmap ativo fica em `docs/ROADMAP.md`. O foco atual esta em:

- melhorar sensibilidade e confiabilidade do touch;
- estabilizar presence detection leve via camera;
- tornar timers, lembretes e alarmes funcionais localmente;
- criar um status rail invisivel para icones persistentes;
- manter a documentacao limpa e decisiva.

Itens como servo real, IMU, bateria, wake word customizada e camera avancada
continuam fora do ciclo atual ate que as dependencias certas estejam prontas.

## Guardrails Para Contribuicao

- Nao usar Arduino.
- Nao mover pinos de camera DVP ja conectados na placa.
- Nao adicionar movimento fisico sem passar por `motion_safety`.
- Nao colocar framebuffer de display em SRAM; sprites devem usar PSRAM.
- Nao fazer escrita SD sincrona em task de alta prioridade.
- Nao transformar o firmware em servidor web rico; UI e diagnostico ficam fora
  do ESP32.
- Ao mudar comportamento significativo, publique estado/evento em vez de chamar
  subscritores diretamente.

## Licenca

Licenca ainda nao definida.
