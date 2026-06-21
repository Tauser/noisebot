# DMM.1 - Baseline fisico confirmado da bancada

**Status:** confirmado por relato da bancada nesta sessao  
**Objetivo:** registrar o estado fisico atual antes de qualquer tentativa de
DMM.2.  
**Nao entra:** alteracao de firmware, selecao de perfil, recabeamento novo,
energizacao de perifericos adicionais ou criacao de novas subfases.

## 1. Resumo

O estado fisico atual informado para a bancada e:

- **Freenove/head:** permanece com os perifericos locais integrados e ligados.
- **Waveshare/main:** mantem somente a comunicacao com a Freenove.
- **Nao ha, neste corte, migracao fisica adicional para a Waveshare.**

## 2. Estado da Freenove/head

Conforme o relato atual da bancada, a Freenove segue com:

- display ligado;
- camera ligada;
- microSD ligado;
- console ligado;
- perifericos locais legados ainda presentes na placa enquanto o recabeamento
  para a main nao e concluido.

## 3. Estado da Waveshare/main

Conforme o relato atual da bancada, a Waveshare segue com:

- enlace de comunicacao com a Freenove ligado;
- nenhum periferico de corpo migrado neste corte;
- nenhum audio, servo, LED externo, touch corporal ou sensor adicional
  assumido por ela ainda.

## 4. Interpretação para o roadmap

Este baseline reforca as conclusoes ja documentadas:

- DMM.1 continua sendo o corte documental que descreve o inventario e a
  matriz de recabeamento;
- No momento deste baseline, `DMM.2` continuava bloqueada porque a selecao de
  perfil e o HAL seguro da Waveshare ainda nao podiam ser iniciados como
  implementacao;
- camera e SD permanecem no head;
- o enlace entre as placas e o unico ponto fisico compartilhado neste momento.

## 5. Proxima verificacao util

Se houver interesse em continuar sem tocar em firmware, o proximo passo
documental mais seguro e montar uma lista curta de:

1. fios efetivamente presentes na Freenove;
2. fios efetivamente presentes na Waveshare;
3. o que ainda impede a entrada formal em DMM.2.

O status atual da trilha deve ser lido no roadmap canonico; este baseline fica
preservado como retrato do estado fisico anterior ao inicio de `DMM.2`.

Esse documento nao substitui inspecao de bancada, mas serve como referencia
de estado atual para a equipe.
