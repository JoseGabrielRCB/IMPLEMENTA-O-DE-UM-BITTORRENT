# Guia da interface gráfica, testes e demonstração do Aluno 2

Este guia explica como usar a interface gráfica do harness distribuído, traz uma bateria de testes passo a passo com o resultado esperado e, na Parte C, um roteiro só para o **Aluno 2** demonstrar Chord, Gossip, Bully, SMR, 2PC e IST.

> **O que a interface é e o que não é.** A interface (`script/bittorrent-project/tests/cluster/gui/`) é uma **ferramenta de operação e visualização** do harness. Ela não tem lógica do sistema: os botões chamam o `cluster.sh`/`run.sh`, e os gráficos leem os logs que os Super Peers escrevem. O enunciado deixa interface gráfica **fora do escopo** do sistema (Seção 1.3), então ela não é apresentada como requisito cumprido. Tudo o que ela mostra também pode ser feito pelo terminal (indicado em cada teste).

---

## Parte A — Como a interface funciona

### A.1 Pré-requisitos (só na máquina de quem opera)

```bash
sudo apt install -y python3-tk python3-venv python3-pip   # uma vez
```

- Tela gráfica: no Windows/WSL o WSLg já resolve; por ssh, use `ssh -X`.
- Na primeira execução o `gui/run.sh` cria `gui/.venv` e instala o matplotlib (precisa de internet uma vez).
- As outras máquinas do cluster não precisam de nada disso.

### A.2 Configurar o cluster

A interface usa o mesmo `cluster.conf` do harness (`script/bittorrent-project/tests/cluster/cluster.conf`).

**Para ensaiar numa máquina só** (tudo local, sem ssh):

```text
set sp_per_host      4
set clients_per_host 2
set sp_port_base     5100
host m1 local dir=/tmp/bt-local
```

**No laboratório**, uma linha `host` por máquina — o passo a passo completo está na **Parte E**. Resumo, pelo terminal:

```bash
cd script/bittorrent-project/tests/cluster
./cluster.sh copy-keys    # só no laboratório
./cluster.sh check        # confere ssh, compilador, bibliotecas, portas
./cluster.sh deploy       # copia o código, compila e gera os arquivos de teste
```

(Também dá para fazer `check` e `deploy` pela aba **Cluster**.)

### A.3 Abrir

```bash
cd script/bittorrent-project/tests/cluster
./gui/run.sh
```

Para usar outro arquivo de configuração: `CLUSTER_CONF=/caminho/do/arquivo ./gui/run.sh`. O caminho em uso aparece no topo da janela, junto com o número de máquinas e de Super Peers.

> Fechar a janela **não** derruba o cluster. Para desligar, use **Cluster → down** (ou `./cluster.sh down`).

### A.4 As cinco abas

#### Cliente — fazer o papel de um usuário

| Elemento | O que faz |
|---|---|
| **Cliente** + **Atualizar lista** | Lista os clientes abertos (C1, C2…) e mostra em que máquina estão e por qual Super Peer entram. Clique em *Atualizar lista* sempre que abrir ou fechar clientes. |
| **Upload** + **Enviar** | Escolhe um arquivo de teste (`documento_a.pdf` … `documento_e.pdf`, `documento_big.pdf` de 9 MiB) e envia pelo cliente escolhido. |
| **Download** + **Baixar** | Baixa pelo nome. A saída fica em `run/<cliente>/` na máquina do cliente (vazia = `<nome>.out`). |
| **Lookup** + **Buscar** | Pelo **nome**: mostra o metadado completo (13 campos) e exercita o cache LFU. Pelo **ObjectID** (64 hex): mostra o caminho no anel Chord até o dono. |
| **Comando** | Qualquer comando da sessão do cliente, por exemplo `delete documento_c.pdf` (remoção lógica). `help` lista todos. |
| **state (topologia), ping, join, leave, help** | Atalhos. `state` mostra sucessor, predecessor e finger table do Super Peer de entrada. |

Cada comando aparece na área de texto com a saída e termina com `[done N rc=R]`: `rc=0` é sucesso; `rc=1` é falha da operação (arquivo não encontrado, sem conexão, SHA-256 divergente); `rc=2` é comando inválido.

#### Cluster — ligar, desligar e injetar falhas

