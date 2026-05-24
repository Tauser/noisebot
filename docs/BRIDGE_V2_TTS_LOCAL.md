# BRIDGE_V2_TTS_LOCAL.md — TTS local com Piper

Este guia instala o TTS local usado pelo `bridge_v2` para transformar respostas
de texto em áudio PCM enviado ao firmware via `SAY`.

O alvo inicial é Windows, rodando no servidor local/PC do NoiseBot.

## Visão Geral

O `bridge_v2` usa `PiperServerTTS`:

- executável local `piper.exe`;
- modelo de voz `.onnx` + arquivo `.onnx.json`;
- saída `--output_raw` em PCM bruto;
- reamostragem automática do modelo para `16000 Hz`, mono, `int16`;
- chunks de `256` samples, ou `512` bytes, enviados ao firmware.

No dashboard, `TTS ok` significa que o provider está configurado. Para confirmar
que áudio foi gerado e enviado, olhe também:

- `TTS first audio`;
- `First audio out`.

Se esses campos ficam vazios após uma resposta, o bridge não gerou chunks de
áudio naquele turno.

## Download

Fontes oficiais/recomendadas:

- Piper releases: <https://github.com/rhasspy/piper/releases>
- Vozes Piper no Hugging Face: <https://huggingface.co/rhasspy/piper-voices>
- Voz PT-BR Faber medium: <https://huggingface.co/rhasspy/piper-voices/tree/main/pt/pt_BR/faber/medium>

Arquivos necessários para a voz PT-BR:

- `pt_BR-faber-medium.onnx`
- `pt_BR-faber-medium.onnx.json`

Os dois arquivos devem ficar juntos na mesma pasta.

## Estrutura Local Recomendada

No projeto:

```text
D:\Projetos\Noisebot\
├── tools\
│   └── piper\
│       └── piper\
│           └── piper.exe
└── models\
    └── piper\
        ├── pt_BR-faber-medium.onnx
        └── pt_BR-faber-medium.onnx.json
```

Os modelos `.onnx` são dependência local e não devem ser versionados no Git.

## Configuração

Crie ou edite:

```text
D:\Projetos\Noisebot\bridge_v2\.env
```

Com:

```env
NOISEBOT_PIPER_EXECUTABLE=D:\Projetos\Noisebot\tools\piper\piper\piper.exe
NOISEBOT_PIPER_MODEL=D:\Projetos\Noisebot\models\piper\pt_BR-faber-medium.onnx
NOISEBOT_TTS_CACHE_SIZE=64
NOISEBOT_TTS_TARGET_PEAK=12000
```

O `bridge_v2` carrega esse arquivo quando iniciado com:

```powershell
python -m bridgev2 service --env .env
```

ou pelo serviço/atalho que já aponta para esse `.env`.

## Teste Manual do Piper

Antes de testar pelo robô, valide o Piper sozinho:

```powershell
"Ola, teste de voz do NoiseBot." | & "D:\Projetos\Noisebot\tools\piper\piper\piper.exe" --model "D:\Projetos\Noisebot\models\piper\pt_BR-faber-medium.onnx" --output_file "D:\Projetos\Noisebot\models\piper\teste_bridge.wav"
```

Resultado esperado:

- o comando termina sem erro;
- o arquivo `teste_bridge.wav` é criado;
- o WAV toca normalmente no Windows.

## Teste Pelo Bridge

Suba o bridge:

```powershell
cd D:\Projetos\Noisebot\bridge_v2
python -m bridgev2 service --host 192.168.1.30 --port 9000 --env .env
```

Abra:

```text
http://127.0.0.1:8765/
```

No dashboard:

1. confirme `TTS ok`;
2. confirme que `NOISEBOT_PIPER_MODEL` aparece em `/ai/config`;
3. injete ou fale um comando simples, por exemplo `que horas são`;
4. confira se `TTS first audio` e `First audio out` ganharam valores.

Se a resposta aparece em texto mas o robô não fala, use a distinção:

- `TTS first audio` vazio: problema no bridge/Piper/configuração;
- `TTS first audio` com valor: o bridge gerou/enviou áudio; investigar firmware,
  fila `SAY`, volume, I2S ou MAX98357A.

## Troubleshooting

### `TTS ok`, mas sem voz

Verifique primeiro as métricas:

- se `tts_first_audio` não incrementa, o Piper não gerou áudio;
- se `tts_first_audio` incrementa, o problema está depois do bridge.

### `NOISEBOT_PIPER_MODEL não configurado`

O `.env` não foi carregado ou a variável está ausente.

Confirme:

```powershell
Get-Content D:\Projetos\Noisebot\bridge_v2\.env
```

### Modelo existe, mas bridge não fala

Confirme que existe também o JSON ao lado do modelo:

```text
pt_BR-faber-medium.onnx.json
```

O bridge usa esse arquivo para descobrir o `sample_rate` da voz.

### Piper gera WAV, mas bridge não gera chunks

O `bridge_v2` deve usar `--output_raw`. No Windows, o Piper pode escrever texto
de log/caminho no stdout quando usado como WAV em pipe; por isso o provider do
NoiseBot consome PCM bruto e não WAV pelo stdout.

### Áudio picotado ou robô mudo apesar de chunks gerados

No firmware, procure logs:

```text
Bridge SAY dropado: fila cheia drops=...
```

Se aparecer, a fila `SAY` está saturando. Aumentar a fila no firmware ou ajustar
o pacing do `OutputScheduler` resolve o sintoma.

### Volume

O volume do firmware vem de `config_get_volume()` e também pode ser alterado
pelo dashboard do robô. Confirme que não está em `0`.

## Contrato de Áudio

O firmware espera:

| Campo | Valor |
| --- | --- |
| Formato | PCM signed 16-bit little-endian |
| Canais | mono |
| Sample rate | 16000 Hz |
| Chunk | 256 samples |
| Bytes por chunk | 512 bytes |
| Mensagem | `NB_BRIDGE_MSG_SAY` |

O modelo `pt_BR-faber-medium` normalmente trabalha em `22050 Hz`; o bridge
reamostra para `16000 Hz` antes de enviar ao firmware.

O `NOISEBOT_TTS_TARGET_PEAK` normaliza o pico do PCM antes do envio. O valor
recomendado inicial é `12000`: alto o bastante para o MAX98357A sem saturar com
facilidade. Se distorcer, reduza para `8000`; se ficar baixo demais, teste
`14000`.
