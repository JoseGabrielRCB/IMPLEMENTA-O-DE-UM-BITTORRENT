# Harness distribuído (CP6)

Sobe Super Peers e clientes em várias máquinas por ssh, a partir de um arquivo
de configuração, e roda os cenários de demonstração do Checkpoint 6.

Todos os caminhos abaixo são relativos a esta pasta
(`script/bittorrent-project/tests/cluster/`). O harness envia às máquinas a raiz do
repositório (onde ficam o `Makefile` e os `.c`), compila lá e roda `bin/node` e
`bin/client`.

Os clientes **não morrem depois de um comando**. Cada um é um
`bin/client --interactive` que fica aberto até receber `quit`. Ele aceita comandos
em modo interativo ou em lote.

## Requisitos

- Na máquina do operador: `bash` 5, `ssh`, `tar`, `tmux` (só para o `watch`) e, para
  copiar as chaves, `ssh-copy-id` e `sshpass`.
- Em cada máquina: `gcc`, `make`, `stdbuf`, `setsid`, as bibliotecas `libcrypto` e
  `liblz4` **com os cabeçalhos** (pacotes `libssl-dev` e `liblz4-dev`). O `check`
  confere tudo isso. Linux x86-64, como pede a spec.
- As máquinas se enxergam na mesma rede, e as portas dos Super Peers estão liberadas.

## Primeiros passos

```bash
cd script/bittorrent-project/tests/cluster
cp cluster.conf.example cluster.conf   # ajuste hosts e usuários
./cluster.sh list          # confere a topologia gerada
./cluster.sh copy-keys     # copia a chave ssh (pede a senha, não grava)
./cluster.sh check         # ssh, compilador, bibliotecas, relógio, portas
./cluster.sh deploy        # envia o código, compila e envia os arquivos de teste
./run.sh smoke             # confere rede, ssh e roster
./watch.sh final           # cenário final com painéis em tmux
```

`copy-keys` gera `~/.ssh/id_ed25519` se não houver chave, pergunta a senha uma vez
(ou usa `$SSHPASS`) e roda `ssh-copy-id` nas máquinas em que a chave ainda não
funciona. Use `--per-host` se cada máquina tiver uma senha. A senha nunca vai para
arquivo.

Para ensaiar sem rede, use `host m1 local` (veja `cluster.conf.example`): tudo roda
nesta máquina, sem ssh.

## `cluster.conf`

Cada `host` gera sozinho os Super Peers e os clientes dele. Os padrões são
**3 Super Peers e 30 clientes por máquina**:

```text
set sp_per_host      3
set clients_per_host 30
set sp_port_base     5100        # SP k (global) usa a porta base+k
set client_port_base 9000
host m1  aluno@192.168.18.11
host m2  aluno@192.168.18.12
host m3  aluno@lab-pc3.local  ip=192.168.18.13  sps=2  clients=10
host m4  aluno@192.168.18.14  sp_ports=5201,5202,5203
```

Os nomes são globais (`SP1`…`SPn`, `C1`…`Cm`). O roster dos Super Peers
(`nome,superpeer,ip,porta`) é **gerado** em `out/stage/<host>/roster.conf` e enviado
às máquinas. Ninguém edita esse arquivo.

Como o código usa o roster:

- Cada Super Peer acha a própria linha **pela porta** (`ler_linha_csv` em `superpeer.c`)
  e tira dela o nome e o IP que anuncia aos outros. Por isso as portas dos Super Peers
  precisam ser únicas no cluster inteiro, mesmo em máquinas diferentes.
- O primeiro da lista (`SP1`) é o bootstrap do Chord e do Gossip para todos.
- O coordenador é o **maior NodeID** (Bully), e o NodeID é o SHA-256 de IP, porta e UUID.
  Ele só é conhecido depois do `up`, com `coordinator`.
- No máximo 64 Super Peers (`GOSSIP_MAX` e `BULLY_MAX`), cerca de 21 máquinas com 3.
- Defina `ip=` quando o nome do alvo ssh não resolve para o IP que as outras máquinas
  usam.
- **Pasta pessoal compartilhada (laboratório):** se a home é a mesma em todas as
  máquinas (NFS), dê a cada host um `dir=` local, por exemplo `dir=/tmp/bt-m1`. Sem
  isso, todas compilam e gravam no mesmo `~/bittorrent-project`.

Cada cliente recebe por opção o Super Peer de entrada (`--host/--port`) e a própria
identidade (`--ip`, `--myport`). O NodeID dele sai de `run/<nome>/peer.uuid`.

## Comandos