| Grupo | Botões |
|---|---|
| **Preparo** | `list` (topologia), `check`, `net` (cada máquina alcança cada Super Peer?), `deploy`, `fixtures`, `clean` (apaga logs e estado) |
| **Execução** | `up`, **`up + clientes`**, `down`, `status`, `abrir clientes`, `fechar clientes` |
| **Falhas em Super Peer** | Escolha o SP e clique em **`kill -9`** (queda brusca), `stop` (encerramento normal), **`start`** (religa) ou `restart` |
| Lateral | `Parar` (interrompe o comando em andamento), `Recarregar conf`, `Limpar` |

> **Cuidados:** clique em **`up + clientes` uma vez só**: cada clique zera e reinicia o cluster inteiro (aparece `zerando o estado anterior`). E **`abrir clientes`** tenta abrir todos; se alguns já estiverem abertos, ele para com erro. Para abrir só os novos, use o terminal: `./cluster.sh client start C3 C4`.

#### Métricas — o estado do sistema em gráficos

Atualiza sozinha a cada 5 s (dá para mudar ou desligar). *Consultar processos vivos (ssh)* faz um `cluster.sh status` em cada atualização; desligado, os gráficos usam só os logs.

| Gráfico | Como ler |
|---|---|
| **Anel Chord** | Cada bolinha é um Super Peer na posição do seu NodeID (0 no topo, sentido horário). A seta azul vai para o **sucessor** que o nó reporta. Verde = vivo; contorno cinza tracejado = fora do ar. O **anel preto em volta** marca o **coordenador**. |
| **Membership** | Linha = quem observa; coluna = membro. Cada célula diz o que aquele nó sabe do outro pelo Gossip: `ALIVE` (verde), `SUSPECT` (amarelo), `FAILED` (vermelho), `REMOVED` (vinho), `eu`, ou `-` (nunca viu). |
| **LogIndex do SMR** | Último `LOG #n` aplicado por Super Peer e quantos objetos ativos ele tem. Todos iguais = consistentes. Aparece *divergência* se os vivos diferirem. Barra cinza = nó fora. |
| **Cache LFU** | Hits e misses acumulados por Super Peer. |
| **Linha do tempo** | Barra azul = período em que cada processo esteve no ar (um buraco = ficou fora). Marcas: ▼ SUSPECT, ✕ FAILED, ★ venceu a eleição, ◆ IST concluído. |

Abaixo dos gráficos, **Inspeção em texto** roda os comandos do harness: `overview`, `members`, `chord + fingers`, `coordinator`, `state + arquivos` e `metadata`.

#### Cenários — testes automáticos

Escolha `smoke`, `final` ou `carga` e clique em **Rodar**. A saída aparece à esquerda; ao terminar, o relatório (`out/relatorio.md`, com fases e métricas de tempo) aparece à direita. *Deixar o cluster de pé* (`CLUSTER_KEEP`) mantém tudo no ar no fim, para olhar os gráficos. `WAVE` controla quantos clientes agem ao mesmo tempo no `carga`.

| Cenário | Duração (4 SPs, local) | O que cobre |
|---|---|---|
| `smoke` | ~30 s | sobe, um upload, um download, consistência do LogIndex |
| `final` | ~2 min | **uma fase por item do CP6**: Bully/Chord, SMR/2PC, lookup por ObjectID, Gossip e detecção, 2PC com um nó fora, IST, LFU, **remoção lógica**, download, chunks/LZ4, réplica |
| `carga` | depende dos clientes | todos os clientes fazendo upload e download ao mesmo tempo |

#### Logs — o que cada processo está escrevendo

Escolha o processo (SP ou cliente) e o **filtro**: `tudo`, `sem ruído` (esconde heartbeats), `membership`, `eleição`, `chord`, `2pc`, `ist`, `lfu`, `log SMR`, `chunks`, `cliente`, ou digite uma expressão regular e Enter. *Acompanhar* mostra as linhas novas em tempo real. Quando o processo é religado, aparece `--- nova vida ---`.

---

## Parte B — Bateria de testes pela interface

Faça na ordem; cada teste parte do anterior. Coluna "Terminal" = o mesmo pelo `cluster.sh`.

### T1 — Subir o cluster

1. **Cluster → `up + clientes`** (uma vez).
2. Aguarde ~10 s e abra **Métricas**.

**Esperado:** a saída termina com `rc=0`; o anel mostra todos os Super Peers verdes com setas fechando o círculo; um deles tem o anel preto (o de **maior NodeID**); a matriz de membership toda `ALIVE`; LogIndex 0.
Terminal: `./cluster.sh up --clients` e `./cluster.sh overview`.

