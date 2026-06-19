# NoiseBot — Plano de Conversas Persistentes e Continuidade de Estudos

**Status:** proposta técnica detalhada, aguardando início da implementação
**Data:** 2026-06-18
**Escopo:** server local, dashboard e integração controlada com voz
**Prioridade de produto:** antes da biblioteca local/RAG

Plano complementar obrigatório para estudo por conversação:
[`BILINGUAL_VOICE_STT_PLAN.md`](./BILINGUAL_VOICE_STT_PLAN.md).

## 1. Objetivo

Permitir que o NoiseBot mantenha conversas duráveis e retome atividades de
longo prazo, especialmente estudos de inglês, sem depender da memória volátil
do processo atual.

O usuário deve poder criar uma conversa chamada, por exemplo, `Estudos de
inglês`, voltar a ela dias ou meses depois e continuar com:

- histórico completo das aulas;
- resumo acumulado do que já foi estudado;
- nível, objetivo e preferência de correção;
- vocabulário e erros recorrentes;
- atividade que ficou pendente;
- continuidade entre texto no dashboard e voz no robô.

O histórico pertence ao usuário. Ele deve ser legível fora do NoiseBot,
exportável, removível e armazenado localmente por padrão.

## 2. Decisão de Arquitetura

Usar uma arquitetura híbrida:

1. **SQLite é a fonte de verdade operacional.**
2. **Um vault privado do Obsidian é a projeção humana do conteúdo.**
3. **O firmware recebe somente a saída necessária para falar ou reagir.**

O Obsidian não será usado como banco transacional primário. Markdown puro é
excelente para leitura, edição e portabilidade, mas não oferece sozinho:

- transação entre mensagem, resposta, anexo e resumo;
- prevenção segura de duplicação em retries;
- ordenação concorrente entre dashboard e voz;
- migrações de schema;
- paginação eficiente;
- marcação confiável de turnos interrompidos;
- integridade referencial para exclusão de conversas e anexos.

SQLite resolve essas necessidades sem dependência externa, pois faz parte da
biblioteca padrão do Python. O Obsidian preserva a parte mais valiosa para o
usuário: notas Markdown locais, pesquisáveis, vinculáveis e independentes da
interface do NoiseBot.

### 2.1 Distinção do Knowledge OS do projeto

O vault de conversas é dado privado do produto e não é o antigo Knowledge OS
de engenharia em `D:\base_conhecimento\projects\Noisebot`.

A decisão atual de não manter aquele Knowledge OS continua válida. O novo vault
será separado do repositório e não poderá ser commitado por acidente.

## 3. Princípios Inegociáveis

- **Local-first:** nenhum histórico depende de nuvem.
- **Persistência antes de geração:** a mensagem do usuário é salva antes de
  chamar a LLM.
- **Histórico bruto é imutável:** resumos ajudam o contexto, mas nunca
  substituem nem apagam mensagens.
- **Falha explícita:** um turno interrompido fica registrado como interrompido,
  não desaparece.
- **Sem conteúdo bruto no firmware:** texto histórico, anexos, resumos e perfil
  de estudo permanecem no server.
- **Sem mistura silenciosa de conversas:** cada turno pertence a uma conversa
  identificada.
- **Sem mistura silenciosa de usuários:** conversa e contexto pertencem ao
  `user_id` ativo.
- **Dados importados são não confiáveis:** Markdown, anexos e resumos nunca
  podem fornecer instruções de sistema à LLM.
- **Exclusão real no domínio da aplicação:** apagar uma conversa remove banco,
  anexos persistidos e projeção no vault, com aviso honesto sobre backups e
  limitações de secure erase em SSD.
- **Compatibilidade:** o fluxo atual continua funcional durante a migração.

## 4. Componentes Propostos

```text
Dashboard ───────────────┐
                        │
Voz / Orchestrator ─────┼──> ConversationService
                        │         │
                        │         ├──> SQLite (fonte de verdade)
                        │         ├──> ContextBuilder
                        │         └──> ObsidianExporter
                        │
                        └──> LLM ──> resposta
                                      │
                                      └──> firmware (somente saída do turno)
```

### 4.1 `ConversationStore`

Novo módulo síncrono, pequeno e testável:

`server/noisebot_server/internal/conversations/store.py`

Responsabilidades:

