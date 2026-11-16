# Checkpoint 6 — Integração e Teste Final (Aluno 1 + Aluno 2)

Documento do Checkpoint 6 (Seção 16.11 da especificação; Observação 2 da Seção 16.12).

---

## 1. O que o Checkpoint 6 pede

A Seção 16.11 pede a **integração e o teste final** do sistema:

| Integrante | Deve demonstrar |
|---|---|
| **Aluno 1** | upload, download, chunks, compression, cache, replica |
| **Aluno 2** | Chord, Gossip, Bully, SMR, 2PC, IST |
| **Ambos** | Integração, Debugging, Documentação e Testes |

Os mecanismos foram implementados nos checkpoints 2 a 5 (documentos em `docs/cp3`, `docs/cp4` e `docs/cp5`). No CP6, o trabalho foi integrar tudo, consolidar o código e corrigir o que a bateria final mostrou.

---

## 2. Integração: como o código ficou organizado

O código foi consolidado nos 5 arquivos do Checkpoint 1. Os `.h` de `include/` continuam sendo a interface de cada camada.

| Arquivo | Conteúdo |
|---|---|
| `protocol.c` | header de 99 bytes, CRC32, empacotar/desempacotar, nomes dos tipos de mensagem |
| `network.c` | sockets TCP, framing (header fixo + payload pelo tamanho) |
| `node.c` | NodeID = SHA256(IP‖Porta‖UUID) com UUID persistido, configuração, metadados (hash table + índice por nome) |
| `peer.c` | cliente: upload (SHA-256 → chunks de 4 MiB → LZ4), download paralelo com troca de réplica |
| `superpeer.c` | Super Peer: tabela de membros, Chord, Gossip, Bully, SMR + 2PC + IST, Cache LFU, armazenamento e réplica dos chunks |

O `arquitetura.md` (seção 15) mostra em qual arquivo ficou cada módulo antigo (`chord.c`, `gossip.c`, `bully.c`, `smr.c`, `cache.c`, `metadata.c`).

Fluxo de upload integrado (Seções 3.7 e 11.4):

```
Peer: arquivo → SHA-256 (ObjectID) → chunks de 4 MiB → SHA-256 de cada chunk → LZ4
  │
  ├─ STORE (metadado) ─► Super Peer ─► Coordenador (Bully)
  │                                     └─ 2PC: PREPARE → YES/NO → COMMIT/ABORT → LOG #n (SMR)
  │
  └─ CHUNK_STORE (cada chunk) ─► Super Peer de entrada
                                   ├─ LZ4 decode → SHA-256 → validação → storage/<porta>/
                                   └─ replica o chunk para os demais Super Peers vivos
```

Fluxo de download (Seções 3.8 e 11.5):

```
LOOKUP nome → Cache LFU (HIT/MISS) → índice por nome → metadado + lista de réplicas
  → 1 thread por chunk → DOWNLOAD_REQ à réplica (round-robin, troca de réplica se falhar)
  → LZ4 decode → SHA-256 do chunk → reconstrução → SHA-256 do arquivo = ObjectID
```

---

## 3. Aluno 1 — o que demonstrar e onde está

| Item | Onde | Evidência no log |
|---|---|---|
| **upload** | `peer.c` `processar_upload`, `enviar_chunks` | `File:`, `ObjectID:`, `Chunk n/3 enviado ao Super Peer ... e replicado` |
| **chunks** | `CHUNK_SIZE = 4 MiB` (`include/metadata.h`) | `Chunks: 3` para o arquivo de 10 MiB |
| **compression** | LZ4 por chunk (`LZ4_compress_default` / `LZ4_decompress_safe`) | `Compression: LZ4` |
| **cache** | `superpeer.c` camada Cache (LFU, desempate pelo último acesso, capacidade 8) | `LFU CACHE MISS`, `LFU CACHE INSERT`, `LFU CACHE HIT (frequency=…)`, `LFU CACHE EVICT` |
| **replica** | `superpeer.c` `tratar_chunk_store` / `replicar_chunk`; `peer.c` troca de réplica | `CHUNK_STORE: chunk … com 3 copias`, `selecionando proxima replica` |
| **download** | `peer.c` `processar_download_paralelo` | `DOWNLOAD_REP replica …`, `Download completed`, `SHA-256 verified` |

Demonstração manual (3 terminais com Super Peers e 1 com o peer):

```bash
make
./bin/superpeer --config tests/config/c6.conf --port 5101
./bin/superpeer --config tests/config/c6.conf --port 5102
./bin/superpeer --config tests/config/c6.conf --port 5103

./bin/peer --cmd upload   --file tests/data/documento_c.pdf --host 127.0.0.1 --port 5101
./bin/peer --cmd lookup   --name documento_c.pdf --host 127.0.0.1 --port 5102   # MISS
./bin/peer --cmd lookup   --name documento_c.pdf --host 127.0.0.1 --port 5102   # HIT
./bin/peer --cmd download --name documento_c.pdf --output /tmp/saida.pdf --host 127.0.0.1 --port 5103
# replica: derrube um Super Peer (Ctrl+C no 5102) e repita o download
```