### T2 — Upload e download entre clientes diferentes

1. **Cliente → Atualizar lista**, escolha **C1**, Upload `documento_a.pdf` → **Enviar**.
2. Escolha **C2**, Download `documento_a.pdf` → **Baixar**.

**Esperado:** no upload, `ObjectID`, `Chunks: 1`, `Compression: LZ4`, `rc=0`; no download, `Transfer state … TRANSFERRING`, `Download completed`, **`SHA-256 verified`**, `rc=0`. Em **Métricas**, o LogIndex de todos vai para **#1 · 1 obj**.
Terminal: `./cluster.sh client do C1 upload ../files/documento_a.pdf` e `./cluster.sh client do C2 download documento_a.pdf a.out`.

### T3 — Metadado completo e cache LFU

1. C1 → Lookup `documento_a.pdf` → **Buscar**, duas vezes.

**Esperado:** além de File/Size/ObjectID/Chunks, aparecem `Version`, `Status: ACTIVE`, `Compression`, `OwnerPeer`, `UploadDate`, `LastAccess`, `DownloadCounter` e `ReplicaPeers: N (...)`. Em **Logs** (Super Peer de entrada do C1, filtro `lfu`): `LFU CACHE MISS` na primeira e `LFU CACHE HIT … frequency=2` na segunda (se o Super Peer já tinha o arquivo em cache por ser o coordenador, as duas são HIT com frequência crescente).

### T4 — Chunks e compressão

1. C2 → Upload `documento_big.pdf` (9 MiB) → **Enviar**.

**Esperado:** `Chunks: 3` (4 MiB + 4 MiB + 1 MiB), `Compression: LZ4`, três linhas `Chunk n/3 enviado ao Super Peer … e replicado`. Filtro `chunks` num Super Peer: `CHUNK_STORE: chunk … com N copias`.

### T5 — Remoção lógica

1. C1 → **Comando**: `delete documento_a.pdf` → **Enviar**.
2. C1 → Lookup `documento_a.pdf`.
3. C1 → Upload `documento_a.pdf` de novo, e Lookup.

**Esperado:** (1) `Documento … marcado como REMOVED (remocao logica)`, `rc=0`; em todos os Super Peers (filtro `log SMR`): `LOG #n` + `DELETE …documento_a.pdf`; o gráfico de objetos diminui. (2) `nao encontrado`, `rc=1`. (3) volta a aparecer com `Version` maior (`STORE: arquivo … reativado`).

### T6 — Queda de um Super Peer comum (réplica e Gossip)

1. **Cluster → Falhas**: escolha um SP que **não** seja o coordenador → **kill -9**.
2. Imediatamente, C2 → Download `documento_big.pdf`.
3. Observe **Métricas** por ~50 s.

**Esperado:** o download termina com `SHA-256 verified` (troca de réplica se precisar: `selecionando proxima replica`). Na matriz, o nó derrubado vira `SUSPECT` em ~15 s, `FAILED` em ~30 s e `REMOVED` em ~45 s na visão dos outros; no anel ele fica tracejado e as setas se refazem sem ele.

### T7 — Uploads com um nó fora e volta pelo IST

1. Com o nó do T6 ainda fora, C1 → Upload `documento_d.pdf` e `documento_e.pdf`.
2. **Cluster → start** no nó derrubado.

**Esperado:** os uploads funcionam com um participante a menos; ao religar, **Logs** (nó religado, filtro `ist`): `IST: SYNCHRONIZING (local version=0, coordinator version=N, versions missing=N)`, `IST: receive logs (1 .. N)`, `IST: READY`. Na linha do tempo aparece o ◆; o LogIndex dele alcança os outros.

### T8 — Queda do coordenador e eleição

1. Veja quem tem o anel preto (ou **coordinator** na inspeção).
2. **Cluster → kill -9** nesse Super Peer.
3. Aguarde ~20 s.

**Esperado:** um novo coordenador — o **maior NodeID entre os vivos** — com ★ na linha do tempo; **coordinator** diz `todos os vivos reconhecem SPx`. Um upload em seguida funciona.

### T9 — O antigo coordenador volta (Seção 15.7)

1. **Cluster → start** no coordenador derrubado no T8.

**Esperado:** ele **não** retoma a coordenação: filtro `eleição` no nó religado mostra `COORDINATOR <atual>` e `Bully: coordenador … ja conhecido e sistema em operacao (LOG #n): entra como FOLLOWER, sem eleicao de partida (Secao 15.7)`. O comando **coordinator** explica: *o maior NodeID vivo é SPx: ele voltou com o sistema em operação e entrou como FOLLOWER*.

