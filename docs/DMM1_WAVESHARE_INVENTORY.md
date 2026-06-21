# DMM.1 - Inventario eletrico e matriz de recabeamento da Waveshare

**Status:** consolidado documentalmente em 2026-06-20  
**Escopo:** inventario, matriz de recabeamento, riscos, gates e rollback.  
**Nao entra:** qualquer alteracao de firmware, selecao de build, energizacao de
perifericos, avance para DMM.2 ou criacao de subfases novas.

## 1. Fontes usadas

- `CLAUDE.md`
- `docs/DUAL_MCU_MIGRATION_ROADMAP.md`
- `docs/HARDWARE.md`
- `docs/GPIO_DUAL_MCU.md`
- `docs/ENERGY.md`
- `docs/DM1_BRINGUP.md`

## 2. Inventario consolidado

### 2.1 Main - Waveshare ESP32-S3 N32R16V

- Controlador principal alvo: Waveshare ESP32-S3 N32R16V.
- Memoria confirmada em bancada/registro: flash octal OPI de 32 MB e PSRAM de
  16 MB.
- Dominio VDD_SPI de 1.8 V: GPIO47 e GPIO48 nao podem ser usados como logica
  3.3 V.
- GPIO38 pertence ao LED RGB onboard da placa e nao e um GPIO limpo.
- GPIO19 e GPIO20 continuam reservados ao USB nativo.
- GPIO43 e GPIO44 continuam reservados ao console/programacao.

### 2.2 Head - Freenove ESP32-S3 CAM N16R8

- Controlador de cabeca e autoridade multimidia local.
- Mantem display, camera, microSD e console.
- GPIO1, GPIO2, GPIO14, GPIO41 e GPIO42 sao o legado de audio/touch/servo e
  precisam estar livres antes de qualquer recabeamento do enlace.

### 2.3 Perifericos do corpo a migrar para a Waveshare

- Audio: INMP441 + MAX98357A.
- Servos: SCS0009 via FE-TTLinker.
- LEDs externos: 2 x WS2812.
- Touch corporal: fita capacitiva em GPIO2.
- Monitoramento de 5 V: divisor para ADC na main.

### 2.4 Itens que nao entram neste corte

- Display, camera, touchscreen e microSD do head.
- Reescrita de firmware ou selecao de perfil de build.
- Movimento real de servo.
- Qualquer tentativa de energizar perifericos durante este corte documental.

## 3. Matriz de recabeamento

| Recurso | Legado Freenove | Waveshare alvo | Tensao logica | Alimentacao | Gate dono | Rollback |
| --- | --- | --- | --- | --- | --- | --- |
| Enlace SPI inter-MCU | GPIO1/2/14/41/42 + EN do head | GPIO10/11/12/13/14 + GPIO8 | 3.3 V | GND comum; 5 V separado por bancada | DM1 | Desconectar 6 sinais e voltar ao perfil normal sem link |
| INMP441 microfone | GPIO14 (SD/RX) | GPIO39 (SD/RX) | 3.3 V | 3.3 V da placa | DMM.4 | Desconectar mic e manter audio legado desligado |
| MAX98357A speaker | GPIO1 (DIN/TX) | GPIO42 (DIN/TX) | 3.3 V | 5 V de carga | DMM.4 | Desconectar speaker e manter saida muda |
| FE-TTLinker / servos | GPIO19 (RX) e GPIO20 (TX) | GPIO18 (RX) e GPIO17 (TX) | 3.3 V | Rail de 5 V ou 6 V separado | DMM.8 | Desconectar UART e manter torque desabilitado |
| WS2812 externos | GPIO3 | GPIO21 | 3.3 V com level shift | 5 V dos LEDs | DMM.7 | Desconectar fita e manter LED onboard separado |
| Touch corporal | GPIO2 | GPIO2 | 3.3 V | Sensor capacitivo local | DMM.6 | Restaurar legado somente se o recabeamento nao for efetivado |
| Monitor 5 V | N/A no legado | GPIO7 via divisor 68k/56k | ADC <= 3.1 V | Barramento 5 V do sistema | DMM.3 | Desabilitar leitura e manter o boot sem monitor |

## 4. Separacao por componente

### 4.1 Audio

- INMP441:
  - `GPIO39` = `SD/RX`
  - `GPIO40` = `BCLK`
  - `GPIO41` = `WS/LRCK`
- MAX98357A:
  - `GPIO42` = `DIN/TX`
  - `GPIO40` = `BCLK`
  - `GPIO41` = `WS/LRCK`

### 4.2 Touch

- Touch corporal:
  - `GPIO2`

### 4.3 LEDs

- LEDs externos WS2812:
  - `GPIO21`
- LED onboard Waveshare:
  - `GPIO38`