---

## 4. Aluno 2 — o que demonstrar e onde está

| Item | Onde (`superpeer.c`) | Evidência no log |
|---|---|---|
| **Chord** | camada Chord: successor, predecessor, finger table, stabilize, notify, fix_fingers | `Successor:`, `Finger table:`; `./bin/peer --cmd topology` e `--cmd lookup --object-id` |
| **Gossip** | camada Gossip: heartbeat 5 s, timeout 15 s, ALIVE → SUSPECT → FAILED → REMOVED | `SUSPECT SP2`, `FAILED SP2`, `Gossip: SP2 voltou (FAILED -> ALIVE)` |
| **Bully** | camada Bully: maior NodeID vence, ELECTION / OK / COORDINATOR | `detect failure`, `ELECTION -> …`, `highest NodeID wins`, `COORDINATOR …` |
| **SMR** | camada SMR: log com LogIndex, TransactionID, versão, checksum SHA-256 | `LOG #1`, `PUT documento_c.pdf` |
| **2PC** | `smr_put` + `smr_handle_prepare/commit/abort` | `2PC [IDLE -> PREPARING]`, `2PC YES <-`, `2PC [COMMIT -> DONE]`; com `SIMULATE_PREPARE_FAIL=1`: `2PC [PREPARING -> ABORT]` |
| **IST** | `smr_sync_with_coordinator` / `smr_handle_state_transfer` | `IST: SYNCHRONIZING (… versions missing=N)`, `IST: receive logs (a .. b)`, `IST: READY` |

---

## 5. Testes

### 5.1 Bateria final: `bash tests/c6/run.sh`

Repete os itens de verificação da especificação em sequência, sobre 3 Super Peers:

| Fase | Item da especificação | Verificações |
|---|---|---|
| Aluno 1 | 16.11 | chunks (3), LZ4, LFU MISS/HIT, 3 cópias de cada chunk, download entre réplicas, arquivo baixado idêntico ao original |
| Réplica | 11.6 | `kill -9` no SP2 → download troca de réplica → arquivo idêntico |
| Gossip | 16.8 | SP2 morto → `SUSPECT SP2` |
| IST | 16.10 | 2 uploads com o SP2 fora → SP2 religado → `versions missing` → `receive logs` → `READY`, e o Gossip o reconhece como ALIVE |
| Bully | 16.9 | `kill -9` no coordenador atual → detecção → ELECTION → maior NodeID vence → COORDINATOR |
| 2PC | 16.10 / 14.7 | `SIMULATE_PREPARE_FAIL=1` → falha no PREPARE → ABORT, sem COMMIT aplicado |
| Robustez | 16.6 | nenhum crash nos logs |

Resultado: **29 PASS, 0 FAIL**. A bateria também passa com os binários compilados com `-fsanitize=address,undefined`, sem nenhum relatório do sanitizer.

> **Atenção à verificação "ELECTION enviada aos nós de maior prioridade".** Ela depende de qual Super Peer percebe primeiro a queda do coordenador, e às vezes falha (28/29), **com ou sem as mudanças da Seção 8**. Ver a Seção 8.4.

### 5.2 Baterias anteriores (regressão)

| Bateria | Resultado |
|---|---|
| `tests/c1/test_protocol` | 5/5 |
| `bash tests/run.sh` (C2) | 8/8 |
| `bash tests/c3/run.sh` | 5/5 |
| `bash tests/c4/run.sh` | 4/4 |
| `bash tests/c5/run.sh` | 5/5 |

---

## 6. Debugging: problemas encontrados e corrigidos no CP6

| # | Problema | Causa | Correção |
|---|---|---|---|
| 1 | **Réplica só funcionava na mesma pasta.** Com os Super Peers em diretórios diferentes, todas as réplicas respondiam ERROR e o download falhava. | Os chunks só eram gravados no `storage/` do cliente; os Super Peers liam esse mesmo diretório. | Nova mensagem interna `CHUNK_STORE` (tipo 29): o peer envia os chunks ao Super Peer, que valida, grava em `storage/<porta>/` e replica para os demais. O LOOKUP não anuncia mais um provedor falso (`127.0.0.1:55102`) para peers sem JOIN. |
| 2 | **SHA-256 do chunk calculado sobre o dado comprimido.** | A Seção 10.4 define `LZ4 Decode → SHA-256 → Validação`, e a 11.3 pede o hash sobre o conteúdo descomprimido. | O hash passa a ser calculado sobre o chunk original no upload, e a validação é feita depois da descompressão. |
| 3 | **Super Peer reiniciado continuava "morto" para os outros**, e por alguns segundos havia dois coordenadores. | O contador de heartbeat recomeçava em 0, e os outros nós ignoravam heartbeats menores que o último visto. | O contador começa no horário de início do processo (número de encarnação). |
| 4 | **Documentação dos CP3–CP5 removida** e `arquitetura.md` citando arquivos que não existem mais. | Consolidação dos arquivos. | `docs/` restaurado, este documento criado e `arquitetura.md` atualizado (seções 15 e 16). |
| 5 | **Teste final não derrubava nenhum nó.** | As verificações de Gossip, Bully e IST passavam só com a subida normal. | `tests/c6/run.sh` reescrito com os cenários de falha da especificação. |