### T10 — Cenário final automático

1. **Cenários** → `final` → **Rodar** (o cenário derruba e sobe o cluster sozinho).

**Esperado:** 11 fases `PASS` e o relatório com as métricas (eleição, detecção ~15 s, IST < 2 s, reeleição). Para acompanhar ao vivo, deixe **Métricas** aberta numa segunda olhada.

### T11 — Desligar

**Cluster → down**. Esperado: `tudo encerrado`; no anel todos ficam tracejados.

### Baterias do repositório (terminal)

```bash
make && make -C tests/c1 && ./tests/c1/test_protocol   # C1: 5/5
bash tests/run.sh                  # C2: 8/8
bash tests/c3/run.sh               # 5/5
bash tests/c4/run.sh               # 4/4
bash tests/c5/run.sh               # 5/5
bash tests/c6/run.sh               # 29/29 (ou 28/29: ver docs/cp6, seção 8.4)
```

---

## Parte C — Demonstração do Aluno 2: Chord, Gossip, Bully, SMR, 2PC e IST

Roteiro de ~10 minutos, na ordem em que um mecanismo leva ao outro. Prepare antes: cluster no ar com **4 Super Peers** e 2 clientes (T1), aba **Métricas** visível e uma segunda olhada na aba **Logs**.

> **Frase de abertura:** "Os Super Peers formam um anel Chord; o Gossip mantém a membership e detecta falhas; o Bully elege o coordenador; o coordenador ordena toda escrita pelo log SMR, confirmado em todos por 2PC; e quem volta de uma falha recebe só o que perdeu pelo IST."

### C.1 Chord (Seção 6; Checkpoint 3)

**Mostrar**
1. **Métricas → anel**: "cada Super Peer fica na posição do seu NodeID = SHA-256(IP‖Porta‖UUID); a seta é o sucessor".
2. **Inspeção → `chord + fingers`**: tabela com sucessor e predecessor de cada nó contra o esperado (`ok`) e a finger table ao vivo.
3. **Cliente**: faça um upload, copie o ObjectID e faça **Lookup** com ele.

**Comprova:** a saída do lookup no formato do Checkpoint 3:
```text
Lookup path:
SP2
 ↓
SP3
 ↓
SP1
Owner: SP1
```
**Logs (filtro `chord`)**: `Successor: SPx (ip:porta)`, `Predecessor: …`.

**Perguntas prováveis**
- *Por que a finger table não tem 256 entradas preenchidas?* — Com poucos nós, as fingers repetem o mesmo destino; guardamos só as entradas úteis. A comparação de IDs é `memcmp` sobre 32 bytes em big-endian (Seção 6.2).
- *Como o lookup acha o dono?* — Vai pela finger mais próxima que precede o ObjectID até chegar ao sucessor do ObjectID, que é o dono (Seção 6.5).

### C.2 Gossip e detecção de falha (Seção 7; Checkpoint 3)

**Mostrar**
1. **Métricas → Membership**: tudo `ALIVE`.
2. **Cluster → kill -9** num Super Peer que **não** é o coordenador.
3. Cronometre na matriz: `SUSPECT` (~15 s) → `FAILED` (~30 s) → `REMOVED` (~45 s).
4. **Cluster → start** no mesmo nó.

**Comprova (filtro `membership`)**: `SUSPECT SP3 (ip:porta): sem heartbeat ha 16 s`, `FAILED SP3 …`, `REMOVED SP3 …` e, na volta, `Gossip: SP3 voltou (FAILED -> ALIVE)`.

**Perguntas prováveis**
- *Qual o heartbeat e o timeout?* — Heartbeat a cada 5 s, timeout de 15 s (Seção 5.3). Sem heartbeat por 15 s → SUSPECT; 30 s → FAILED; 45 s → REMOVED.
- *Como um nó religado não é confundido com o antigo?* — O contador de heartbeat começa no horário de início do processo (número de encarnação), então é sempre maior que o último visto.
- *O Gossip propaga metadados?* — Não. Só membership e falhas; metadados vão pelo SMR + 2PC (decisão `[C-07]`, consistência forte na escrita).

### C.3 Bully (Seção 15; Checkpoint 4)