### 4.4 Servo

- FE-TTLinker / servos:
  - `GPIO17` = `TX`
  - `GPIO18` = `RX`

### 4.5 I2C e monitoramento

- I2C sensores:
  - `GPIO4` = `SDA`
  - `GPIO5` = `SCL`
- Monitor 5 V:
  - `GPIO7` = ADC via divisor 68k/56k

### 4.6 Enlace entre placas

- `GPIO8` = `HEAD_RESET`
- `GPIO10` = `LINK_CS`
- `GPIO11` = `LINK_MOSI`
- `GPIO12` = `LINK_SCLK`
- `GPIO13` = `LINK_MISO`
- `GPIO14` = `HEAD_IRQ`

### 4.7 Reservados

- `GPIO19` e `GPIO20` = USB nativo
- `GPIO43` e `GPIO44` = console/programacao
- `GPIO47` e `GPIO48` = dominio VDD_SPI de 1.8 V

## 4. Riscos

1. **GPIO47/48 em 3.3 V por engano**: risco de boot inconsistente ou uso
   eletricamente incorreto do dominio VDD_SPI.
2. **Conflito USB x servo em GPIO19/20**: pode aparecer como perda de
   comunicacao, ruido ou comportamento intermitente durante debug/Wi-Fi.
3. **Rail servo com sag**: TTLinker e servos exigem alimentacao separada e
   capacitor bulk; sem isso ha brownout e reset.
4. **WS2812 sem level shift e sem budget de corrente**: flicker, queda de
   tensao e ruido sobre o restante do sistema.
5. **Touch sem recalibracao**: threshold antigo pode gerar falso positivo ou
   perda de sensibilidade.
6. **Legado ainda dirigindo pinos do head**: impede qualquer banco de enlace e
   falseia o inventario.
7. **Advance indevido para DMM.2**: seleciona perfil de placa cedo demais e
   mistura documentacao com alteracao de firmware.

## 5. Gates

### Gate DMM.1-A - variante e dominio eletrico

- Silk/variante da Waveshare identificados.
- Confirmado o dominio VDD_SPI de 1.8 V na N32R16V.
- GPIO47/48 proibidos para logica 3.3 V.

### Gate DMM.1-B - inventario completo

- Cada dominio recebe origem, destino, tensao, alimentacao, gate e rollback.
- O que fica no head e o que migra para a main esta explicitado.
- O que permanece desconectado foi registrado.

### Gate DMM.1-C - isolamento

- Nenhum firmware foi alterado.
- Nenhum periferico foi energizado por causa deste documento.
- No momento do fechamento documental de `DMM.1`, `DMM.2` permanecia
  bloqueado.

### Gate DMM.1-D - rollback

- A volta ao baseline monolitico e possivel apenas por desconexao fisica dos
  perifericos e retorno ao perfil normal.
- Nenhuma decisao de documentacao depende de uma selecao de build nova.

## 6. Evidencia documental

- O mapa pino a pino base vem de `docs/GPIO_DUAL_MCU.md`.
- As restricoes eletricas da Waveshare e do N32R16V vem de
  `docs/HARDWARE.md`.
- A topologia de energia e a necessidade de rail separado para servos vem de
  `docs/ENERGY.md`.
- O bring-up de DM1 ja deixa o link e o rollback de bancada descritos em
  `docs/DM1_BRINGUP.md`.
- O programa DMM segue o registro fechado de
  `docs/DUAL_MCU_MIGRATION_ROADMAP.md`.

## 7. Resultado

DMM.1 fica consolidado como inventario documental fechado para a Waveshare.
No momento deste registro, o proximo passo continuava sendo `DMM.2` ainda
bloqueado. O status atual da trilha, contudo, deve ser lido no roadmap
canonico, que agora coloca `DMM.2` em `EM VALIDAÇÃO`.

## 8. Comandos de bancada

Usar estes comandos apenas para reproduzir os dois firmwares independentes no
estado atual do projeto. Substituir `COM_MAIN` e `COM_HEAD` pela porta serial
real observada na bancada.

### 8.1 Main-controller Waveshare

Build:

```bat
cmd.exe /c "set SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.dm2.defaults&& call C:\esp\v5.5.4\esp-idf\export.bat && idf.py -B build_dm2 build"
```

Flash:

```bat
cmd.exe /c "call C:\esp\v5.5.4\esp-idf\export.bat && idf.py -B build_dm2 -p COM_MAIN flash"
```

### 8.2 Head-controller Freenove

Build:

```bat
cmd.exe /c "call C:\esp\v5.5.4\esp-idf\export.bat && idf.py build"
```

Flash:

```bat
cmd.exe /c "call C:\esp\v5.5.4\esp-idf\export.bat && idf.py -p COM_HEAD flash"
```
