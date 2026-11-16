# Roteiro de Execução Manual — Checkpoint 5

Este documento apresenta o guia completo para inicializar, testar e verificar manualmente todas as funcionalidades do **Checkpoint 5** (Cache LFU, Transferência Paralela, Two-Phase Commit e Incremental State Transfer).

---

## 1. Objetivos do Checkpoint 5 (O que está acontecendo?)

1. **Two-Phase Commit (2PC) — Atomicidade nas Escritas:**
   * Toda inserção de metadados (`upload`) é coordenada pelo líder da eleição Bully.
   * **Fase 1 (`PREPARE`):** O líder envia `MSG_PREPARE` para todos os SuperPeers vivos. Cada participante valida o cabeçalho, os chunks e o SHA-256 e responde `MSG_VOTE_YES` (ou `MSG_VOTE_NO` se houver divergência).
   * **Fase 2 (`COMMIT` ou `ABORT`):** Se todos votarem `YES`, o líder confirma a operação com `MSG_COMMIT` e todos gravam no log SMR. Se algum nó falhar ou votar `NO`, o líder envia `MSG_ABORT` e ninguém aplica a mudança.
2. **Cache LFU (Least Frequently Used) — Desempenho nas Consultas:**
   * Cada SuperPeer armazena os metadados mais populares em memória RAM (capacidade para 8 arquivos).
   * **1ª busca (`LOOKUP`):** Ocorre `LFU CACHE MISS` $\to$ o nó lê a tabela local e insere na cache (`LFU CACHE INSERT` com `frequency=1`).
   * **2ª busca em diante:** Ocorre `LFU CACHE HIT` $\to$ os metadados são devolvidos instantaneamente da memória e a frequência é incrementada (`frequency=2, 3...`).
3. **Incremental State Transfer (IST) — Recuperação Rápida pós-Falha:**
   * Quando um SuperPeer reinicia após ter caído (ou entra atrasado), ele não transfere todo o banco de dados.
   * Ele consulta o coordenador via `MSG_STATE_TRANSFER` informando qual foi seu último `log_index` aplicado.
   * O coordenador envia **apenas o delta de logs perdidos** (`versions missing`), permitindo que o nó transite de `SYNCHRONIZING` para `READY` consumindo o mínimo de banda de rede.
4. **Transferência Paralela de Chunks (Rede + LZ4 + SHA-256):**
   * Ao baixar um arquivo (`download`), o cliente dispara **uma thread independente (`pthread_create`) para cada pedaço de 4 MB**.
   * Cada thread requisita seu bloco comprimido (`MSG_DOWNLOAD_REQ` $\to$ `MSG_DOWNLOAD_REP`), valida o hash SHA-256 do chunk, descomprime via LZ4 e remonta o arquivo conferindo o SHA-256 global contra o `ObjectID`.

---

## 2. Preparação do Ambiente

Recomenda-se abrir **4 janelas/abas de terminal** no WSL/Linux:

* **Terminal 1:** SuperPeer 1 (`SP1` — Porta `5101`)
* **Terminal 2:** SuperPeer 2 (`SP2` — Porta `5102`)
* **Terminal 3:** SuperPeer 3 (`SP3` — Porta `5103`)
* **Terminal 4:** Cliente/Peer (para envio dos comandos de teste)

Em todos os terminais, navegue para o diretório raiz do projeto:
```bash
cd /mnt/c/Arquivos/Faculdade/4_Year/PD/Trabalho
```

---

## 3. Passo a Passo da Execução Manual

### Passo 0: Compilação e Fixtures de Teste
No **Terminal 4**, compile os binários e crie os arquivos de teste:
```bash
make clean && make
./tests/generate_fixtures.sh
```

---

### Passo 1: Inicializar os SuperPeers
Inicie um SuperPeer em cada terminal:

* **No Terminal 1:**
  ```bash
  ./bin/superpeer --config tests/config/c5.conf --port 5101
  ```
* **No Terminal 2:**
  ```bash
  ./bin/superpeer --config tests/config/c5.conf --port 5102
  ```
* **No Terminal 3:**
  ```bash
  ./bin/superpeer --config tests/config/c5.conf --port 5103
  ```

**O que verificar nos logs:**
* Os nós montam o anel Chord e trocam mensagens de Gossip.
* Em **~5 segundos**, o algoritmo **Bully** roda e o nó de maior ID anuncia:
  ```text
  highest NodeID wins: SP...
  COORDINATOR SP...
  resume operations (proximo LOG #1)
  ```

---

### Passo 2: Testar o Two-Phase Commit (2PC) no Upload
No **Terminal 4**, envie um arquivo:
```bash
./bin/client --cmd upload --file tests/data/documento_a.pdf --host 127.0.0.1 --port 5101
```

