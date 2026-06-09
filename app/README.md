# NoiseBot App

Dashboard amigavel do usuario para o NoiseBot.

O app segue a fronteira de produto do NoiseBot:

- `components/` e `main/`: firmware ESP32-S3 leve, sem UI pesada.
- `server/`: backend local, IA, TTS, STT, visao, rotina e APIs.
- `app/`: interface de produto para o usuario.

O app fala sempre com o `server`. Ele nao deve depender de rotas diretas do
firmware.

## Stack

- React + TypeScript + Vite
- Tailwind CSS
- Lucide React para icones
- pnpm

## Rodando

```bash
cd app
pnpm install
pnpm dev
```

Por padrao o app considera o server em `http://localhost:8765`. Para alterar:

```bash
VITE_NOISEBOT_SERVER_URL=http://localhost:8765 pnpm dev
```

Build de producao:

```bash
pnpm build   # tsc -b && vite build
```

## Estrutura

```text
src/
├── App.tsx               # Layout + roteamento (slim, ~220 linhas)
├── api.ts                # Todas as chamadas HTTP ao server
├── main.tsx              # Entry point
├── vite-env.d.ts
├── hooks/
│   └── useAppState.ts    # Estado global e acoes (extraido do App)
├── lib/
│   ├── classes.ts        # Constantes de classe Tailwind reutilizaveis
│   ├── format.ts         # Utilitarios de formatacao puros
│   └── voice.ts          # Logica de pipeline de voz
├── components/           # Componentes atomicos/moleculares
│   ├── AudioSamplesPanel.tsx
│   ├── ControlPanel.tsx
│   ├── DiagnosticCard.tsx
│   ├── ErrorLog.tsx
│   ├── InfoRow.tsx
│   ├── LabeledField.tsx
│   ├── Metric.tsx
│   ├── ServiceTile.tsx
│   ├── StatusPill.tsx
│   ├── ToggleRow.tsx
│   ├── TurnBubble.tsx
│   ├── Vital.tsx
│   ├── VoiceAlertBanner.tsx
│   ├── VoiceSessionHistory.tsx
│   └── VoiceStage.tsx
└── views/                # Uma view por secao do nav
    ├── BasicSettingsView.tsx
    ├── DevConsoleView.tsx
    ├── DevIntegrationsView.tsx
    ├── DevTelemetryView.tsx
    ├── InteractionView.tsx
    ├── RoutineView.tsx
    ├── UserHomeView.tsx
    └── UserProfileView.tsx
```

## Navegacao

**User mode:** Inicio · Perfil · Interacao · Rotinas · Ajustes

**Dev mode:** Telemetria · Integracoes · Sistema

A pagina "Visao" foi removida — camera (OV2640) esta adiada no hardware.
A pagina "Sensores" foi removida — IMU (MPU-6050) esta adiado no hardware.

## Principios

- Home mostra status real do robo + ultima interacao + resumo de rotinas.
- Ajustes tem salvamento individual por controle (volume e LEDs separados).
- Nenhum PlannedFeature ou DisabledButton no codigo — so funcionalidades vivas.
- Logs, bridge AFE e diagnostico ficam no modo Dev.
- Camera e IMU so aparecem quando o hardware for ativado.