| Comando | Efeito |
| --- | --- |
| `list`, `check`, `net` | Topologia, diagnóstico das máquinas e alcance TCP até cada Super Peer |
| `sync`, `build`, `fixtures`, `deploy` | Código, compilação e arquivos de teste nas máquinas |
| `up [--clients] [--keep]` | Zera o estado e sobe os Super Peers (e os clientes) |
| `down` | Fecha clientes e Super Peers e para os espelhos de log |
| `status [--clients]` | Estado de cada processo |
| `kill`, `stop`, `start`, `restart <nome>` | Injeção de falha e volta (`kill` é `kill -9`) |
| `client start\|stop\|status` | Sobe e fecha clientes persistentes |
| `client do <nome> <comando...>` | Envia, espera o `[done]` e mostra a saída |
| `client send <nome> <comando...>` / `client wait <nome> <N>` | Envio sem esperar e espera pelo comando `N` |
| `client batch <nome> <arquivo>` | Manda um arquivo de comandos |
| `client attach <nome>` | Digita direto na sessão do cliente |
| `console` | Laço interativo |
| `watch [cenário\|--console]` | Painéis em tmux |
| `run <cenário>` | Executa `scenarios/<cenário>.sh` |
| `exec <host> <cmd...>`, `logs <nome>`, `clean [--data]` | Depuração e limpeza |
| `overview`, `members`, `chord`, `coordinator`, `state`, `metadata`, `lookup`, `events` | Inspeção dos Super Peers (abaixo) |

## Inspeção dos Super Peers

Nenhum destes comandos mexe nos nós. Eles leem os logs espelhados em `logs/` e, no
`chord --fingers` e no `lookup`, consultam o Super Peer ao vivo (`--cmd topology` e
`--cmd lookup`). Os nomes de Super Peer são opcionais; sem eles, valem todos.

| Comando | O que mostra | Fonte |
| --- | --- | --- |
| `overview [SP...]` | Uma linha por Super Peer: processo vivo ou não, NodeID, sucessor, coordenador, último `LOG #` e membros ALIVE/SUSPECT/FAILED | logs e ssh |
| `members [SP...]` | Matriz observador x membro com o estado que o Gossip de cada nó registrou | logs |
| `chord [SP...] [--fingers]` | Anel por NodeID, sucessor e predecessor de cada nó contra o esperado (`ok` ou `DIVERGE`) e, com `--fingers`, a finger table | logs / ao vivo |
| `coordinator [SP...]` | Quem cada Super Peer reconhece como coordenador, quantas eleições começou e quantas venceu, se todos concordam e o maior NodeID entre os vivos | logs e ssh |
| `state [SP...] [--files]` | Último `LOG #`, objetos (nomes dos `PUT`) e contadores do LFU, com aviso de divergência. `--files` traz a matriz arquivo x versões | logs |
| `metadata [filtro]` | Arquivo, tamanho, ObjectID e chunks que os clientes imprimiram no upload | logs dos clientes |
| `lookup <nome\|object-id> [SP]` | Caminho Chord até o dono do objeto. Pelo nome, o ObjectID sai do log do upload | ao vivo |
| `events <SP> [cat] [N]` | Últimos N eventos. `cat`: `membership`, `election`, `chord`, `2pc`, `ist`, `lfu`, `log`, `chunks`, `all` ou uma regex. Sem `cat`, esconde o heartbeat do Gossip, os `RX` e a finger table | logs |

```text
./cluster.sh overview
./cluster.sh state --files
./cluster.sh chord --fingers SP1
./cluster.sh events SP3 election
./cluster.sh lookup documento_a.pdf SP2
```

O Super Peer não tem consulta de estado (`GET_STATE`). O `state` reconstrói o estado
replicado pelo log da vida atual de cada nó: o último `LOG #n` e os `PUT <arquivo>`
aplicados. Depois de um IST, esses são os logs que o nó recebeu.

## Comandos do cliente

Os mesmos em modo interativo (`bin/client --interactive`), em lote
(`bin/client --batch arquivo`, onde `-` lê da stdin e `--keep-going` não para no
primeiro erro) e nos arquivos de `batch/`:

```text
upload <arquivo>            download <nome> [saída]
lookup <nome|object-id>     delete <nome>            (remoção lógica: Status = REMOVED)
state [host porta]          (topologia Chord do Super Peer)
ping|join|leave [host porta]
sleep <segundos>            help | quit
```

Linhas em branco ou iniciadas por `#` são ignoradas, e aspas duplas agrupam um
argumento com espaços. A sessão tem um NodeID só e escreve estes marcadores na saída:

```text
[ready]            sessão pronta
[done N rc=R]      fim do comando N com código R (é o que o harness espera)
[bye]              sessão fechada
```

`rc` é 0 no sucesso, 1 em falha da operação (sem conexão, resposta diferente de ACK,
arquivo inexistente, SHA-256 divergente) e 2 em comando ou argumento inválido. O modo
de um comando (`--cmd upload --file ...`) continua igual e sai com o mesmo código.

Cada cliente roda dentro de `run/<nome>/` na máquina dele, então os arquivos de teste
ficam em `../files/` (por exemplo `upload ../files/documento_a.pdf`).

