# DMM.2 - Matriz preliminar de conflitos e bloqueios

**Status:** documento de preflight preservado como referencia de entrada; o
status da subfase no programa segue o roadmap canônico  
**Objetivo:** listar, de forma conservadora, os conflitos que a `DMM.2`
precisou proteger antes da selecao de perfil e da alteracao inicial de
firmware.  
**Nao entra:** novos IDs. O texto original foi mantido no tempo verbal de
preflight para preservar rastreabilidade.

## 1. Contexto

DMM.2 so pode tratar selecao explicita de placa e HAL da Waveshare quando o
mapa eletrico estiver fechado e quando o fallback legado continuar reproduzivel.
Esta matriz existe para evitar que o corte documental vire uma implementacao
disfarcada. Depois do inicio de `DMM.2`, ela continua valendo como registro dos
conflitos que ainda precisam permanecer protegidos durante a validacao.

## 2. Conflitos principais

### 2.1 GPIOs do head que nao podem ser tratados como livres

| GPIO | Motivo | Risco se reutilizado cedo | Bloqueio esperado |
| --- | --- | --- | --- |
| 1 | Legado de speaker/TTLinker no head | Contencao com audio e servo | Manter legado ate DMM.13 |
| 2 | Legado de touch corporal / HEAD_IRQ | Quebra de evento ou falso toque | Nao mover sem recabeamento total |
| 14 | Legado de mic / HEAD_IRQ / link MISO | Contencao com audio e enlace | Separar funcoes antes de DMM.2 |
| 41 | Legado de BCLK / link SCLK | Quebra de audio ou link | Sem uso silencioso |
| 42 | Legado de WS/LRCK / link MOSI | Contencao entre audio e enlace | Sem remapeamento sem rollback |
| 47 | Dominio VDD_SPI 1.8 V na Waveshare | Uso incorreto de nivel logico | Proibido para 3.3 V |
| 48 | Dominio VDD_SPI 1.8 V na Waveshare | Uso incorreto de nivel logico | Proibido para 3.3 V |

### 2.2 Recursos da Waveshare que exigem protecao documental

| Recurso | Motivo | Risco se a DMM.2 ignorar | Requisito minimo |
| --- | --- | --- | --- |
| GPIO38 onboard | LED RGB da placa | Interpretação errada como GPIO limpo | Tratar como ocupado |
| GPIO19/20 USB nativo | Console e USB PHY | Perda de debug ou conflito de USB | Reservar explicitamente |
| GPIO43/44 console | Programacao/debug | Quebra de acesso serial | Reservar explicitamente |
| GPIO7 monitor 5 V | Instrumentacao de energia | Confusao com sensor funcional | Manter como ADC de monitor |

### 2.3 Conflitos de periferico

| Grupo | Conflito | Risco | Protecao documental |
| --- | --- | --- | --- |
| Audio | INMP441 + MAX98357A | Troca de canais, sample rate ou pino | Descrever o alvo em DMM.1 e manter rollback |
| Servos | FE-TTLinker + SCS0009 | Movimento indevido, contencao de 5 V | Torque continua desabilitado ate DMM.9 |
| LEDs | WS2812 externos + LED onboard | Queda de tensao, flicker, diagnostico ambiguo | Separar status de produto e status de placa |
| Touch | Fita capacitiva + possíveis sensores futuros | Falso positivo / regressao de sensibilidade | Recalibrar somente quando o recabeamento existir |

## 3. Bloqueios de processo

1. Nao alterar `sdkconfig` normal antes da autorizacao inicial da `DMM.2`.
2. Nao mover pinos do head para a main sem matriz de rollback.
3. Nao introduzir selecao automatica de placa por detecao silenciosa.
4. Nao misturar documentacao com implementacao no mesmo passo sem ganho de
   rastreabilidade.
5. Nao criar nova subfase para comportar um conflito descoberto.

## 4. Evidencias que precisam existir antes de DMM.2

- `docs/DMM1_WAVESHARE_INVENTORY.md` fechado.
- `docs/GPIO_DUAL_MCU.md` e `docs/HARDWARE.md` consistentes entre si.
- `docs/ENERGY.md` cobrindo o rail de 5 V e o capacitor bulk.
- `docs/DUAL_MCU_MIGRATION_ROADMAP.md` registrando `DMM.1` fechada e, no ponto
  de entrada original, `DMM.2` ainda bloqueada.
- `docs/DMM2_ENTRY_CHECKLIST.md` como porta de entrada documental.

## 5. Resultado

Esta matriz nao inicia mais a `DMM.2`; ela documenta o que precisou ser
preservado para iniciar a subfase e o que ainda precisa continuar protegido
durante a validacao. O estado correto do programa hoje e:

- DMM.1 consolidada;
- DMM.2 em validacao no roadmap canônico;
- alteracoes de firmware ja existem e devem ser avaliadas contra esta matriz;
- nenhum GPIO exclusivo do head tratado como livre.
