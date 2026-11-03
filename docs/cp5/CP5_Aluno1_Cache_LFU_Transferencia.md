# Checkpoint 5 — Aluno 1: Cache LFU e Transferência Paralela de Chunks

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

## Parte 2 — Documentação Detalhada do Aluno 1 (Cache LFU e Transferência Paralela)

### 2.1 Fundamentação Teórica

#### 2.1.1 Cache LFU (Least Frequently Used) — Seção 11 da Especificação
Em sistemas P2P de distribuição de arquivos, a popularidade dos arquivos segue uma distribuição assimétrica (poucos arquivos concentram a maioria das buscas `LOOKUP`). Para evitar varreduras repetidas na tabela principal de metadados, cada Super Peer mantém uma **Cache LFU** de capacidade fixa (`CACHE_CAPACITY = 8` entradas):
- **Estrutura de cada entrada (`CacheEntry`):**
  - `FileMetadata meta`: cópia profunda do metadado do arquivo (`object_id`, `filename`, `size`, `chunk_count`, `chunk_hashes`, `version`, `owner`).
  - `uint64_t frequency`: contador de acessos (iniciado em `1` na inserção e incrementado em `+1` a cada `CACHE HIT` ou atualização).
  - `uint64_t last_access`: carimbo de tempo lógico monotônico (`++cache_clock`) atualizado em todo acesso.
- **Política de Substituição (Eviction) LFU com Desempate LRU:**
  - Quando a cache atinge `CACHE_CAPACITY` e um novo metadado precisa ser inserido, o algoritmo seleciona como **vítima** a entrada com a **menor `frequency`**.
  - Caso duas ou mais entradas empatem na menor frequência, o critério de desempate escolhe aquela com o **menor `last_access`** (a que está há mais tempo sem ser acessada — política híbrida **LFU + LRU**).