**Mostrar**
1. Aponte o coordenador (anel preto) e rode **Inspeção → `coordinator`**: "o maior NodeID vence".
2. **Cluster → kill -9 no coordenador.**
3. Em ~16–17 s o novo coordenador aparece (★ na linha do tempo).
4. **Cluster → start** no antigo coordenador (Seção 15.7).

**Comprova (filtro `eleição`)**, no nó que detectou:
```text
detect failure: SP1 (coordenador)
start election
ELECTION -> SP2        (se houver nó maior vivo)
OK <- SP2
highest NodeID wins: SP2
COORDINATOR SP2
resume operations (proximo LOG #n)
```
Na volta do antigo coordenador: `… entra como FOLLOWER, sem eleicao de partida (Secao 15.7)`.

**Perguntas prováveis**
- *Por que não aparece `ELECTION ->` às vezes?* — Se quem percebe a queda primeiro já é o maior nó vivo, não há a quem mandar ELECTION: ele vence direto. As duas sequências são Bully correto (docs/cp6, seção 8.4).
- *O RNF-010 pede eleição < 5 s, mas levou ~17 s.* — Os 15 s são a detecção (timeout do heartbeat); a eleição em si, do `start election` ao `COORDINATOR`, leva milissegundos. Medimos a partir do ELECTION, como registrado em `[C-16]`.
- *E se o antigo coordenador, que tem NodeID maior, voltar?* — Com o sistema em operação ele entra como FOLLOWER, sincroniza pelo IST e só disputa outra eleição se houver nova falha (Seção 15.7). Na montagem do cluster, sem operações, vale o Bully clássico: o maior NodeID assume (Checkpoint 4).
- *Quando a eleição começa: SUSPECT ou FAILED?* — Em SUSPECT, pela Seção 5.3; está registrado como decisão de projeto.

### C.4 SMR — log replicado (Seção 12; Checkpoint 4)

**Mostrar**
1. **Cliente**: uploads de `documento_a.pdf` e `documento_b.pdf`.
2. **Métricas → LogIndex**: todas as barras iguais (`#2 · 2 obj`).
3. **Inspeção → `state + arquivos`**: matriz arquivo × Super Peer, mesma versão em todos.
4. **Comando** `delete documento_b.pdf`: a operação `DELETE` também entra no log.

**Comprova (filtro `log SMR`)**, igual em todos os Super Peers:
```text
LOG #1
PUT …/documento_a.pdf
TransactionID: …  Version: 1  Checksum: …
LOG #2
PUT …/documento_b.pdf
LOG #3
DELETE …/documento_b.pdf
```
Compare **metadata SP1 / SP2 / SP3** (Checkpoint 4): `state` mostra o mesmo LogIndex e os mesmos objetos.

**Perguntas prováveis**
- *O que tem numa entrada do log?* — LogIndex, TransactionID (128 bits: timestamp 48 ‖ 64 bits altos do NodeID ‖ sequência 16, `[C-02]`), operação (PUT/DELETE), ObjectID, timestamp lógico, versão e checksum SHA-256 do comando (Seção 12.3).
- *Quem escreve no log?* — Só o coordenador cria entradas; os outros só aceitam do coordenador atual e na ordem (LogIndex seguinte).

### C.5 2PC (Seção 14; Checkpoint 5)

**Mostrar o caminho feliz** — faça um upload e abra **Logs** do coordenador com o filtro `2pc`:
```text
2PC [IDLE -> PREPARING] LOG #n TransactionID=… (3 participantes)
2PC PREPARE -> SP2
2PC YES <- SP2
…
2PC [READY -> COMMIT] LOG #n confirmado
2PC COMMIT -> SP2
…
2PC [COMMIT -> DONE] LOG #n
```
Num participante: `2PC PREPARE #n -> YES (estado READY)` e `2PC COMMIT #n recebido -> aplicando operacao`. Com um nó fora (T7), o número de participantes cai.

**Mostrar a falha no PREPARE** (pedido do Checkpoint 5). O harness não liga as variáveis de simulação, então faça em 3 terminais na raiz do repositório (o `c6.conf` usa `127.0.0.1`; com os `.uuid` do repositório o coordenador é o **SP1**):