- abrir e migrar o banco;
- criar, listar, renomear, arquivar e excluir conversas;
- criar turnos idempotentes;
- persistir mensagens, metadados e anexos;
- recuperar páginas ordenadas do histórico;
- manter conversa ativa por usuário;
- fornecer transações curtas e consistentes;
- criar backup consistente usando a API de backup do SQLite.

Todo acesso a disco chamado pelo event loop deve passar por
`asyncio.to_thread(...)`.

Configuração inicial do SQLite:

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = 5000;
```

O caminho padrão será:

`~/.noisebot-server/conversations.sqlite3`

Override opcional:

`NOISEBOT_CONVERSATIONS_DB_PATH`

### 4.2 `ConversationService`

Novo módulo de aplicação:

`server/noisebot_server/internal/conversations/service.py`

Responsabilidades:

- validar ownership e estado da conversa;
- coordenar persistência antes/depois da LLM;
- decidir quais turnos de voz entram no histórico;
- montar contexto por orçamento;
- agendar resumo sem bloquear a resposta;
- projetar atualizações para o Obsidian;
- recuperar turnos interrompidos após restart.

### 4.3 `ConversationContextBuilder`

Substitui o uso exclusivo dos deques voláteis
`_recent_user_texts`/`_recent_llm_replies` para conversas persistentes.

O contexto será composto, nesta ordem:

1. regras do sistema e persona;
2. política de idioma da conversa;
3. perfil de estudo estruturado;
4. resumo acumulado até uma sequência conhecida;
5. mensagens recentes posteriores ao resumo;
6. mensagem atual;
7. contexto do anexo atual;
8. resultado de tool, quando houver.

Cada bloco terá orçamento próprio. O histórico não poderá consumir a reserva de
saída da LLM.

### 4.4 `ObsidianExporter`

Novo módulo:

`server/noisebot_server/internal/conversations/obsidian.py`

Primeiro corte: exportação unidirecional SQLite → Markdown.

Não haverá importação automática de notas editadas pelo usuário. Isso evita
conflitos e impede que texto arbitrário de um vault vire instrução oculta para
o agente.

O vault padrão ficará fora do repositório:

`~/.noisebot-server/obsidian-vault`

Override opcional:

`NOISEBOT_OBSIDIAN_VAULT_PATH`

O usuário poderá abrir essa pasta diretamente no Obsidian. Nenhum plugin será
obrigatório.

## 5. Modelo de Dados

Todas as datas serão UTC em ISO 8601 no contrato HTTP e Unix time interno ou
texto UTC no banco. IDs públicos serão UUID v4; `turn_id` atual continuará
existindo como correlação de runtime, não como chave primária permanente.

### 5.1 `schema_migrations`

| Campo | Uso |
| --- | --- |
| `version` | versão inteira única |
| `applied_at` | data UTC da migração |

Migrações serão monotônicas, transacionais e testadas contra banco vazio e
banco da versão anterior.

### 5.2 `conversations`

| Campo | Uso |
| --- | --- |
| `id` | UUID estável |
| `user_id` | dono local da conversa |
| `title` | título editável |
| `kind` | `general`, `study` ou futuro tipo explícito |
| `status` | `active` ou `archived` |
| `language_policy` | `auto`, `pt-BR`, `en-US` ou `bilingual` |
| `response_mode` | padrão `dashboard` ou `robot` |
| `created_at` | criação |
| `updated_at` | última alteração |
| `last_message_at` | ordenação da lista |
| `metadata_json` | opções versionadas e limitadas |

Não usar `metadata_json` para campos essenciais de consulta.

### 5.3 `turns`

| Campo | Uso |
| --- | --- |
| `id` | UUID do turno persistente |
| `conversation_id` | FK |
| `runtime_turn_id` | correlação com event bus/telemetria |
| `client_request_id` | chave idempotente do dashboard |
| `origin` | `dashboard` ou `voice` |
| `response_mode` | modo usado no turno |
| `route` | `llm`, `local_intent`, `dashboard`, etc. |
| `status` | `pending`, `complete`, `failed`, `interrupted` |
| `error_code` | código estável, sem segredo |
| `created_at` | início |
| `completed_at` | conclusão |

`client_request_id` terá índice único por conversa. Repetir uma requisição após
timeout retorna o mesmo turno em vez de duplicar a mensagem.

### 5.4 `messages`

| Campo | Uso |
| --- | --- |
| `id` | UUID |
| `conversation_id` | FK |
| `turn_id` | FK |
| `sequence` | ordem monotônica dentro da conversa |
| `role` | `user`, `assistant`, `tool` |
| `content` | texto integral |
| `language` | idioma detectado/declarado |
| `created_at` | data UTC |
| `metadata_json` | fontes, métricas e campos de apresentação |

Constraint única: `(conversation_id, sequence)`.

Mensagens não serão reescritas para “corrigir” resumos. Edição futura deverá
criar revisão explícita ou evento de edição.

### 5.5 `attachments`

| Campo | Uso |
| --- | --- |
| `id` | UUID |
| `message_id` | FK |
| `name` | nome sanitizado para exibição |
| `media_type` | tipo detectado por conteúdo |
| `size_bytes` | tamanho |
| `sha256` | integridade/deduplicação |
| `retention` | `ephemeral` ou `persistent` |
| `relative_path` | caminho interno aleatório, nunca nome fornecido |
| `created_at` | criação |

Política inicial:

- mensagens e respostas são permanentes;
- anexos continuam efêmeros por padrão;
- o usuário pode optar por “guardar anexo nesta conversa”;
- anexo persistente usa diretório privado, nome aleatório e validação pelo
  conteúdo;
- nenhum caminho absoluto é devolvido pela API;
- HTML e executáveis não entram no escopo.

### 5.6 `conversation_summaries`

| Campo | Uso |
| --- | --- |
| `conversation_id` | FK |
| `through_sequence` | última mensagem coberta |
| `summary_text` | resumo regenerável |
| `summary_version` | versão do formato |
| `model` | modelo que produziu o resumo |
| `source_hash` | detecta resumo obsoleto |
| `updated_at` | atualização |

O resumo é cache derivado. Se ele faltar ou estiver inválido, o histórico bruto
continua disponível.

### 5.7 `study_profiles`

Um registro opcional por conversa do tipo `study`.

| Campo | Uso |
| --- | --- |
| `conversation_id` | FK |
| `subject` | exemplo: `English` |
| `target_language` | `en-US`, `en-GB`, etc. |
| `current_level` | autodeclarado ou confirmado: A1–C2 |
| `goal` | objetivo do usuário |
| `correction_style` | imediato, ao final, somente erros importantes |
| `session_style` | conversa, gramática, leitura, revisão |
| `next_activity` | ponto seguro de retomada |
| `updated_at` | atualização |

O nível nunca deve subir automaticamente com base em uma única resposta. A LLM
pode sugerir mudança, mas a aplicação registra como observação até confirmação.

### 5.8 `learning_items`

Entrará depois do histórico básico estar estável.

Tipos iniciais:

- `vocabulary`;
- `grammar`;
- `pronunciation`;
- `recurring_mistake`;
- `topic`;
- `goal`.

Cada item terá estado `observed`, `learning`, `review` ou `mastered`, origem em
uma mensagem e possibilidade de edição/exclusão pelo usuário.

## 6. Fluxo Transacional de um Turno

### 6.1 Dashboard

1. Dashboard envia `conversation_id`, `client_request_id`, texto, modo e anexo.
2. Server valida token, usuário, conversa, limites e conteúdo.
3. Em uma transação:
   - cria `turn` como `pending`;
   - grava mensagem do usuário;
   - grava metadados do anexo.
4. Commit acontece antes da chamada à LLM.
5. Server monta contexto da conversa.
6. LLM responde.
7. Em uma transação:
   - grava mensagem do assistente;
   - marca turno `complete`;
   - atualiza `last_message_at`.
8. Exportação para Obsidian é tentada após o commit.
9. Falha do Obsidian não desfaz o turno; fica pendente para retry.

Se a LLM falhar, o turno vira `failed` e a mensagem do usuário permanece.

### 6.2 Voz

Primeiro corte:

- somente turnos conversacionais completos entram na conversa ativa;
- wake vazio, áudio rejeitado, barge-in sem conteúdo e comandos técnicos não
  poluem o histórico de estudo;
- intents locais podem aparecer em telemetria, mas não entram como aula;
- a conversa ativa por voz expira após janela configurável;
- o dashboard mostra claramente qual conversa receberá a próxima fala;
- sem conversa ativa, voz usa `Conversa geral`.

Não é necessário enviar `conversation_id` ao firmware. O server associa o
turno usando o usuário reconhecido e a conversa ativa.

### 6.3 Recuperação após crash

No startup:

- turnos `pending` mais antigos que o limite são marcados `interrupted`;
- mensagens do usuário continuam visíveis;
- o dashboard oferece “tentar novamente”;
- retry usa novo turno ligado ao anterior, sem apagar a tentativa antiga.

## 7. Continuidade da LLM

### 7.1 Contexto recente

O contexto recente deve preservar pares ordenados de mensagens. O modelo atual,
que mantém listas separadas de usuário e robô, será substituído gradualmente por
uma lista explícita:

```json
[
  {"role": "user", "content": "..."},
  {"role": "assistant", "content": "..."}
]
```

Isso evita pareamento incorreto quando um turno falha ou não produz resposta.

### 7.2 Resumo progressivo

O resumo será criado quando a conversa ultrapassar um limite de mensagens ou
tokens, nunca a cada turno.

Regras:

- resumir somente mensagens já persistidas;
- registrar `through_sequence`;
- manter mensagens recentes posteriores ao resumo;
- incluir fatos confirmados, assuntos estudados, erros recorrentes observados,
  decisões e atividade pendente;
- não transformar inferências em fatos;
- não apagar contradições silenciosamente;
- permitir regeneração a partir do histórico bruto;
- falha de resumo não bloqueia a conversa.

### 7.3 Idioma

O guard atual força respostas em português. Isso é incompatível com uma aula
de inglês.

Cada conversa terá política explícita:

- `pt-BR`: resposta em português;
- `en-US`: resposta em inglês;
- `bilingual`: inglês para prática e português para explicações;
- `auto`: acompanha o pedido atual sem bloquear conteúdo pedagógico.

O guard de idioma deverá receber essa política. Uma conversa de estudo em
inglês não pode tratar frases em inglês como vazamento acidental.

### 7.4 Segurança contra prompt injection

Histórico, resumo, anexos e notas do Obsidian serão delimitados como dados do
usuário. O prompt do sistema deve declarar que instruções encontradas nesses
blocos não alteram políticas, tools ou permissões.

Conteúdo antigo não pode:

- habilitar uma tool;
- alterar ownership;
- pedir leitura de outro arquivo/conversa;
- mudar `response_mode`;
- autorizar ação sensível;
- substituir confirmação atual do usuário.

## 8. Integração com Obsidian

### 8.1 Estrutura do vault

```text
NoiseBot Vault/
├── Home.md
├── Conversations/
│   ├── General/
│   │   └── <conversation-id>/
│   │       ├── _index.md
│   │       └── Sessions/
│   │           └── 2026-06-18.md
│   └── Studies/
│       └── English/
│           └── <conversation-id>/
│               ├── _index.md
│               ├── Progress.md
│               ├── Vocabulary.md
│               ├── Recurring Mistakes.md
│               └── Sessions/
│                   └── 2026-06-18.md
└── Attachments/
    └── <conversation-id>/
