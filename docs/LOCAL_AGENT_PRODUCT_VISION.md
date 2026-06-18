# NoiseBot — Companheiro Físico e Agente Local Privado

**Status:** direção estratégica ativa
**Registrado em:** 2026-06-18
**Escopo principal:** server, dashboard e integração com o robô

## Decisão de Produto

O NoiseBot evolui como dois produtos integrados:

1. **Companheiro físico:** o ESP32 continua responsável por corpo, presença,
   expressão, toque e voz.
2. **Agente local privado:** o servidor se torna o cérebro de trabalho, com
   dashboard multimodal, documentos, pesquisa, memória controlável e
   automações.

Essa evolução deve aproveitar a máquina local sem transformar o firmware em
um computador de uso geral. Processamento pesado, arquivos, indexação, modelos
e integrações externas pertencem ao servidor. O firmware recebe apenas o
resultado necessário para a interação física.

## Princípios Inegociáveis

- **Local-first:** dados e funções essenciais permanecem na máquina.
- **Privacidade por padrão:** nuvem somente com autorização ou quando a
  capacidade local não for suficiente.
- **Firmware preservado:** anexos e conteúdo bruto não atravessam o bridge.
- **Resposta adequada ao contexto:** tarefas silenciosas não acionam TTS,
  movimentos ou interrupções do robô.
- **Memória explícita:** nada entra em memória pessoal automaticamente sem uma
  política clara e visível.
- **Controle do usuário:** memórias, documentos, projetos e automações podem ser
  revisados, editados e apagados.
- **Ações seguras:** operações destrutivas ou com efeito externo exigem
  confirmação.
- **Fontes rastreáveis:** respostas sobre documentos e web devem indicar suas
  fontes.
- **Degradação graciosa:** falha de modelo, internet ou ferramenta não deve
  derrubar o companheiro físico.

## Modos de Resposta

| Modo | Dashboard | Robô |
| --- | --- | --- |
| **Só dashboard** | Resposta completa | Nenhum TTS, movimento, expressão ou evento de sessão |
| **Resumir para o robô** | Resposta completa | Resumo curto e opcional |
| **Conversar com o robô** | Registro e detalhes | Resposta falada e comportamento normal |

O modo escolhido deve ser explícito por interação e poderá ter um padrão por
projeto.

## Capacidades Planejadas

### 1. Workspace Multimodal

- Enviar imagens, áudios, PDFs, DOCX e TXT.
- Perguntar, resumir, comparar e extrair informações.
- Manter a resposta completa no dashboard.
- Enviar ao robô somente um resumo quando solicitado.

### 2. Biblioteca Pessoal Local

- Selecionar documentos para indexação.
- Fazer busca semântica e RAG local.
- Responder citando arquivo, página e trecho.
- Reindexar ou remover documentos sob controle do usuário.
- Manter os dados na máquina.

### 3. Áudio Inteligente

- Transcrever reuniões e gravações.
- Separar tópicos, decisões e pendências.
- Gerar tarefas e lembretes após confirmação.
- Comparar transcrições com documentos enviados.

### 4. Visão Prática

- Explicar erros e interfaces em screenshots.
- Analisar fotos, objetos, textos e problemas visuais.
- Aplicar OCR a notas, recibos e documentos.
- Oferecer a câmera do servidor como ferramenta opcional.

### 5. Pesquisa Profunda

- Combinar Tavily com leitura segura das páginas.
- Tratar conteúdo externo como dado não confiável.
- Comparar fontes e apontar divergências.
- Produzir relatórios com citações.
- Cruzar informações da web com arquivos locais.

A leitura de páginas somente entra após proteção SSRF, limites de download,
validação de tipo de conteúdo e isolamento contra prompt injection.

### 6. Memória Controlável

- Comandos explícitos como “lembre disso” e “esqueça isso”.
- Separação entre memórias pessoais, projetos e preferências.
- Tela para revisar, editar e apagar todas as memórias.
- Política clara para qualquer memória sugerida automaticamente.

### 7. Projetos e Sessões

- Organizar conversas por projeto.
- Dar a cada projeto documentos, memória e instruções próprias.
- Evitar vazamento de contexto entre projetos.
- Exemplos: NoiseBot, finanças, estudos e casa.

### 8. Ferramentas Locais

- Agenda, timers, notas e listas.
- Leitura e criação de arquivos em áreas autorizadas.
- Análise de logs e código.
- Prévia e confirmação antes de ações destrutivas.

### 9. Automação Local

- Resumo diário.
- Monitoramento de arquivos, serviços e eventos.
- Alertas no dashboard.
- Interrupção pelo robô somente quando a prioridade justificar.
- Histórico visível de execuções, falhas e ações tomadas.

### 10. Painel Operacional

- Modelo usado, tokens, latência e ferramentas.
- Contextos e fontes utilizados.
- Indicação clara de execução local, web ou híbrida.
- Opção de repetir a tarefa com outro modelo.

### 11. Roteamento entre Modelos

- Modelo pequeno para classificação e comandos.
- Modelo principal para análise e síntese.
- Modelos especializados para visão e transcrição.
- Nuvem somente mediante política e autorização.

## Sequência de Entrega