## Interface gráfica (`gui/`)

```bash
./gui/run.sh            # usa o mesmo cluster.conf (ou CLUSTER_CONF)
```

Janela em tkinter com gráficos do matplotlib. Na primeira execução cria `gui/.venv`
e instala o matplotlib (precisa de internet). Precisa de `python3-tk`, `python3-venv`
e `python3-pip` (`sudo apt install`) e de tela gráfica (no WSL, o WSLg; por ssh,
`ssh -X`). Só a máquina do operador precisa disso.

| Aba | O que faz |
| --- | --- |
| Cliente | Escolhe um cliente (C1…) e faz upload (arquivos de `out/files`), download, lookup, `state`, ping/join/leave ou um comando livre (`client do`) |
| Cluster | Preparo (list, check, net, deploy, fixtures), subir/derrubar, status, abrir e fechar clientes, `kill -9`/stop/start/restart de um Super Peer |
| Métricas | Anel Chord pelo NodeID com a seta do sucessor, matriz de membership, LogIndex por Super Peer, hits e misses do LFU e linha do tempo (vidas de cada processo, SUSPECT/FAILED, vitórias na eleição, IST). Atualiza sozinha; embaixo, a inspeção em texto do `cluster.sh` |
| Cenários | Roda `scenarios/*.sh` (com `CLUSTER_KEEP` e `WAVE`) e mostra `out/relatorio.md` |
| Logs | Acompanha o log espelhado de um processo, com filtro por categoria ou regex, e marca a troca de vida |

A interface não tem lógica de teste própria: os botões chamam o `cluster.sh`/`run.sh`
e os gráficos leem os logs em `logs/`. "Consultar processos vivos (ssh)" faz um
`cluster.sh status` a cada atualização; desligado, os gráficos usam só os logs.

## Console

```text
cluster> C1 upload ../files/documento_a.pdf     # envia a C1 e mostra a saída e o rc
cluster> C1,C5 lookup documento_a.pdf           # vários clientes de uma vez
cluster> @SP2 kill                              # kill -9 do Super Peer SP2 (também stop, start)
cluster> status
```

Qualquer comando do `cluster.sh` vale no console. Ctrl-D sai sem derrubar o cluster.

## Cenários

| Cenário | O que faz |
| --- | --- |
| `smoke` | Sobe tudo, faz um upload e um download (em outra máquina, se houver) e confere o `LOG #1` em todos |
| `final` | Roteiro do CP6, uma fase por item da lista do professor (abaixo) |
| `carga` | Todos os clientes ao mesmo tempo: cada um envia um arquivo e baixa o do seguinte. `WAVE=<n>` limita quantos agem por vez |
| `professor` | Encaixe para o conjunto de testes do professor |

Fases do `final`:

1. Bully e Chord no boot.
2. SMR e 2PC: uploads de A, B e C.
3. Lookup pelo ObjectID.
4. Gossip e detecção de falha (`kill -9`).
5. 2PC com um participante fora: uploads de D e E.
6. IST na volta do nó.
7. Cache LFU.
8. Remoção lógica: `delete` de C vira `DELETE` no log de todos os Super Peers.
9. Download com `sha256` conferido em duas máquinas.
10. Chunks e compressão (arquivo de 9 MiB).
11. Réplica: derruba o dono de A e baixa de novo.

Ao terminar, `run.sh` grava `out/relatorio.md` (fases, métricas de tempo e caminho dos
logs) e derruba o cluster. `CLUSTER_KEEP=1` deixa tudo de pé.

## Logs

Cada processo remoto escreve em `run/<nome>/proc.log`. Um espelho por máquina lê todos
esses logs (`ssh tail -F`), carimba cada linha com **o relógio da máquina do operador**
(a spec assume relógios independentes) e grava em `logs/<nome>.<vida>.log`.
`<nome>.log` aponta para a vida mais recente. Uma vida nova começa quando o processo é
reiniciado.

## Observações

- O `ssh` da máquina remota pode imprimir lixo antes do comando (`.bashrc`, `motd`). O
  harness descarta tudo até um marcador, então isso não atrapalha.
- Um `upload` feito depois de um `kill -9` e antes da detecção da falha (cerca de 15 s)
  pode ser recusado pelo 2PC. Os cenários esperam o `SUSPECT`/`FAILED` antes de enviar.
- **Eleição de partida.** 5 s depois de subir, o Super Peer só faz eleição se não
  conhece um coordenador vivo ou se tem NodeID maior que o dele (`bully_start_needs_election`).
  Um nó que volta e já recebeu `COORDINATOR` imprime
  `Bully: coordenador <SP> ja conhecido, sem eleicao de partida` e segue aceitando STORE.
- O nome que o Super Peer registra (`PUT <nome>`) é o caminho passado ao `upload`. A
  busca e as conferências usam o basename.