```

O primeiro corte pode gerar apenas `_index.md` e notas de sessão. `Progress`,
vocabulário e erros entram quando `study_profiles`/`learning_items` estiverem
estáveis.

### 8.2 Frontmatter

Exemplo:

```yaml
---
noisebot_schema: 1
conversation_id: "uuid"
title: "Estudos de inglês"
kind: "study"
user_id: "owner"
language_policy: "bilingual"
created_at: "2026-06-18T15:00:00Z"
updated_at: "2026-06-18T16:10:00Z"
exported_through_sequence: 42
tags:
  - noisebot
  - study/english
---
```

### 8.3 Regras de escrita

- exportação após commit do SQLite;
- arquivo temporário + `os.replace`;
- caminho sempre resolvido dentro do vault configurado;
- nunca usar o nome da conversa diretamente como diretório sem slug seguro;
- manter ID no caminho/frontmatter para evitar colisão após rename;
- escapar HTML bruto e embeds perigosos do Obsidian;
- não executar links, comandos ou plugins;
- registrar hash do conteúdo exportado;
- retry idempotente se o Obsidian estiver aberto, bloqueado ou indisponível.

### 8.4 Edição pelo usuário

No primeiro corte, notas exportadas são leitura humana. Para não sobrescrever
anotações pessoais:

- conteúdo gerado fica em arquivos/seções identificados;
- `Minhas notas` fica em arquivo ou seção preservada;
- o exportador não reinterpreta conversa editada;
- importação futura será explícita, com preview e confirmação.

### 8.5 Nuvem

O NoiseBot não habilita sync. Se o usuário colocar o vault no Obsidian Sync,
OneDrive, Dropbox ou Git, a cópia deixa de ser estritamente local por decisão
externa ao NoiseBot. O dashboard deve mostrar essa responsabilidade de forma
clara quando um caminho sincronizado for detectável.

## 9. API Local

Todos os endpoints de conversa, inclusive leitura, exigem token. O conteúdo é
mais sensível que telemetria de operação.

### 9.1 Conversas

```text
GET    /api/conversations
POST   /api/conversations
GET    /api/conversations/{conversation_id}
PATCH  /api/conversations/{conversation_id}
DELETE /api/conversations/{conversation_id}
GET    /api/conversations/{conversation_id}/messages
POST   /api/conversations/{conversation_id}/activate
GET    /api/conversations/active
```

Regras:

- paginação por cursor/sequence, não offset em históricos grandes;
- limites máximos de página;
- `DELETE` exige confirmação no dashboard;
- arquivar usa `PATCH`, não exclusão;
- nenhuma resposta expõe caminho local absoluto;
- erros usam códigos estáveis além da mensagem humana.

### 9.2 Interação

`POST /api/interactions` será ampliado com:

```text
conversation_id
client_request_id
persist_attachment
```

Durante compatibilidade:

- ausência de `conversation_id` usa/cria `Conversa geral`;
- clientes antigos continuam funcionando;
- a resposta inclui `conversation_id`, `turn_id`, `turn_uuid`,
  `user_message_id` e, quando pronta, `assistant_message_id`.

## 10. Dashboard

### 10.1 Layout

Tela de interação em desktop:

- coluna lateral de conversas;
- busca local por título;
- botão `Nova conversa`;
- conversa ativa destacada;
- título, tipo e idioma no cabeçalho;
- feed paginado;
- composer atual preservado;
- indicador `Esta conversa também recebe voz`;
- menu para renomear, arquivar e excluir.

Em telas pequenas, a coluna vira drawer.

### 10.2 Criação de conversa

Campos mínimos:

- título;
- tipo: geral ou estudo;
- política de idioma;
- resposta padrão: dashboard ou robô.

Para estudo:

- matéria/idioma;
- nível inicial opcional;
- objetivo;
- estilo de correção.

### 10.3 Estados e feedback

- criação/salvamento otimista apenas para metadados reversíveis;
- mensagem aparece como `enviando` após persistência confirmada;
- erro de LLM não remove mensagem;
- turno interrompido pode ser reenviado;
- histórico carrega páginas anteriores sob demanda;
- status do Obsidian aparece separado do status da conversa;
- falha de exportação nunca aparece como perda do chat.

## 11. Privacidade, Integridade e Backup

### 11.1 Fronteiras

- bind continua em `127.0.0.1`;
- bearer token obrigatório para leitura e escrita;
- sem segredos em SQLite, Markdown, logs ou respostas HTTP;
- logs registram IDs, tamanhos e códigos, não conteúdo integral;
- anexos são validados por conteúdo;
- nomes e paths são sanitizados;
- symlinks e traversal para fora do vault são rejeitados.

### 11.2 Backup

Backup não será feito copiando o `.sqlite3` aberto diretamente.

Usar:

- API `sqlite3.Connection.backup`;
- arquivo temporário + rename;
- manifesto com versão, data e hash;
- comando manual de backup/restore no primeiro corte;
- automação periódica somente após teste de restauração.

O vault pode ser copiado normalmente quando nenhuma exportação estiver em
andamento, mas o SQLite continua sendo a referência para restore completo.

### 11.3 Exclusão

Ao excluir:

1. confirmar título da conversa;
2. remover registros em transação;
3. remover anexos persistentes correspondentes;
4. remover ou tombstonar arquivos exportados;
5. informar se backups ainda contêm cópia;
6. permitir purga separada dos backups.

Secure erase físico não pode ser garantido em SSD. Para proteção forte em
repouso, usar criptografia do disco do sistema; SQLCipher pode ser avaliado
depois, pois adiciona dependência operacional.

## 12. Migração e Compatibilidade

Estado atual:

- últimos 12 turnos vivem apenas no `StatusStore`;
- contexto recente vive em dois deques de seis itens;
- anexos expiram após 30 minutos;
- não há histórico recuperável após restart.

Migração:

1. criar banco e `Conversa geral`;
2. manter endpoints e telemetria atuais;
3. gravar novos turnos simultaneamente no histórico persistente;
4. dashboard passa a ler a nova API;
5. deques continuam como fallback temporário;
6. depois da validação, contexto usa `ConversationContextBuilder`;
7. remover dependência dos deques somente em commit separado.

Os turnos antigos já perdidos não podem ser reconstruídos. Os turnos ainda
presentes no `StatusStore` no momento da migração podem ser importados uma única
vez, marcados como `legacy_import`, sem inventar timestamps ou anexos.

## 13. Fases de Implementação

### Fase C0 — Contratos e banco

Estado em 2026-06-18: **fundação implementada, wiring ainda pendente**.

Implementado:

- schema SQLite v1 e migrations monotônicas;
- `ConversationStore` em
  `server/noisebot_server/internal/conversations/store.py`;
- CRUD e ownership local por usuário;
- conversa ativa por usuário;
- criação atômica de turno + mensagem do usuário;
- idempotência por `client_request_id`;
- conclusão/falha sem perder a pergunta;
- paginação por `sequence`;
- recuperação de turnos pendentes após restart;
- cascata de exclusão e rejeição de schema mais novo;
- testes focados em `server/tests/test_conversation_store.py`.

Ainda não implementado neste corte:

- composição no `NoiseBotServer`;
- endpoints HTTP;
- dashboard;
- contexto da LLM;
- Obsidian e anexos persistentes.

Entregas:

- schema v1;
- migrations;
- `ConversationStore`;
- testes de CRUD, ordenação, FK, idempotência e recuperação;
- configuração de paths;
- nenhum uso pela LLM ainda.

Gate:

- banco sobrevive a restart;
- retry não duplica turno;
- migração pode rodar duas vezes sem dano;
- corrupção/versão incompatível falha alto e preserva o arquivo original.

### Fase C1 — Histórico persistente no dashboard

Entregas:

- endpoints autenticados;
- lista/nova conversa/rename/archive/delete;
- mensagens paginadas;
- `POST /api/interactions` associado à conversa;
- mensagens de dashboard persistidas;
- compatibilidade com cliente antigo.

Gate:

- criar conversa, trocar, reiniciar server e continuar vendo tudo;
- falha da LLM mantém a pergunta;
- exclusão não deixa registros órfãos;
- build do app e testes do server aprovados.

### Fase C2 — Continuidade real da LLM

Entregas:

- `ConversationContextBuilder`;
- política de idioma;
- contexto recente ordenado;
- resumo progressivo;
- orçamento de tokens;
- fallback se resumo falhar.

Gate:

- “vamos continuar” recupera assunto e atividade pendente;
- conversa A nunca recebe contexto da B;
- estudo em inglês não é bloqueado pelo guard pt-BR;
- conversa longa mantém espaço de resposta;
- nenhuma tool é autorizada por texto histórico.

### Fase C3 — Voz na conversa ativa

Entregas:

- conversa ativa por usuário;
- indicador no dashboard;
- persistência de turnos conversacionais de voz;
- filtro de intents técnicos/turnos rejeitados;
- expiração segura da seleção.
- integração com a política de idioma e com a transcrição corrigível definida
  em `docs/BILINGUAL_VOICE_STT_PLAN.md`.

Gate:

- aula começa no dashboard e continua por voz;
- comando “volume 50%” não vira conteúdo pedagógico;
- reconhecimento de outro usuário não vaza conversa;
- firmware continua recebendo apenas saída do turno.
- turno incerto do Whisper não atualiza progresso até confirmação/correção.

### Fase C4 — Exportação Obsidian

Entregas:

- vault privado configurável;
- `_index.md` e notas de sessão;
- export idempotente;
- retry e status de sincronização;
- sanitização Markdown/paths;
- backup/restore documentado.

Gate:

- conversa aparece no Obsidian após restart;
- rename não cria conversa duplicada;
- Obsidian indisponível não quebra o chat;
- nenhuma escrita sai do vault configurado;
- notas pessoais preservadas.

### Fase C5 — Progresso de estudos

Entregas:

- `study_profiles`;
- atividade pendente;
- vocabulário e erros recorrentes como itens revisáveis;
- tela de progresso;
- notas `Progress.md`, `Vocabulary.md` e `Recurring Mistakes.md`.

Gate:

- progresso é baseado em evidência ligada a mensagens;
- mudança de nível exige confirmação;
- usuário pode corrigir e excluir itens;
- “o que estudamos?” e “onde paramos?” têm resposta rastreável.

### Fase C6 — Backup, exportação e endurecimento

Entregas:

- backup consistente;
- restauração validada;
- exportação portátil;
- purga de backups;
- limites de crescimento e observabilidade;
- teste de carga com histórico grande.

Gate:

- restore reproduz conversas, resumos e metadados;
- 10 mil mensagens continuam pagináveis;
- falhas de disco têm erro explícito;
- documentação operacional concluída.

## 14. Estratégia de Commits e Rollback

Commits pequenos:

1. docs e contratos;
2. schema/store;
3. API;
4. persistência do dashboard;
5. UI;
6. contexto LLM;
7. voz;
8. Obsidian;
9. progresso de estudos;
10. backup/hardening.

Cada fase deve manter o fluxo anterior utilizável. Não misturar migração de
banco, redesign completo do dashboard e mudança de prompt no mesmo commit.

Rollback:

- schema nunca é apagado automaticamente;
- recurso protegido por flag até C2 estabilizar;
- dashboard antigo continua usando `Conversa geral`;
- exportador Obsidian pode ser desligado sem afetar SQLite;
- resumos podem ser ignorados e regenerados;
- voz pode voltar a não persistir sem perder os chats do dashboard.

Flags sugeridas:

```text
NOISEBOT_CONVERSATIONS_ENABLED=0|1
NOISEBOT_CONVERSATION_VOICE_ENABLED=0|1
NOISEBOT_OBSIDIAN_EXPORT_ENABLED=0|1
```

## 15. Matriz de Testes

### Unitários

- migrations e constraints;
- CRUD e paginação;
- idempotência;
- sequence monotônica;
- ownership;
- sanitização de paths/Markdown;
- orçamento de contexto;
- resumo e hash;
- política de idioma;
- classificação de turno persistível.

### Integração

- HTTP com token válido/inválido;
- interação dashboard completa;
- falha e retry;
- restart entre pergunta e resposta;
- troca de conversa;
- voz associada à conversa ativa;
- anexo efêmero/persistente;
- exportação Obsidian bloqueada;
- backup e restore.

### Segurança

- path traversal;
- symlink para fora do vault;
- nomes maliciosos;
- HTML/embed do Obsidian;
- prompt injection no histórico;
- tentativa de acessar conversa de outro usuário;
- retry replay;
- payload e páginas acima do limite;
- logs sem conteúdo sensível.

### Produto

Cenário obrigatório:

1. criar `Estudos de inglês`;
2. definir nível A2 e objetivo conversação;
3. praticar `present perfect`;
4. registrar duas correções;
5. reiniciar server e dashboard;
6. reabrir a conversa;
7. perguntar “onde paramos?”;
8. continuar por voz;
9. abrir a sessão no Obsidian;
10. confirmar que outra conversa não recebeu esse contexto.

## 16. Fora do Primeiro Corte

- sincronização cloud automática;
- edição bidirecional livre entre Obsidian e SQLite;
- colaboração multiusuário remota;
- embeddings/RAG sobre todo o histórico;
- avaliação automática definitiva de nível;
- repetição espaçada completa;
- persistência obrigatória de anexos;
- criptografia própria substituindo a proteção do sistema operacional.

Esses itens só entram depois que histórico, restore, isolamento e privacidade
estiverem comprovados.

## 17. Critério Global de Conclusão

A capacidade só será considerada pronta quando:

- o histórico sobreviver a restart;
- a LLM retomar corretamente uma conversa selecionada;
- voz e dashboard compartilharem contexto sem misturar conversas;
- estudo em inglês funcionar com política de idioma apropriada;
- transcrição incerta permanecer revisável e não contaminar a memória;
- o robô conseguir ouvir e falar inglês por configuração explícita;
- Obsidian receber uma projeção legível sem ser ponto único de falha;
- exclusão, backup e restore forem testados;
- anexos e conteúdo bruto nunca atravessarem o bridge;
- o usuário conseguir identificar e controlar tudo que foi guardado.
