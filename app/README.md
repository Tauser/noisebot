# NoiseBot App

Dashboard amigavel do usuario para o NoiseBot.

O app segue a fronteira de produto do NoiseBot:

- `components/` e `main/`: firmware ESP32-S3 leve, sem UI pesada.
- `server/`: backend local, IA, TTS, STT, visao, rotina e APIs.
- `app/`: interface de produto para o usuario.

O app fala sempre com o `server`. Ele nao deve depender de rotas diretas do
firmware.

## Stack inicial

- React
- TypeScript
- Vite
- CSS proprio
- Lucide React para icones

## Rodando

```bash
cd app
npm install
npm run dev
```

Por padrao o app considera o server em `http://localhost:8765`. Para alterar:

```bash
VITE_NOISEBOT_SERVER_URL=http://localhost:8765 npm run dev
```

## Estrutura

```text
app/
├── src/       # Aplicacao React
├── assets/    # Assets do app
└── test/      # Testes futuros do app
```

## Principios

- Home nao e painel tecnico.
- Rotina, timers, alarmes e agenda sao fluxos centrais.
- Ajustes do dia a dia ficam separados de configuracoes avancadas.
- Visao e captura/análise sob demanda; a camera fica reservada para face detect
  e outros recursos internos de percepção, sem monitoramento contínuo.
- Logs, bridge e detalhes de diagnostico ficam em configuracoes/avancado.