---

## 7. Decisões de projeto registradas (Apêndice C, `[P-02]`)

| Ref. | Decisão |
|---|---|
| `[C-02]` TransactionID | 128 bits = timestamp (48) ‖ 64 bits mais significativos do NodeID ‖ sequência (16) (ver `docs/cp4`). |
| `[C-07]` Consistência forte | É forte no caminho de escrita: toda alteração de metadado passa por 2PC e entra no log SMR em ordem total (`LogIndex`). O Gossip é usado **apenas** para membership e detecção de falha, nunca para propagar metadados. |
| `[C-08]` AUTHENTICATED | Handshake trivial: confere a versão do protocolo e se o NodeID não é zero. Sem TLS ou chaves. |
| `[C-16]` RNF-010 (eleição < 5 s) | Medida a partir do envio da mensagem `ELECTION`. A espera por OK/COORDINATOR é limitada a `BULLY_WAIT_SEC = 5`. Da falha até a eleição soma-se o timeout de 15 s do heartbeat. |
| `[C-18]` 2PC bloqueante | Implementado como especificado. Se o coordenador cair depois do PREPARE e antes da decisão, os participantes ficam em READY até um novo coordenador ser eleito; a limitação está documentada em `docs/cp5`. |
| Réplica dos chunks | Os chunks são replicados para todos os Super Peers vivos no momento do upload (com 3 Super Peers, isso equivale ao `ReplicationFactor = 3` da Seção 8.6). Um Super Peer que estava fora no upload recebe os metadados pelo IST, mas não os chunks; o cliente então busca o chunk em outra réplica (Seção 11.6). |

---

## 8. Mudanças depois da bateria final (Debugging e Integração)

Estas mudanças vieram de levar o sistema para várias máquinas (o harness distribuído) e de conferir o código contra o enunciado.

### 8.1 Correções

| # | Problema | Causa | Correção |
|---|---|---|---|
| 6 | **Não funcionava em várias máquinas.** Chord, Gossip e Bully anunciavam `127.0.0.1` aos outros nós. | O IP do Super Peer era fixo (`IP_PADRAO`). | O Super Peer lê o próprio IP da linha do roster que tem a sua porta (`ler_linha_csv`). Com o roster local (127.0.0.1) nada muda. |
| 7 | **O antigo coordenador retomava a coordenação ao voltar** e, durante essa eleição, recusava STORE (`STORE: sem coordenador`). | A eleição de partida (`bully_loop`) rodava sempre, mesmo com um coordenador vivo. Contraria a Seção 15.7. | Com o sistema em operação (algum `LOG` aplicado) e um coordenador vivo, o nó entra como FOLLOWER (`Bully: coordenador X ja conhecido e sistema em operacao ... (Secao 15.7)`). Na montagem do cluster (LOG 0), a eleição de partida continua: o maior NodeID vence, como no Checkpoint 4. |
| 8 | **O nó que voltava demorava a saber quem era o coordenador** (até 5 s), e às vezes decidia a eleição sem saber. | O coordenador só anunciava a cada 5 s e só para quem já via como vivo. | Ao entrar, o nó avisa logo os membros que o bootstrap passou (`Gossip: entrada anunciada a N membros`), e o coordenador anuncia `COORDINATOR` assim que vê um membro novo ou de volta (UC-01). |
| 9 | **Metadado com 7 dos 13 campos** da Seção 8.3 (`[C-05]`). | O `FileMetadata` era o mínimo do Checkpoint 2. | Acrescentados `Compression`, `UploadDate`, `LastAccess`, `DownloadCounter`, `Status` e `ReplicaPeers` (serializados no STORE, no LOOKUP e no log). `./bin/peer --cmd lookup --name X` mostra todos. |
| 10 | **Não havia remoção de documento** (ator Usuário, Seção 8.4, operação `DELETE` da Seção 12.3, exemplo do Checkpoint 4). | Só existia `PUT`. | `delete <nome>` no cliente: busca o metadado, marca `Status = REMOVED` e envia um STORE. O coordenador registra `DELETE` no log via 2PC e cada Super Peer marca o registro, com `Version++`. LOOKUP e download passam a responder "não encontrado"; um novo upload do mesmo arquivo reativa o registro. Um nó que estava fora recebe o `DELETE` pelo IST. |

