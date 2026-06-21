# DMM.2 - Checklist de entrada segura

**Status:** checklist documental de entrada; mantido como referencia mesmo com
`DMM.2` em validacao no roadmap canônico  
**Propósito:** registrar o pacote minimo que precisou existir antes de iniciar
`DMM.2` e servir como checklist de auditoria da subfase.  
**Nao entra:** hardware novo ou criacao de novas subfases. O escopo original
deste documento foi definido antes da implementacao de firmware.

## 1. Intencao da DMM.2

DMM.2 vai tornar a selecao Freenove-legado/Waveshare explicita e segura no
build, mantendo o fallback legado ate o corte final da trilha DMM.13.

Este checklist nasceu para autorizar a entrada em `DMM.2`. Depois da execucao
inicial da subfase, ele continua valendo como referencia do gate de entrada,
mas o status atual de execucao passa a ser definido por
`docs/DUAL_MCU_MIGRATION_ROADMAP.md`.

## 2. Precondicoes obrigatorias

- DMM.1 consolidada e revisada, com matriz de recabeamento fechada.
- Variante da Waveshare N32R16V confirmada em bancada.
- Dominio VDD_SPI de 1.8 V reconhecido e documentado.
- Nenhum GPIO 47/48 tratado como logica 3.3 V.
- Nenhum periferico de corpo energizado durante o corte documental.
- Historicamente, este item exigia `DMM.2` bloqueada ate haver autorizacao
  explicita para alterar build ou firmware.
- Estado atual: a autorizacao ja foi exercida, o roadmap canônico moveu
  `DMM.2` para `EM VALIDAÇÃO`, e este documento permanece apenas como gate de
  entrada/auditoria.

## 3. Evidencias de entrada esperadas

- Referencia unica para o mapa de placa alvo:
  `docs/DMM1_WAVESHARE_INVENTORY.md`.
- Referencia eletrica/pino a pino:
  `docs/GPIO_DUAL_MCU.md` e `docs/HARDWARE.md`.
- Referencia de energia:
  `docs/ENERGY.md`.
- Referencia de ordem e gates:
  `docs/DUAL_MCU_MIGRATION_ROADMAP.md`.

## 4. Checklist tecnico antes de iniciar DMM.2

### 4.1 Definicao de perfil

- [ ] A selecao Freenove-legado/Waveshare esta planejada de forma explicita.
- [ ] O perfil Waveshare nao inclui DVP, display ou SD locais do head.
- [ ] O mapa Freenove permanece como fallback temporario.
- [ ] Nao existe selecao silenciosa do novo mapa em runtime.

### 4.2 Integridade de placa

- [ ] `board_caps` reflete a Waveshare real.
- [ ] `nb_hw_config_main.h` ou equivalente esta alinhado ao hardware alvo.
- [ ] GPIO38, 47 e 48 continuam tratados como restritos.
- [ ] USB nativo e console permanecem reservados.

### 4.3 Exclusao de conflitos

- [ ] Existe lista explicita de conflitos a checar entre link, audio, servo,
  LED, touch e USB.
- [ ] Qualquer remapeamento de GPIO vem acompanhado de rollback documentado.
- [ ] O head continua com seus GPIOs legados livres antes de qualquer
  recabeamento de campo.

### 4.4 Evidencia de rollback

- [ ] O baseline monolitico continua reproduzivel.
- [ ] O retorno ao perfil normal nao exige reescrever firmware novo.
- [ ] A reversao depende apenas de restaurar o cabeamento e o perfil antigo.

## 5. Gates de saida da entrada

Para dizer que a entrada em `DMM.2` estava segura, os seguintes itens precisavam
estar prontos no papel antes da implementacao inicial:

1. matriz de conflitos fechada;
2. perfil de placa descrito;
3. fallback documentado;
4. rollback descrito;
5. link entre documentos e mapa eletrico validado;
6. nenhum risco novo para head, audio, servos ou safety.

## 6. Limite deste checklist

Durante a preparacao original, se surgisse qualquer necessidade de:

- criar novo ID;
- mover GPIO exclusivo do head;
- alterar o build normal;
- ou tocar em firmware;

entao o trabalho deveria parar e a mudanca precisava voltar para o roadmap
canonico antes de seguir.

## 7. Leitura correta deste documento hoje

- Fonte de verdade para o status atual da subfase: `docs/DUAL_MCU_MIGRATION_ROADMAP.md`.
- Este arquivo continua util para auditar se a entrada em `DMM.2` respeitou os
  pre-requisitos.
- Implementacao, build e evidencias correntes de validacao nao devem ser
  inferidos apenas a partir deste checklist.