| Ordem | Entrega | Resultado de saída |
| --- | --- | --- |
| ~~1~~ | ~~Anexos de imagem~~ | **CONCLUÍDO** — imagem enviada, exibida e analisada sem chegar ao firmware |
| ~~2~~ | ~~Modo “só dashboard”~~ | **CONCLUÍDO** — tarefa silenciosa sem TTS, movimento ou sessão no robô |
| ~~3~~ | ~~Documentos com citações~~ | **CONCLUÍDO** — PDF, DOCX e TXT respondidos com marcadores rastreáveis |
| 4 | **Áudio e transcrição — próximo passo** | Áudio enviado, transcrito e resumido com decisões e tarefas |
| 5 | Biblioteca local/RAG | Coleções locais indexadas, pesquisáveis e removíveis |
| 6 | Pesquisa profunda | Relatório web seguro, comparativo e citado |
| 7 | Projetos, memória e automações | Contextos isolados, memória revisável e rotinas locais |

## Primeiro Ciclo Multimodal — Concluído em 2026-06-18

O dashboard possui um fluxo multimodal completo para imagens:

- envia JPEG, PNG e WebP de até 5 MB;
- valida o formato pela assinatura do arquivo;
- mostra a prévia antes do envio;
- exibe a imagem dentro da mensagem do chat;
- abre a imagem ampliada ao clicar;
- mantém as imagens disponíveis após refresh por até 30 minutos;
- limita o cache em memória às 12 imagens mais recentes;
- protege envio e leitura com autenticação;
- oferece resposta silenciosa no dashboard ou resposta falada pelo robô;
- permite pesquisa web no modo dashboard;
- processa anexos e pesquisa inteiramente no server;
- não ocupa a FSM de voz no modo silencioso;
- nunca envia arquivo, bytes ou conteúdo bruto ao firmware.

As imagens não são persistidas em disco. O endpoint de leitura do cache exige
reinício do server após a instalação dessa versão.

### Evidência de Implementação

| Commit | Entrega |
| --- | --- |
| `5daa65b` | Agente multimodal isolado no server |
| `5970963` | Interface para anexar imagens |
| `bb86f18` | Cache das imagens recentes da interação |
| `8994d5b` | Imagens exibidas dentro do chat |
| `052f43e` | Documentação do cache de imagens |
| `806b42d` | Documentação das interações multimodais |

### Validação

- **575 testes aprovados** na validação final do ciclo.
- Build aprovado.
- Revisão visual aprovada.
- A validação intermediária do primeiro corte havia aprovado 573 testes.

## Segundo Ciclo Multimodal — Documentos

Concluído em 2026-06-18:

- PDF, DOCX e TXT de até 10 MB pelo mesmo endpoint autenticado;
- validação por conteúdo, sem confiar apenas na extensão ou MIME declarado;
- PDF com texto citado por página;
- DOCX citado por parágrafo;
- TXT citado por intervalo de linhas;
- seleção local dos trechos mais relacionados à pergunta;
- extração executada fora do event loop;
- proteção contra DOCX inválido e expansão excessiva de ZIP;
- anexos apenas em memória, no cache existente de 12 itens por 30 minutos;
- documento reaberto pelo histórico enquanto estiver no cache;
- conteúdo tratado como dado externo não confiável;
- nenhum byte ou conteúdo bruto enviado ao firmware.

Limitação deste corte: PDF escaneado sem camada textual ainda exige OCR, que
fica para a evolução de visão prática.

Validação:

- **580 testes do server aprovados**, incluindo PDF real, DOCX e TXT;
- build de produção do dashboard aprovado;
- `git diff --check` aprovado.

## Estado após o Segundo Ciclo

| Entrega | Estado |
| --- | --- |
| Imagens JPEG/PNG/WebP no dashboard | **Concluído** |
| Prévia, exibição no chat e ampliação | **Concluído** |
| Cache autenticado em memória, 12 imagens/30 minutos | **Concluído** |
| Modo `response_mode=dashboard` silencioso e isolado | **Concluído** |
| Resposta falada pelo robô | **Concluído** |
| Pesquisa web no modo dashboard | **Concluído** |
| Modo “resumir para o robô” | **Planejado** |
| PDF, DOCX e TXT com citações | **Concluído** |
| Áudio enviado e transcrição | **Próximo passo** |
| Biblioteca local/RAG | **Planejado** |
| Leitura profunda de páginas | **Planejado; depende das proteções de segurança** |
| Projetos, memória revisável e automações | **Planejado; ferramentas locais básicas já existem** |

## Critérios Gerais de Aceite

Uma capacidade dessa visão só pode ser marcada como concluída quando:

- possui fluxo utilizável no dashboard;
- respeita o modo de resposta escolhido;
- não envia anexos ou conteúdo bruto ao firmware;
- expõe erros sem perder silenciosamente dados;
- possui testes para limites, formatos inválidos e isolamento;
- informa quando usou modelo local, web ou nuvem;
- permite remover os dados persistidos que criou;
- documenta riscos, configuração e limitações.

## Relação com o Roadmap

Este documento define **para onde o produto vai**. O
[`ROADMAP.md`](./ROADMAP.md) define **o que está em execução agora**, com
prioridade e critérios de aceite.

Ao iniciar uma entrega desta visão, ela deve ser desdobrada no roadmap antes da
implementação. Mudanças de prioridade não devem apagar esta direção estratégica;
devem apenas alterar a ordem de execução registrada.