### 8.2 Cliente com sessão (`--interactive` / `--batch`)

O `bin/client` continua aceitando um comando por execução (`--cmd ...`) e ganhou uma sessão com um NodeID só, usada pelo harness distribuído:

```text
bin/client --interactive --host <SP> --port <porta>     # comandos pela stdin
bin/client --batch arquivo.cmds                          # comandos de um arquivo
upload <arq> | download <nome> [saida] | lookup <nome|ObjectID> | delete <nome>
state | ping | join | leave [host porta] | sleep <s> | help | quit
```

Cada comando termina com `[done N rc=R]`. O código de saída do modo `--cmd` agora é diferente de 0 quando a operação falha (antes era sempre 0).

### 8.3 Decisões de projeto acrescentadas (`[P-02]`)

| Ponto | Decisão |
|---|---|
| Disparo da eleição | O Bully elege quando o coordenador fica **SUSPECT** (timeout de 15 s), apoiado na Seção 5.3 ("após o timeout... Bully Election é iniciado"). As Seções 7.7 e 15.8 sugerem esperar a confirmação (FAILED, 30 s); optamos pela recuperação mais rápida. |
| Volta de um nó (15.7) | "Sistema em operação" = algum `LOG` aplicado depois do IST. Sem operações (montagem), a eleição de partida é a do Bully clássico. |
| `ReplicaPeers` | Os Super Peers que aplicam a entrada: o coordenador e os participantes ALIVE do 2PC. |
| `LastAccess` e `DownloadCounter` | Estatísticas **locais** de cada Super Peer (RF-024), atualizadas a cada LOOKUP que encontra o arquivo (todo download começa por um LOOKUP). Não passam pelo SMR. |
| Remoção | Lógica (Seção 8.4): o registro fica com `Status = REMOVED`; os chunks continuam guardados. Reaproveita o `STORE`, sem criar mensagem fora da Seção 5.1. |
| Réplica dos chunks | Para todos os Super Peers vivos (ver Seção 7). O fator de replicação da Seção 8.6 não é configurável. |

### 8.4 A verificação instável do `tests/c6`

A verificação "Bully: ELECTION enviada aos nós de maior prioridade" procura a linha `ELECTION ->` depois que o coordenador SP1 cai. Sobram SP2 (maior NodeID) e SP3:

- se o **SP3** percebe a queda primeiro, manda `ELECTION -> SP2`, recebe OK e o SP2 vence → a verificação passa;
- se o **SP2** percebe primeiro, não há nó maior vivo a quem mandar `ELECTION`: ele vence direto (`detect failure` → `start election` → `highest NodeID wins: SP2`) → a verificação falha.

As duas sequências são o Bully correto, e o resultado é o mesmo (o maior nó vivo assume; as outras verificações do Bully passam). Quem percebe primeiro depende da fração de segundo em que cada processo roda o ciclo do Gossip e do Bully, isto é, do instante em que cada um subiu. Medimos: o código anterior às mudanças desta seção também falha quando a corrida vira (3 de 3 ensaios com o teste iniciado na mesma fração de segundo), e o código atual passou 3 de 3 no `c6`. Se só essa verificação falhar, basta rodar a bateria de novo.

### 8.5 Harness distribuído e interface (ferramentas de teste)

`script/bittorrent-project/tests/cluster/` sobe Super Peers e clientes em várias máquinas por ssh, a partir de um `cluster.conf`, e roda os cenários `smoke`, `final` (uma fase por item do CP6, incluindo a remoção lógica) e `carga`. O `README.md` dessa pasta explica o uso no laboratório. A interface gráfica (`gui/`, tkinter + matplotlib) é só uma ferramenta de operação e visualização do harness: a interface gráfica está **fora do escopo** do sistema (Seção 1.3). O uso da interface, uma bateria de testes passo a passo e o roteiro de demonstração do Aluno 2 estão em [`Guia_Interface_e_Demonstracao.md`](Guia_Interface_e_Demonstracao.md).

### 8.6 Limitações conhecidas

- O IST transfere os logs pendentes; não há snapshot/checkpoint (Seções 12.4 e 13.1).
- A compressão dos chunks é sequencial; o download é paralelo (uma thread por chunk), mas não há o pool de compressão da Seção 10.5.
- O cache LFU é uma região só (a Seção 9.4 sugere quatro).
- Um Super Peer que estava fora no upload recebe o metadado pelo IST, mas não os chunks (o cliente busca o chunk em outra réplica).
- O 2PC é bloqueante (`[C-18]`).
