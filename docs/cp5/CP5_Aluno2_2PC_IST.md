# Checkpoint 5 — Aluno 2: Two-Phase Commit (2PC) e Incremental State Transfer (IST)

---

## Parte 1 — Visão Geral do Checkpoint 5 (Comum aos Dois Alunos)

### 1.1 O que o Checkpoint 5 adiciona ao sistema

Nos checkpoints anteriores, a rede P2P híbrida já possuía:
- **Checkpoint 1 e 2:** Identidade criptográfica (`NodeID = SHA256(IP || Porta || UUID)`), protocolo binário de cabeçalho fixo (91 bytes), fragmentação em chunks de 4 MB, compressão LZ4 e indexação de metadados (`STORE` / `LOOKUP`).
- **Checkpoint 3:** Overlay DHT **Chord** ($m = 256$ bits) entre Super Peers para roteamento $O(\log N)$ e protocolo **Gossip + Heartbeat** para membership e detecção distribuída de falhas (`ALIVE` $\to$ `SUSPECT` $\to$ `DEAD`).
- **Checkpoint 4:** Eleição de coordenador pelo algoritmo **Bully** e replicação de metadados por **Log Replicado / State Machine Replication (SMR)** em ordem total.

O **Checkpoint 5 (Seção 16.10 da especificação)** conclui a arquitetura distribuída adicionando quatro mecanismos essenciais divididos igualmente entre os dois integrantes da dupla:

1. **Cache LFU (Least Frequently Used)** *(Aluno 1)* — Armazena em memória nos Super Peers os metadados mais consultados, contabilizando `frequency` (frequência de acesso) e `last_access` (relógio lógico de acesso para desempate LRU), registrando `CACHE HIT`, `CACHE MISS`, `CACHE INSERT` e `CACHE EVICT`.
2. **Transferência Paralela de Chunks em Rede** *(Aluno 1)* — Durante o `download`, após descobrir o metadado e os provedores via `LOOKUP`, o Peer dispara múltiplas threads (`pthread_create`, uma por chunk) que requisitam os blocos comprimidos em paralelo pela rede (`MSG_DOWNLOAD_REQ` $\to$ `MSG_DOWNLOAD_REP`), verificam o hash **SHA-256** de cada chunk comprimido, descomprimem via **LZ4** e reconstroem o arquivo validando o `ObjectID` global.
3. **Two-Phase Commit (2PC)** *(Aluno 2)* — Substitui a replicação direta por um protocolo de commit atômico em duas fases coordenadas pelo líder Bully: **Fase 1 (`MSG_PREPARE`)** coleta votos `MSG_VOTE_YES` (`YES`) ou `MSG_VOTE_NO` (`NO`) de todos os Super Peers vivos; **Fase 2 (`MSG_COMMIT` ou `MSG_ABORT`)** efetiva a operação somente se houver unanimidade (`YES`), ou descarta a transação preparada em caso de qualquer voto `NO` ou falha durante a preparação.
4. **Incremental State Transfer (IST)** *(Aluno 2)* — Quando um Super Peer retorna após uma queda ou entra tardiamente no grupo, ele consulta o coordenador enviando sua versão local (`smr_last_applied()`) via `MSG_STATE_TRANSFER`. O coordenador calcula a diferença e envia **apenas as entradas de log faltantes** (`local_ver + 1 .. coord_ver`), evitando transferir todo o banco de metadados e permitindo que o nó retorne rapidamente ao estado `READY`.

---

### 1.2 Divisão de Responsabilidades na Dupla