```bash
# terminais 1 e 3: Super Peers normais
./bin/superpeer --config tests/config/c6.conf --port 5101
./bin/superpeer --config tests/config/c6.conf --port 5103
# terminal 2: este participante vota NO em todo PREPARE
SIMULATE_VOTE_NO=1 ./bin/superpeer --config tests/config/c6.conf --port 5102
# terminal 4 (depois de ~10 s): um upload
./bin/peer --cmd upload --file tests/data/documento_a.pdf --port 5101
```
**Comprova:** no coordenador, `2PC YES <- SP3`, `2PC NO <- SP2`, `2PC [PREPARING -> ABORT] LOG #1 abortado` e `2PC ABORT -> …`; no SP2, `2PC PREPARE recusado: voto NO simulado (ABORT)` e `2PC ABORT recebido -> estado limpo (IDLE)`; no SP3, que tinha votado YES, `2PC ABORT #1 recebido -> descartando transacao preparada`. **Nenhum** `LOG #1` é aplicado e o cliente recebe `Erro: super peer respondeu ERROR ao STORE` (rc=1). A alternativa `SIMULATE_PREPARE_FAIL=1` no **coordenador** simula a falha durante a fase PREPARE, antes de enviar os pedidos; é essa forma que o `tests/c6/run.sh` usa.

**Perguntas prováveis**
- *E se o coordenador cair depois do PREPARE?* — Os participantes ficam em READY até haver novo coordenador: o 2PC é bloqueante. Implementamos como especificado e registramos a limitação (`[C-18]`).
- *Por que 2PC além do SMR?* — O SMR dá a ordem; o 2PC garante que ou todos aplicam a entrada, ou ninguém aplica.

### C.6 IST (Seção 13; Checkpoint 5)

**Mostrar** — o roteiro do Checkpoint 5:
1. Uploads A, B e C (C1).
2. **Cluster → kill -9 no SP2** (que não seja o coordenador).
3. Uploads D e E.
4. **Cluster → start no SP2**.

**Comprova (Logs do SP2, filtro `ist`)**:
```text
IST: SYNCHRONIZING (local version=0, coordinator version=5, versions missing=5)
IST: receive logs (1 .. 5) de SP1
IST: READY (sincronizado ate LOG #5)
```
Na linha do tempo: buraco na barra do SP2 e ◆ na volta; no gráfico de LogIndex ele alcança **#5**. Uma remoção feita enquanto ele estava fora também chega (`DELETE` dentro do IST).

**Perguntas prováveis**
- *Por que o nó religado recebe 1..5 e não só 4..5?* — O log é mantido em memória; o processo religado começa com versão 0 e recebe tudo o que falta. O mecanismo é o mesmo do exemplo da Seção 13.3 (só as versões que faltam); é rápido (~40 ms para 5 entradas, RNF-008 < 2 s).
- *E snapshot/checkpoint?* — Não implementado; está nas limitações (docs/cp6, seção 8.6).
- *O nó religado recebe os chunks?* — Recebe os metadados pelo IST; os chunks continuam nas outras réplicas, e o cliente baixa de lá (Seção 11.6).

### C.7 Fechamento

Rode **Cenários → `final`** (2 minutos) para mostrar tudo de uma vez com verificação automática, e termine com o relatório aberto. Ou, pelo terminal, `bash tests/c6/run.sh`.

---

## Parte D — Problemas comuns

| Sintoma | Causa e solução |
|---|---|
| A janela não abre: `sem tela gráfica` | Sem DISPLAY. No WSL, atualize o WSL (WSLg); por ssh, `ssh -X`. Ou use o terminal (`./run.sh final`, `./watch.sh final`). |
| `falta o tkinter` | `sudo apt install python3-tk` |
| Gráficos vazios | O cluster não está no ar (**Cluster → up + clientes**) ou ainda não houve atualização (clique em **Atualizar**). |
| Lista de clientes vazia | Clique em **Atualizar lista**; abra os clientes (**up + clientes** ou `./cluster.sh client start`). |
| `abrir clientes` falha com "já está rodando" | Alguns já estavam abertos; abra só os novos pelo terminal: `./cluster.sh client start C3`. |
| Upload logo depois de um `kill -9` falha | O 2PC ainda conta com o nó caído até a detecção (~15 s). Espere o SUSPECT. |
| O coordenador mudou depois de um `up` | Cada `up` apaga os `.uuid` e gera NodeIDs novos. Para reiniciar um nó mantendo o NodeID, use **start/restart**, nunca um `up` novo. |
| `tests/c6` deu 28/29 em "ELECTION enviada…" | Corrida de detecção, não erro do sistema (docs/cp6, seção 8.4). Rode de novo. |

---

## Parte E — Passo a passo no laboratório (várias máquinas)