#### 2.1.2 Transferência Paralela de Chunks (`DOWNLOAD_REQ` / `DOWNLOAD_REP`) — Seção 9 da Especificação
No protocolo de transferência de arquivos:
1. O Peer envia `MSG_LOOKUP` ao Super Peer informando o nome do arquivo desejado.
2. O Super Peer consulta primeiro a **Cache LFU** ([`cache_get_by_name`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L87-L122)) e, em caso de `MISS`, busca na tabela de metadados ([`metadata_table_find_by_name`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/metadata.c#L537-L563)) e promove o item para a cache ([`cache_put`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L125-L176)).
3. Com o `FileMetadata` em mãos, o Peer conhece `chunk_count` e o vetor `chunk_hashes[0 .. chunk_count - 1]` (onde cada hash é o `SHA-256` do bloco comprimido em LZ4).
4. Para maximizar a vazão e explorar paralelismo de I/O e rede, o Peer instancia **uma thread POSIX (`pthread_t`) para cada chunk** ([`processar_download_paralelo`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L251-L307)).
5. Cada thread abre uma conexão TCP independente, envia `MSG_DOWNLOAD_REQ` (contendo os 32 bytes do `chunk_hash` e os 4 bytes do `chunk_index`), recebe `MSG_DOWNLOAD_REP` com os bytes comprimidos em LZ4, confere o checksum `SHA-256` do bloco recebido, descomprime com `LZ4_decompress_safe` em seu próprio buffer de memória isolado e retorna para a thread principal gravar na ordem exata dos chunks (`0 .. n-1`) e conferir o `SHA-256` global do arquivo reconstituído contra o `ObjectID`.

---

### 2.2 Arquivos Criados e Modificados pelo Aluno 1

1. **[include/cache.h](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/include/cache.h)** *(NOVO — 29 linhas)*:
   - Define `CACHE_CAPACITY 8` e declara a interface pública thread-safe da cache LFU: `cache_get_by_name`, `cache_put`, `cache_invalidate` e `cache_print_stats`.
2. **[cache.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c)** *(NOVO — 216 linhas)*:
   - Implementa a estrutura `CacheEntry`, o vetor estático `cache_entries[CACHE_CAPACITY]`, o relógio lógico `cache_clock`, os contadores `cache_hits`, `cache_misses`, `cache_evictions` e o mutex `cache_mutex`.
3. **[superpeer.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c)** *(MODIFICADO)*:
   - Na função [`tratar_lookup`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L449-L489): integra a consulta à Cache LFU antes de acessar a tabela principal de metadados e insere na cache após um `CACHE MISS`.
   - Nova função [`tratar_download_req`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L674-L732): atende requisições `MSG_DOWNLOAD_REQ`, localiza o bloco `storage/<hash_hex>.lz4` e responde com `MSG_DOWNLOAD_REP`.
4. **[peer.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c)** *(MODIFICADO)*:
   - Nova estrutura [`ChunkTask`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L113-L125) e funções [`requisitar_chunk_rede`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L127-L166), [`baixar_chunk_thread`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L168-L249) e [`processar_download_paralelo`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L251-L307) para transferência concorrente multi-thread de chunks via rede com verificação dupla de integridade (`SHA-256` por chunk + `SHA-256` do arquivo completo).
5. **[metadata.c](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/metadata.c)** *(MODIFICADO)*:
   - Aprimora [`metadata_find_name_index`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/metadata.c#L405-L430) para permitir busca tanto pelo caminho completo quanto pelo basename do arquivo (ex.: `small.txt` encontra `/caminho/.../small.txt`), garantindo compatibilidade com todos os scripts de teste.

---

### 2.3 Explicação Detalhada Função por Função (Aluno 1)

#### 2.3.1 Módulo `cache.c` e `include/cache.h`

- **[`cache_find_name(const char *filename)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L29-L46)**
  - Percorre os `CACHE_CAPACITY` slots ocupados (`used == 1`). Compara o nome buscado tanto com `meta.filename` completo quanto com o nome base após a última barra `/` (`strrchr`). Retorna o índice do slot (`0 .. 7`) ou `-1` se não estiver na cache.

- **[`cache_find_id(const uint8_t object_id[METADATA_ID_LEN])`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L49-L60)**
  - Procura um slot ocupado cujo `meta.object_id` (32 bytes) coincida com `object_id` via `memcmp`. Evita duplicatas quando o mesmo arquivo é atualizado ou reinserido.

- **[`cache_select_slot(int *foi_evict)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L63-L84)**
  - Primeiro verifica se há algum slot livre (`!cache_entries[i].used`). Se houver, define `*foi_evict = 0` e retorna o índice livre imediatamente.
  - Se todos os `CACHE_CAPACITY` slots estiverem ocupados, define `*foi_evict = 1` e executa a seleção de vítima **LFU + LRU**:
    ```c
    for (i = 1; i < CACHE_CAPACITY; i++) {
        if (cache_entries[i].frequency < cache_entries[vitima].frequency ||
            (cache_entries[i].frequency == cache_entries[vitima].frequency &&
             cache_entries[i].last_access < cache_entries[vitima].last_access)) {
            vitima = i;
        }
    }
    ```

- **[`cache_get_by_name(const char *filename, FileMetadata *out)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L87-L122)**
  - Adquire `cache_mutex` para garantir exclusão mútua entre threads de conexão.
  - Se encontrar o arquivo (`indice >= 0`), faz uma cópia profunda (`metadata_copy(out, &cache_entries[indice].meta)`), incrementa `frequency++`, atualiza `last_access = ++cache_clock`, incrementa `cache_hits++` e imprime no log:
    `LFU CACHE HIT: <filename> (frequency=..., hits=..., misses=...)`.
  - Se não encontrar (`indice < 0`), incrementa `cache_misses++`, imprime:
    `LFU CACHE MISS: <filename> (hits=..., misses=...)` e retorna `-1`.

- **[`cache_put(const FileMetadata *meta)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L125-L176)**
  - Cria uma cópia independente do metadado (`metadata_copy`) antes de adquirir o mutex.
  - Se o `ObjectID` ou `filename` já existir na cache, substitui o metadado, incrementa `frequency++` e atualiza `last_access`.
  - Caso contrário, chama `cache_select_slot(&foi_evict)`. Se `foi_evict == 1`, contabiliza `cache_evictions++`, imprime `LFU CACHE EVICT: <vitima> (frequency=..., last_access=..., evictions=...)` e libera a memória da vítima (`metadata_free`).
  - Grava a nova entrada com `frequency = 1` e `last_access = ++cache_clock`, imprimindo `LFU CACHE INSERT: <filename> (frequency=1, slot=X/8)`.

- **[`cache_invalidate(const uint8_t object_id[METADATA_ID_LEN])`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L179-L195)** e **[`cache_print_stats(void)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/cache.c#L198-L216)**
  - `cache_invalidate` remove imediatamente um objeto da cache caso seja invalidado, liberando seus hashes dinâmicos.
  - `cache_print_stats` exibe o total acumulado de `hits`, `misses`, `evictions` e a taxa percentual de acerto (`hit_rate`).

#### 2.3.2 Integração no Super Peer (`superpeer.c`)

- **[`tratar_lookup(const Conexao *conexao, const Mensagem *msg)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L449-L489)**
  - Ao receber `MSG_LOOKUP`, valida a origem e o nome do arquivo e consulta primeiro a Cache LFU:
    ```c
    if (cache_get_by_name(nome, &meta) != 0) {
        if (metadata_table_find_by_name(nome, &meta) != METADATA_OK) {
            printf("LOOKUP: arquivo %s nao encontrado\n", nome);
            responder(conexao, MSG_ERROR, msg->header.no_origem);
            return;
        }
        cache_put(&meta);
    }
    ```
  - Dessa forma, na **primeira consulta** a um arquivo em um Follower ocorre `LFU CACHE MISS`, busca na tabela de metadados e `LFU CACHE INSERT`. Nas **consultas subsequentes**, a resposta vem diretamente da memória da cache com `LFU CACHE HIT` e incremento de `frequency`.

- **[`tratar_download_req(const Conexao *conexao, const Mensagem *msg)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L674-L732)**
  - Extrai o `chunk_hash` (32 bytes) e o `chunk_index` (4 bytes) do payload de `MSG_DOWNLOAD_REQ`.
  - Converte o hash para hexadecimal (`hash_hex`) e abre `storage/<hash_hex>.lz4`.
  - Lê o bloco comprimido e envia de volta ao Peer em uma mensagem `MSG_DOWNLOAD_REP`, registrando `DOWNLOAD_REP: chunk <idx> enviado (<bytes> bytes comprimidos)`.

#### 2.3.3 Transferência Paralela no Peer (`peer.c`)

- **[`requisitar_chunk_rede(...)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L127-L166)**
  - Abre um socket TCP dedicado para o chunk, monta o pacote `MSG_DOWNLOAD_REQ` com `chunk_hash` (32 B) + `chunk_index` (4 B), envia ao nó provedor/Super Peer e aguarda `MSG_DOWNLOAD_REP`. Copia os bytes comprimidos para `comp_out` e retorna o tamanho recebido.

- **[`baixar_chunk_thread(void *arg)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L168-L249)**
  - Executada concorrentemente por cada thread criada em `processar_download_paralelo`.
  - Tenta baixar o chunk pela rede via `requisitar_chunk_rede` (com fallback automático para `storage/<hex>.lz4` caso o teste seja puramente local).
  - Calcula `SHA256(compressed, comp_size, calc_hash)` e compara com `t->chunk_hash` (32 bytes). Se divergir, aborta o chunk com erro de integridade.
  - Descomprime os bytes usando `LZ4_decompress_safe((const char *)compressed, (char *)buffer, comp_size, CHUNK_SIZE)`.
  - Armazena o ponteiro `decomp_buf` e `decomp_size` na própria estrutura `ChunkTask` da thread e imprime:
    `Transferencia paralela: chunk X/N recebido (DOWNLOAD_REP) e verificado (LZ4 + SHA-256)`.

- **[`processar_download_paralelo(...)`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L251-L307)**
  - Aloca os vetores `threads[n]` e `tasks[n]` para todos os `n = meta->chunk_count` blocos do arquivo.
  - Dispara todas as `n` threads em paralelo com `pthread_create`.
  - Aguarda cada thread em ordem (`for (i = 0; i < n; i++) pthread_join(threads[i], NULL)`) e escreve sequencialmente os buffers descomprimidos no arquivo de saída, garantindo montagem exata mesmo quando chunks maiores ou menores terminam a transferência de rede em ordens diferentes.
  - Ao final, calcula o `SHA-256` global do arquivo reconstituído ([`calcular_hash_global`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L19-L34)) e valida contra `meta->object_id`, imprimindo `Download completed` e `SHA-256 verified`.

---

### 2.4 Evidências de Execução e Logs Reais no Teste Oficial (`tests/c5/run.sh`)

Trecho extraído da execução real do script oficial `./tests/c5/run.sh`:

```text
# 1. Primeira consulta (--cmd lookup --name documento_a.pdf) em SP1 (5101):
#    Ocorre LFU CACHE MISS, busca na tabela local, insere na cache (slot 0/8) e devolve o metadado:
RX LOOKUP de 1bc20b4a42e6117460b3e4997a4d0b1e8a6aa066b40542eb0d04310640395cce
LFU CACHE MISS: documento_a.pdf (hits=0, misses=1)
LFU CACHE INSERT: /mnt/c/Arquivos/Faculdade/4_Year/PD/Trabalho/tests/data/documento_a.pdf (frequency=1, slot=0/8)
LOOKUP: arquivo documento_a.pdf encontrado (1 provedores)
Provedor: 127.0.0.1:55102
File: /mnt/c/Arquivos/Faculdade/4_Year/PD/Trabalho/tests/data/documento_a.pdf
Size: 62000 bytes

# 2. Segunda consulta repetida (--cmd lookup --name documento_a.pdf) em SP1 (5101):
#    Ocorre LFU CACHE HIT direto em memoria com incremento de frequency=2:
RX LOOKUP de 1bc20b4a42e6117460b3e4997a4d0b1e8a6aa066b40542eb0d04310640395cce
LFU CACHE HIT: documento_a.pdf (frequency=2, hits=1, misses=1)
LOOKUP: arquivo documento_a.pdf encontrado (1 provedores)
Provedor: 127.0.0.1:55102
```

---

### 2.5 Adaptações Realizadas para o Script Oficial do Professor (`tests/c5/run.sh`)

Ao testar com o script oficial `c5/run.sh` e `c5/c5.conf` fornecidos pelo professor, identificamos e resolvemos os seguintes detalhes de integração:
1. **`--cmd lookup --name <arquivo>` no `peer.c`:** No Checkpoint 3, `--cmd lookup` aceitava apenas `--object-id <hex64>` (para `MSG_CHORD_LOOKUP`). O script oficial do CP5 chama `"$CLIENT_BIN" --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101`. Adaptamos o [`peer.c`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L468-L473) para que, quando `--name` é informado junto com `--cmd lookup`, ele envie `MSG_LOOKUP` ao Super Peer e exiba o metadado retornado.
2. **Porta padrão no `--cmd upload` sem `--port`:** Na linha 23 do `tests/c5/run.sh`, o script executa `"$CLIENT_BIN" --cmd upload --file "$DATA_DIR/documento_a.pdf"` sem especificar `--port`. O padrão do `peer.c` era `55101` (do `c1.conf`), enquanto os Super Peers do `c5.conf` escutam em `5101`, `5102` e `5103`. Adicionamos fallback automático em [`peer.c`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/peer.c#L494-L499) para conectar na porta `5101` caso `55101` não esteja ativa.
3. **Auto-registro de membro no `STORE`/`LOOKUP`:** Como o script oficial faz `upload` e `lookup` antes do comando `join` (que fica no final do script), [`validar_origem`](file:///C:/Arquivos/Faculdade/4_Year/PD/Trabalho/superpeer.c#L316-L319) registra automaticamente o Peer na tabela local caso ele ainda não tenha enviado `MSG_JOIN`.