**O que verificar nos logs:**
* No terminal do **Coordenador**:
  ```text
  2PC [IDLE -> PREPARING] LOG #1 TransactionID=... (2 participantes)
  2PC PREPARE -> SP...
  2PC YES <- SP...
  2PC [READY -> COMMIT] LOG #1 confirmado
  LOG #1
  PUT .../documento_a.pdf
  2PC COMMIT -> SP...
  2PC [COMMIT -> DONE] LOG #1
  ```
* Nos terminais dos **Participantes**:
  ```text
  2PC PREPARE #1 -> YES (estado READY)
  2PC COMMIT #1 recebido -> aplicando operacao
  LOG #1
  ```

---

### Passo 3: Testar a Cache LFU (Lookups Repetidos)
No **Terminal 4**, realize a **primeira consulta** do arquivo:
```bash
./bin/client --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101
```

**O que verificar no Terminal 1 (SP1):**
```text
LFU CACHE MISS: documento_a.pdf (hits=0, misses=1)
LFU CACHE INSERT: ... documento_a.pdf (frequency=1, slot=0/8)
LOOKUP: arquivo documento_a.pdf encontrado (1 provedores)
```

Agora, execute a **segunda consulta** pelo mesmo arquivo:
```bash
./bin/client --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101
```

**O que verificar no Terminal 1 (SP1):**
```text
LFU CACHE HIT: documento_a.pdf (frequency=2, hits=1, misses=1)
LOOKUP: arquivo documento_a.pdf encontrado (1 provedores)
```
*(O metadado foi recuperado instantaneamente da memória RAM, incrementando a frequência para 2)*.

---

### Passo 4: Testar o Download Paralelo de Chunks
No **Terminal 4**, baixe o arquivo via rede:
```bash
./bin/client --cmd download --name documento_a.pdf --output /tmp/meu_download.pdf --host 127.0.0.1 --port 5101
```

**O que verificar nos logs:**
* As threads paralelas requisitam e recebem os blocos comprimidos:
  ```text
  DOWNLOAD_REP: chunk 0 enviado (315 bytes comprimidos)
  Transferencia paralela: chunk 1/1 recebido (DOWNLOAD_REP) e verificado (LZ4 + SHA-256)
  Download completed
  SHA-256 verified
  ```
* Verifique a integridade do arquivo gerado comparando bit a bit com o original:
  ```bash
  cmp tests/data/documento_a.pdf /tmp/meu_download.pdf && echo "Arquivos 100% identicos!"
  ```

---

### Passo 5: Testar o Incremental State Transfer (IST)
Para demonstrar a resiliência a falhas e a sincronização incremental:

1. **Derrubar um nó:** No **Terminal 2**, pressione `Ctrl + C` para finalizar o `SP2`.
2. **Cadastrar novos arquivos enquanto o SP2 está fora:** No **Terminal 4**, publique outro arquivo:
   ```bash
   ./bin/client --cmd upload --file tests/data/exact_4MiB.bin --host 127.0.0.1 --port 5101
   ```
   *(O 2PC confirmará entre os nós vivos `SP1` e `SP3`, avançando o log para `LOG #2`)*.
3. **Reiniciar o SP2:** No **Terminal 2**, suba o `SP2` novamente:
   ```bash
   ./bin/superpeer --config tests/config/c5.conf --port 5102
   ```

**O que verificar no Terminal 2 (SP2):**
* O `SP2` detecta que possui `local version=1`, enquanto o coordenador já está na `version=2`.
* Ele dispara automaticamente o IST via `MSG_STATE_TRANSFER`:
  ```text
  IST: SYNCHRONIZING (local version=1, coordinator version=2, versions missing=1)
  IST: receive logs (2 .. 2) de SP...
  LOG #2
  PUT .../exact_4MiB.bin
  IST: READY (sincronizado ate LOG #2)
  ```
*(O nó reiniciado recuperou exatamente a entrada pendente e voltou ao estado consistente sem retransferir o histórico inteiro)*.

---

## 4. Execução do Teste Automatizado Oficial

Para rodar a suíte oficial de testes do professor (que executa todo esse fluxo em background com assertions automáticas):
```bash
./tests/c5/run.sh
```

**Resultado esperado:**
```text
==== C5 — 2PC + IST + LFU ====
[PASS] Há fase PREPARE
[PASS] Há decisão COMMIT/ABORT
[PASS] Há evidência de LFU/cache
[PASS] Há evidência de IST/sincronização
[PASS] Sem crash/deadlock reportado em c5.log

==============================
PASS: 5
FAIL: 0
==============================
```