Tempo estimado na primeira vez: 20–30 minutos. Depois, só os passos E.6 em diante.

### E.1 Como as peças se encaixam

```text
             máquina do OPERADOR (você)                    máquinas do CLUSTER
   ┌──────────────────────────────────────────┐        ┌──────────────────────┐
   │ repositório + cluster.sh + interface      │  ssh   │ m1: SP1 SP2 SP3 C1 C2│
   │ logs espelhados em cluster/logs/          │ ─────► │ m2: SP4 SP5 SP6 C3 C4│
   │ (pode ser também uma das máquinas)        │        │ m3: ...              │
   └──────────────────────────────────────────┘        └──────────────────────┘
```

- Você só trabalha na **máquina do operador**. O `deploy` copia o código para as outras por ssh e compila lá; ninguém precisa abrir terminal nas outras máquinas.
- A máquina do operador pode ser um PC do laboratório ou um notebook **na mesma rede**, e pode também rodar Super Peers.

### E.2 Antes de ir ao laboratório

1. Envie tudo ao GitHub (`git push origin checkpoint-6`), ou leve a pasta num pendrive.
2. Se possível, ensaie com **duas** máquinas quaisquer da mesma rede (seu PC e um notebook): pega problemas de ssh e firewall antes do dia.
3. Combine com o técnico, se ninguém tiver `sudo`: as máquinas precisam de `gcc`, `make`, `libssl-dev` e `liblz4-dev`, do **ssh ligado** e das **portas 5101 em diante liberadas** entre elas.

### E.3 Escolher as máquinas e anotar IP e usuário

Em **cada** máquina que vai participar (inclusive a do operador, se ela for rodar Super Peers):

```bash
hostname -I     # o IP da rede, ex.: 192.168.0.21 (ignore 127.x e 172.17.x)
whoami          # o usuário, ex.: aluno
```

Monte uma tabela no papel: `m1 = aluno@192.168.0.21`, `m2 = aluno@192.168.0.22`, …

### E.4 Preparar a máquina do operador

```bash
git clone https://github.com/JoseGabrielRCB/IMPLEMENTA-O-DE-UM-BITTORRENT.git
cd IMPLEMENTA-O-DE-UM-BITTORRENT
git checkout checkpoint-6
make                                   # confere que compila aqui também
sudo apt install -y sshpass tmux       # copy-keys e watch.sh (se tiver sudo)
sudo apt install -y python3-tk python3-venv python3-pip   # só para a interface
```

Teste o ssh na mão para uma das máquinas: `ssh aluno@192.168.0.22 hostname`. Se pedir senha e responder o nome da máquina, está ok. Se der *Connection refused*, o ssh está desligado nela (fale com o técnico).

### E.5 Escrever o `cluster.conf`

```bash
cd script/bittorrent-project/tests/cluster
cp cluster.conf.example cluster.conf
nano cluster.conf
```

Exemplo com 4 máquinas, 1 Super Peer e 2 clientes em cada (4 SPs no total, como nos Checkpoints 4 e 5):

```text
set sp_per_host      1
set clients_per_host 2
set sp_port_base     5100
host m1  aluno@192.168.0.21  dir=/tmp/bt-m1
host m2  aluno@192.168.0.22  dir=/tmp/bt-m2
host m3  aluno@192.168.0.23  dir=/tmp/bt-m3
host m4  aluno@192.168.0.24  dir=/tmp/bt-m4
```

Regras que evitam os erros mais comuns:

- **Use sempre o IP**, não o nome da máquina (o nome pode resolver para `127.0.1.1`).
- **`dir=` com uma pasta local diferente por máquina** (`/tmp/bt-mN`): se a pasta pessoal for compartilhada entre os PCs (NFS), sem isso todas gravariam no mesmo lugar.
- Se a máquina do operador também roda Super Peers, use `host m1 local ip=<IP dela> dir=/tmp/bt-m1` — **o `ip=` é obrigatório** (sem ele seria 127.0.0.1, e o `list` recusa).
- Para 5 Super Peers como no Checkpoint 3, use `sps=2` numa das linhas, ou `set sp_per_host 2` com menos máquinas.

Confira a topologia gerada (não usa a rede):

```bash
./cluster.sh list
```

Deve listar as máquinas com os IPs corretos e os Super Peers SP1…SPn com `ip:porta`.

### E.6 Chaves ssh (uma vez por máquina do operador)