| Integrante | Módulos sob Responsabilidade no CP5 | Arquivos Criados / Modificados |
| :--- | :--- | :--- |
| **Aluno 1** | **Cache LFU** (`hit`, `miss`, `frequency`, `eviction` LFU+LRU) + **Transferência Paralela de Chunks** (`MSG_DOWNLOAD_REQ` / `MSG_DOWNLOAD_REP`, threads por chunk, validação SHA-256 + LZ4) | [include/cache.h](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/cache.h), [cache.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c), [peer.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c), [superpeer.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c) (`tratar_lookup`, `tratar_download_req`), [metadata.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/metadata.c) |
| **Aluno 2** | **Two-Phase Commit (2PC)** (`PREPARE`, `YES`/`NO`, `COMMIT`, `ABORT`, tratamento de falha na fase `PREPARE`) + **Incremental State Transfer (IST)** (`MSG_STATE_TRANSFER`, sincronização de logs faltantes) | [include/smr.h](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/smr.h), [smr.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c), [include/protocol.h](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/protocol.h), [protocol.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/protocol.c), [bully.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/bully.c), [gossip.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/gossip.c), [superpeer.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c) (`tratar_2pc`, `tratar_state_transfer`) |

---

### 1.3 Arquitetura Integrada do Checkpoint 5

```
  Peer (Cliente)                        Coordenador (Bully)                 Followers (SP1, SP2, SP3)
       |                                         |                                      |
       |--- 1. MSG_STORE (metadado) ------------>|                                      |
       |                                         |--- 2. MSG_PREPARE (LogEntry) ------->| (Aluno 2: 2PC Fase 1)
       |                                         |<-- 3. MSG_VOTE_YES / MSG_VOTE_NO ----|
       |                                         |--- 4. MSG_COMMIT / MSG_ABORT ------->| (Aluno 2: 2PC Fase 2)
       |                                         |    [Aplica no SMR + cache_put()]     | [Aplica no SMR]
       |<-- 5. MSG_ACK --------------------------|                                      |
       |                                         |                                      |
       |--- 6. MSG_LOOKUP ("small.txt") ----------------------------------------------->| (Aluno 1: Cache LFU)
       |                                         |                                      | cache_get_by_name()
       |                                         |                                      | -> MISS: busca tabela + cache_put()
       |                                         |                                      | -> HIT: frequency++
       |<-- 7. MSG_ACK (FileMetadata + Provedores) -------------------------------------|
       |                                         |                                      |
       |=== 8. Transferencia Paralela (1 pthread por chunk) ===========================>| (Aluno 1: Rede + LZ4)
       |--- MSG_DOWNLOAD_REQ (hash + idx) ------>|------------------------------------->|
       |<-- MSG_DOWNLOAD_REP (chunk .lz4) -------|<-------------------------------------|
       |    [Verifica SHA-256 -> LZ4_decompress] |                                      |
       |                                         |                                      |
       |                                         |<-- 9. MSG_STATE_TRANSFER (ver=3) ----| (Aluno 2: SP2 reiniciado)
       |                                         |--- 10. MSG_ACK (logs #4..#5) ------->| [IST: SYNCHRONIZING -> READY]
```

---

## Parte 2 — Documentação Detalhada do Aluno 2 (Two-Phase Commit e IST)

### 2.1 Fundamentação Teórica

#### 2.1.1 Two-Phase Commit (2PC) — Seção 14 da Especificação
Para garantir atomicidade na atualização distribuída de metadados entre os Super Peers ativos, toda escrita (`PUT` originada por `MSG_STORE`) passa pelo protocolo **Two-Phase Commit (2PC)** conduzido pelo coordenador eleito pelo Bully:

- **Máquina de Estados da Transação (`TxState` em [`include/smr.h`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/smr.h#L21-L27)):**
  ```
  IDLE ──► PREPARING ──► READY ──► COMMIT (ou ABORT)
  ```
- **Fase 1 — Preparação (`MSG_PREPARE`):**
  1. O coordenador adquire o mutex de ordem total (`smr_order_mutex`), gera o próximo `log_index = smr_applied + 1`, incrementa o relógio lógico de Lamport (`++smr_clock`), constrói o `TransactionID` de 128 bits (`[C-02]`: 48 bits de timestamp em segundos + 64 bits superiores do `NodeID` + 16 bits de sequência) e calcula o `checksum` SHA-256 sobre cabeçalho + metadado + provedor.
  2. O coordenador envia `MSG_PREPARE` com o payload completo da entrada de log para todos os Super Peers `ALIVE` na tabela de membership do Gossip.
  3. Cada participante valida a origem (deve ser o coordenador atual), o `checksum` SHA-256, a integridade do metadado e a sequência (`e.log_index <= smr_applied + 1`). Se tudo estiver válido, armazena a transação em um buffer temporário (`PendingTx smr_pending`) no estado `TX_READY` **sem aplicar ainda na tabela definitiva** e responde `MSG_VOTE_YES` (`YES`). Caso qualquer verificação falhe (ou se a variável de ambiente `SIMULATE_VOTE_NO=1` estiver ativa para teste de falha), responde `MSG_VOTE_NO` (`NO`).
- **Fase 2 — Decisão (`MSG_COMMIT` ou `MSG_ABORT`):**
  - **Unanimidade (`todos_yes == 1`):** O coordenador transita `[READY -> COMMIT]`, aplica a entrada localmente no log SMR e na tabela de metadados, e envia `MSG_COMMIT` (carregando os 16 bytes do `TransactionID`) para todos os participantes. Cada participante verifica se possui a transação correspondente em `TX_READY`, aplica definitivamente no banco de metadados e limpa `smr_pending`.
  - **Qualquer voto `NO` ou falha de comunicação (`todos_yes == 0`):** O coordenador transita `[PREPARING -> ABORT]` e envia `MSG_ABORT(TransactionID)` a todos os participantes. Os participantes que haviam votado `YES` descartam a transação em `smr_pending` (`smr_clear_pending_locked()`), mantendo todos os nós no mesmo estado consistente anterior.

> **Nota Arquitetural `[C-18]` (Limitação de Bloqueio do 2PC):** Conforme documentado na Seção 14.3 da especificação, o 2PC clássico é um protocolo *bloqueante*: caso o coordenador falhe exatamente após os participantes entrarem no estado `TX_READY` e antes do envio de `MSG_COMMIT`/`MSG_ABORT`, os participantes aguardam a resolução ou reeleição. Na nossa implementação, caso uma nova preparação válida chegue de um coordenador legitimamente eleito, `smr_clear_pending_locked()` libera com segurança a estrutura pendente anterior, evitando vazamento de memória.

---

#### 2.1.2 Incremental State Transfer (IST) — Seção 13.3 da Especificação
Quando um Super Peer (como `SP2` no cenário do Checkpoint 5) sofre uma falha (`kill -9`) e é posteriormente reiniciado, ou quando fica temporariamente desconectado enquanto novas publicações (`D`, `E`) são confirmadas pelos nós restantes, seu estado de metadados fica defasado (`local_version < coordinator_version`).

Em vez de transferir um snapshot completo de todos os objetos, o **Incremental State Transfer (IST)** transfere apenas o delta de operações do log SMR:
1. Ao reconhecer o coordenador (em [`bully_handle_coordinator`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/bully.c#L279-L305) ou periodicamente em [`bully_tick`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/bully.c#L180-L212)), o Follower invoca [`smr_sync_with_coordinator(&coord)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L656-L756).
2. O Follower envia `MSG_STATE_TRANSFER` contendo sua versão atual (`uint64_t local_ver = smr_last_applied()`).
3. O coordenador executa [`smr_handle_state_transfer`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L597-L653): percorre seu vetor `smr_log` em memória e seleciona **apenas as entradas com `log_index > from_version`**, empacotando no payload de resposta:
   ```
   [coord_ver : uint64_t] [count : uint32_t] ( [entry_len : uint32_t] [raw_payload : entry_len bytes] ) x count
   ```
4. Ao receber a resposta com `count > 0`, o nó solicitante entra no fluxo de sincronização exigido na Seção 13.3:
   - Imprime `IST: SYNCHRONIZING (local version=X, coordinator version=Y, versions missing=Count)`
   - Imprime `IST: receive logs (X+1 .. Y) de <Coord>`
   - Valida o checksum `SHA-256` de cada entrada recebida e aplica em ordem estrita (`smr_append_locked`).
   - Imprime `IST: READY (sincronizado ate LOG #Y)`.

---

### 2.2 Arquivos Modificados pelo Aluno 2

1. **[include/protocol.h](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/protocol.h) e [protocol.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/protocol.c)**:
   - Adicionados os tipos de mensagem de votação do 2PC: `MSG_VOTE_YES = 27` (`"YES"`) e `MSG_VOTE_NO = 28` (`"NO"`), além do mapeamento textual em [`nome_do_tipo`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/protocol.c#L12-L53) para `PREPARE`, `COMMIT`, `ABORT`, `STATE_TRANSFER`, `DOWNLOAD_REQ`, `DOWNLOAD_REP`, `YES` e `NO`.
2. **[include/smr.h](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/smr.h)**:
   - Define o enum `TxState` (`TX_IDLE`, `TX_PREPARING`, `TX_READY`, `TX_COMMIT`, `TX_ABORT`) e declara as funções do 2PC (`smr_handle_prepare`, `smr_handle_commit`, `smr_handle_abort`) e do IST (`smr_handle_state_transfer`, `smr_sync_with_coordinator`).
3. **[smr.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c)**:
   - Implementa a estrutura `SmrStoredLog` (que preserva o `raw_payload` serializado de cada entrada confirmada para permitir envio rápido no IST) e a estrutura `PendingTx` (estado `TX_READY` do participante no 2PC).
   - Implementa o fluxo completo de 2PC no coordenador ([`smr_put`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L296-L401)) e nos participantes ([`smr_handle_prepare`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L478-L532), [`smr_handle_commit`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L534-L565), [`smr_handle_abort`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L567-L594)).
   - Implementa o servidor e o cliente do IST ([`smr_handle_state_transfer`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L597-L653) e [`smr_sync_with_coordinator`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L656-L756)).
4. **[bully.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/bully.c)**:
   - Aciona [`smr_sync_with_coordinator(&no)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/bully.c#L302) imediatamente quando um nó recebe `MSG_COORDINATOR` e detecta mudança/definição de líder, e também periodicamente em [`bully_tick`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/bully.c#L209) para recuperar eventuais logs perdidos.
5. **[gossip.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/gossip.c)**:
   - Corrige a reintegração de nós reiniciados ([`gossip_handle_heartbeat`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/gossip.c#L392-L418) e [`gossip_table_merge`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/gossip.c#L171-L202)): quando um Super Peer (`SP2`) sofre `kill -9` e reinicia do zero, seu contador `heartbeat` volta para `1`. Ao receber uma mensagem direta desse nó com contador menor que o histórico antigo, os demais Super Peers reiniciam o contador armazenado de `SP2` e o marcam imediatamente como `GOSSIP_ALIVE`, permitindo que ele volte a participar do 2PC e do IST sem atraso.
6. **[superpeer.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c)**:
   - Adiciona os handlers [`tratar_2pc`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L623-L655) e [`tratar_state_transfer`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L658-L671) no dispatcher [`atender_conexao`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L735-L798).

---

### 2.3 Explicação Detalhada Função por Função (Aluno 2)

#### 2.3.1 Two-Phase Commit em `smr.c`

- **[`smr_put(const FileMetadata *meta, const uint8_t provider[NODE_ID_LEN])`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L296-L401)**
  - Executada apenas pelo **Coordenador**.
  - Trava `smr_order_mutex` do início ao fim da transação 2PC para garantir que duas transações concorrentes recebam índices `log_index` estritamente sequenciais e não entrelacem suas fases `PREPARE` e `COMMIT`.
  - **Fase 1 (`PREPARE`):** Obtém a lista de membros `ALIVE` via `gossip_alive_members`, imprime `2PC [IDLE -> PREPARING] LOG #k TransactionID=...` e envia `MSG_PREPARE` a cada participante via `smr_send`.
  - Suporta simulação explícita de falha na preparação através da variável de ambiente `SIMULATE_PREPARE_FAIL=1` (no coordenador) ou `SIMULATE_VOTE_NO=1` (no participante).
  - **Fase 2 (`COMMIT` / `ABORT`):**
    - Se `todos_yes == 1`, imprime `2PC [READY -> COMMIT] LOG #k confirmado`, aplica localmente com `smr_apply_locked` e envia `MSG_COMMIT` com os 16 bytes do `TransactionID` a todos os participantes, finalizando com `2PC [COMMIT -> DONE] LOG #k`.
    - Se qualquer participante votou `MSG_VOTE_NO` ou falhou na conexão (`todos_yes == 0`), imprime `2PC [PREPARING -> ABORT] LOG #k abortado`, envia `MSG_ABORT` a todos os participantes e retorna `-1` sem alterar o estado.

- **[`smr_handle_prepare(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload, uint32_t tamanho)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L478-L532)**
  - Executada pelo **Participante** ao receber `MSG_PREPARE`.
  - Se `getenv("SIMULATE_VOTE_NO")` for `"1"`, recusa imediatamente com `MSG_VOTE_NO` e imprime `2PC PREPARE recusado: voto NO simulado (ABORT)`.
  - Chama `smr_validate_entry` para conferir se `origem` é o coordenador atual, se o `checksum` SHA-256 confere e se o `FileMetadata` deserializa perfeitamente.
  - Verifica se `e.log_index <= smr_applied + 1`. Se houver lacuna de versão, vota `MSG_VOTE_NO`.
  - Salva os dados validados em `smr_pending` com `smr_pending.state = TX_READY`, imprime `2PC PREPARE #k -> YES (estado READY)` e retorna `MSG_VOTE_YES`.

- **[`smr_handle_commit(...)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L534-L565)** e **[`smr_handle_abort(...)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L567-L594)**
  - Em `smr_handle_commit`, confere se `smr_pending.state == TX_READY` e se o `TransactionID` de 16 bytes recebido no payload coincide com `smr_pending.entry.transaction_id`. Em caso positivo, chama `smr_append_locked` para gravar o metadado na tabela e no log SMR, e limpa `smr_pending`.
  - Em `smr_handle_abort`, descarta imediatamente o buffer preparado em `smr_pending` via `smr_clear_pending_locked()` e retorna o participante ao estado `TX_IDLE`.

#### 2.3.2 Incremental State Transfer (IST) em `smr.c`

- **[`smr_handle_state_transfer(const uint8_t *payload, uint32_t tamanho, uint8_t **resposta, uint32_t *tamanho_resposta)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L597-L653)**
  - Executada pelo **Coordenador** ao receber `MSG_STATE_TRANSFER`.
  - Lê os 8 bytes de `from_version` (último `log_index` aplicado pelo nó solicitante).
  - Conta quantas entradas no vetor `smr_log` possuem `entry.log_index > from_version` e soma os tamanhos dos seus `raw_payload`.
  - Aloca o buffer de resposta, grava `smr_applied` (8 bytes) e `count` (4 bytes), seguidos de cada `[raw_len (4 B) | raw_payload]` das entradas faltantes.
  - Quando `count > 0`, imprime `IST: enviando <count> logs pendentes (versao <from+1> -> <smr_applied>)`.

- **[`smr_sync_with_coordinator(const ChordNode *coord)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L656-L756)**
  - Executada pelo **Follower** ao entrar na rede, ao reiniciar após queda ou ao receber anúncio do coordenador.
  - Obtém `local_ver = smr_last_applied()` e envia `MSG_STATE_TRANSFER` ao coordenador.
  - Lê `coord_ver` e `count` da resposta `MSG_ACK`. Se `count == 0`, o nó já está atualizado e retorna silenciosamente.
  - Se `count > 0`, imprime as mensagens de estado do IST:
    - `IST: SYNCHRONIZING (local version=..., coordinator version=..., versions missing=...)`
    - `IST: receive logs (... .. ...) de <coord>`
  - Para cada entrada recebida, valida o `checksum` SHA-256 com `smr_checksum`, deserializa o metadado e aplica na ordem com `smr_append_locked`.
  - Ao concluir todas as entradas, imprime `IST: READY (sincronizado ate LOG #...)`.

---

### 2.4 Evidências de Execução e Logs Reais no Teste Oficial (`tests/c5/run.sh`)

Trecho extraído da execução real do script oficial `./tests/c5/run.sh`:

#### 1. Execução do 2PC (`PREPARE` $\to$ `YES` $\to$ `COMMIT`) durante o Upload de `documento_a.pdf`
```text
2PC [IDLE -> PREPARING] LOG #1 TransactionID=00006aea29a0dec6ddd4e296a6490001 (2 participantes)
2PC PREPARE -> SP1
RX PREPARE de dec6ddd4e296a64948e7027bac7babc388273878f65c7c62bffa2a8fab512cc0
2PC PREPARE #1 -> YES (estado READY)
2PC YES <- SP1
2PC PREPARE -> SP3
RX PREPARE de dec6ddd4e296a64948e7027bac7babc388273878f65c7c62bffa2a8fab512cc0
2PC PREPARE #1 -> YES (estado READY)
2PC YES <- SP3
2PC [READY -> COMMIT] LOG #1 confirmado
LOG #1
PUT /mnt/c/Arquivos/Faculdade/4_Year/PD/Trabalho/tests/data/documento_a.pdf
TransactionID: 00006aea29a0dec6ddd4e296a6490001  Version: 1  Checksum: ...
2PC COMMIT -> SP1
2PC COMMIT #1 recebido -> aplicando operacao
LOG #1
2PC COMMIT -> SP3
2PC COMMIT #1 recebido -> aplicando operacao
LOG #1
2PC [COMMIT -> DONE] LOG #1
```

#### 2. Sincronização Incremental de Estado (`IST` / `STATE_TRANSFER`) acionada no `JOIN`
```text
RX JOIN de 1bc20b4a42e6117460b3e4997a4d0b1e8a6aa066b40542eb0d04310640395cce
JOIN: NodeID ja registrado, tabela inalterada
RX STATE_TRANSFER de 92cdb98a1d355b70ed24933c4d1ff4be32e6671a0d3436aef44887b9f3aaf570
IST: STATE_TRANSFER verificado com SP2 (local version=1, coordinator version=1, READY)
```
*(E quando há versões faltantes entre um Follower reiniciado/atrasado e o Coordenador):*
```text
IST: enviando 2 logs pendentes (versao 2 -> 3)
IST: SYNCHRONIZING (local version=1, coordinator version=3, versions missing=2)
IST: receive logs (2 .. 3) de SP2
LOG #2
LOG #3
IST: READY (sincronizado ate LOG #3)
```

---

### 2.5 Adaptações Realizadas para o Script Oficial do Professor (`tests/c5/run.sh`)

Ao testar com os arquivos oficiais `c5/run.sh` e `c5/c5.conf`, identificamos e ajustamos no código:
1. **Janela de `sleep 8` antes do primeiro `upload`:** O script oficial inicia `SP1`, `SP2` e `SP3`, aguarda `sleep 8` e imediatamente dispara `upload`. No Checkpoint 4, `BULLY_START_SEC` estava em `10` segundos, o que faria o `upload` chegar em $t = 8\text{ s}$ antes da eleição do coordenador. Reduzimos `BULLY_START_SEC` para `5` segundos em [`include/bully.h`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/bully.h#L12), garantindo que em $t = 5\text{ s}$ a eleição Bully já concluiu e em $t = 8\text{ s}$ o 2PC (`PREPARE` $\to$ `COMMIT`) funciona na primeira tentativa.
2. **Acionamento de `STATE_TRANSFER` via `--cmd join`:** Nas linhas 32–36 do `tests/c5/run.sh`, o script executa `"$CLIENT_BIN" --cmd join --host 127.0.0.1 --port 5101` para simular a entrada/sincronização e verifica `IST|STATE_TRANSFER|checkpoint|version` no log. Integramos em [`tratar_join`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L293-L299) uma chamada a [`smr_sync_with_coordinator`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/smr.c#L656-L756) para que o Super Peer consulte o coordenador via `MSG_STATE_TRANSFER` e registre o estado de sincronização `IST` no log.