```bash
./cluster.sh copy-keys          # pede a senha do laboratório uma vez
./cluster.sh copy-keys --per-host   # se cada máquina tiver uma senha diferente
```

Ele cria `~/.ssh/id_ed25519` se não existir e instala a chave nas máquinas em que ela ainda não funciona. A senha não é gravada. Depois disso, nenhum comando pede senha.

### E.7 Diagnóstico

```bash
./cluster.sh check
```

Cada máquina deve terminar com `ok (ssh, ferramentas, bibliotecas e cabeçalhos, portas livres)`. Como ler os avisos:

| Mensagem | O que fazer |
|---|---|
| `ssh falhou em …` | Rode `copy-keys`; confira IP e usuário no `cluster.conf`. |
| `falta: gcc` / `make` | Instalar `build-essential` (técnico). |
| `falta: lz4.h(pacote -dev)` / `falta: openssl/sha.h(pacote -dev)` | Instalar `liblz4-dev` / `libssl-dev`. |
| `porta 5101 já está em uso` | Outro processo usa a porta (outro grupo?). Mude `set sp_port_base` (ex.: 6100). |
| `AVISO: relógio com mais de 2 s de diferença` | Não impede o funcionamento (os logs são carimbados na máquina do operador); só registre. |

### E.8 Enviar o código e compilar

```bash
./cluster.sh deploy
```

Para cada máquina: `código enviado`, `compilado`, `arquivos de teste enviados`. Se a compilação falhar numa máquina, a mensagem do `gcc` aparece com o nome dela (quase sempre biblioteca faltando — volte ao E.7).

### E.9 Rede entre as máquinas

```bash
./cluster.sh up        # sobe só os Super Peers
./cluster.sh net       # cada máquina tenta abrir conexão com cada Super Peer
```

Esperado: `alcança os N Super Peers` em todas. Se aparecer `sem alcance: SP3 192.168.0.23:5103`, há firewall bloqueando aquela porta — peça para liberar as portas `sp_port_base+1` em diante, ou teste outra base de portas.

### E.10 Teste rápido entre máquinas

```bash
./run.sh smoke
```

Faz upload num cliente e download num cliente de **outra máquina**, conferindo o `sha256`, e verifica o `LOG #1` em todos. Termina com `FAIL: 0` e grava `out/relatorio.md`. Se passou, o laboratório está pronto.

### E.11 Usar

- **Interface:** `./gui/run.sh` → **Cluster → up + clientes** e siga as Partes B e C. Num cluster de várias máquinas, o `kill -9` derruba um processo em outro PC, e o download pode vir de outra máquina.
- **Terminal, com painéis:** `./watch.sh final` (precisa de `tmux`) mostra o cenário final num painel e o log de cada Super Peer em outro.
- **Só o roteiro automático:** `./run.sh final`.
- **Baterias do professor/repositório** (em uma máquina só, como foram escritas): `bash tests/c6/run.sh` na raiz do repositório.

### E.12 Ao terminar

```bash
./cluster.sh down      # fecha clientes e Super Peers em todas as máquinas
./cluster.sh clean --data   # opcional: apaga logs locais e as pastas run/ remotas
```

Os logs da execução ficam em `cluster/logs/` (um arquivo por processo e por vida) e o relatório em `cluster/out/relatorio.md` — bons para anexar à documentação.

### E.13 Problemas típicos de laboratório

| Situação | Solução |
|---|---|
| Sem `sudo` para instalar nada | As máquinas do cluster precisam só de `gcc`, `make`, `libssl-dev`, `liblz4-dev` (peça ao técnico). Sem `sshpass`, faça `ssh-copy-id aluno@IP` à mão para cada máquina. Sem `python3-tk`, use o terminal (`run.sh`, `watch.sh`). |
| `list` recusa com "IP … (loopback)" | Defina `ip=<IP da rede>` na linha daquele host (E.5). |
| Funciona numa máquina só, mas não entre máquinas | Rode `./cluster.sh net`: é firewall ou IP errado no `cluster.conf`. |
| A home é compartilhada (NFS) | Use `dir=/tmp/bt-mN` diferente em cada linha `host`. |
| Outro grupo usando as mesmas máquinas | Mudem as portas (`set sp_port_base 6100`) e as pastas (`dir=`). |
| Uma máquina reiniciou no meio | `./cluster.sh status` mostra quem caiu; `./cluster.sh start SPx` religa (o NodeID é mantido). Se `/tmp` foi apagado, rode `deploy` de novo. |
