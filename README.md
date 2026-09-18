# Checkpoint 1

Sistema P2P em C com dois programas: o **Super Peer**, que escuta e mantem a
tabela de membros, e o **Peer**, que se registra nele.

Mensagens deste checkpoint: `JOIN`, `LEAVE`, `ACK` e `ERROR`. O `PING` e o
`PONG` existem so para o script de teste responder viva.

## Requisitos

```bash
sudo apt install build-essential libssl-dev
```

A `libssl-dev` e obrigatoria: o NodeID usa `SHA256()` da `libcrypto`.

## Compilar

```bash
make
```

Gera quatro executaveis em `bin/`:

| Executavel | O que e |
|---|---|
| `bin/superpeer` | o Super Peer |
| `bin/peer` | o Peer |
| `bin/node` | copia do `bin/superpeer` |
| `bin/client` | copia do `bin/peer` |

Cada binario e compilado de uma unidade so, porque `peer.c` e `superpeer.c`
incluem `node.c` e `network.c`, que por sua vez inclui `protocol.c`. Por isso
nao use `gcc *.c`, use o `make`.

Para limpar:

```bash
make clean
```

## Teste basico (dois terminais)

**Terminal 1**, sobe o Super Peer:

```bash
./bin/superpeer superpeer 127.0.0.1 9000 sp.uuid
```

Ele imprime a identidade e fica escutando:

```
Papel:  SUPERPEER
IP:     127.0.0.1
Porta:  9000
Estado: DISCONNECTED
NodeID: e7b5406b6641a38be5468df5cacdecbbd983ea55e32ec8fdc1f3a4214a683b80
Escutando na porta 9000
```

**Terminal 2**, registra um Peer:

```bash
./bin/peer --cmd join --host 127.0.0.1 --port 9000 --ip 127.0.0.1 --myport 9100 --uuid peer.uuid
```

O Peer imprime a propria identidade e termina com:

```
RX ACK
Estado: AUTHENTICATED
```

No terminal do Super Peer aparece a mensagem recebida e a tabela:

```
RX JOIN de 4ffa318ea4c653de4e1652eb86bc7baf8da5a9470d431a3e3be1f61602e5ff27
Tabela de membros (1/64)
  [0] 4ffa318ea4c653de4e1652eb86bc7baf8da5a9470d431a3e3be1f61602e5ff27 127.0.0.1:9100 ALIVE last_heartbeat=1789699479 version=1
```

Para sair da tabela:

```bash
./bin/peer --cmd leave --host 127.0.0.1 --port 9000 --ip 127.0.0.1 --myport 9100 --uuid peer.uuid
```

Responde `RX ACK` e o Super Peer imprime a tabela sem o no.

## Argumentos

### Super Peer

Formato do checkpoint, posicional:

```
./bin/superpeer superpeer <ip> <porta> <arquivo_uuid>
```

Formato do script de teste, por opcoes:

```
./bin/superpeer --port <porta> [--name <nome>] [--config <arquivo>]
```

No segundo formato o IP e `127.0.0.1`, o arquivo do UUID e `superpeer.uuid` e o
`--config` e aceito mas nao e lido.

### Peer (opcoes)

| Opcao | Para que serve | Padrao |
|---|---|---|
| `--cmd` | `join`, `leave` ou `ping` | `ping` |
| `--host` | IP do Super Peer | `127.0.0.1` |
| `--port` | porta do Super Peer | `55101` |
| `--ip` | IP do proprio Peer | `127.0.0.1` |
| `--myport` | porta do proprio Peer | `55102` |
| `--uuid` | arquivo do UUID do Peer | `peer.uuid` |

IP e porta sao validados nos dois programas. Argumento errado imprime a causa e
sai com codigo diferente de zero.

## O que mais da para conferir

**O NodeID nao muda ao reiniciar.** Rode o mesmo comando duas vezes e compare a
linha `NodeID:`. O UUID fica salvo no arquivo passado no argumento e e lido de
volta. Apagar o arquivo gera outra identidade.

**Registro repetido nao duplica.** Rode o mesmo `--cmd join` duas vezes: a
segunda responde `ACK`, o Super Peer avisa que o NodeID ja estava registrado e a
tabela continua com uma entrada so.

**Varios peers ao mesmo tempo.** Use arquivos de UUID e portas diferentes:

```bash
./bin/peer --cmd join --host 127.0.0.1 --port 9000 --ip 127.0.0.1 --myport 9200 --uuid peer2.uuid
```

Uma thread e criada por conexao, entao varios peers podem se registrar juntos.

**Teste de viva.** `--cmd ping` recebe `RX PONG`. Qualquer outro tipo de
mensagem recebe `RX ERROR`, porque so `JOIN` e `LEAVE` sao tratados aqui.

**Registro recusado.** O Super Peer responde `ERROR` quando a versao do
protocolo e diferente de 1, quando o NodeID vem zerado, quando o endereco do
payload e invalido ou quando a tabela chega em 64 membros. Pacote com checksum
errado e descartado sem resposta.

## Testes unitarios do protocolo

Testam o empacotamento, o desempacotamento e o CRC32 sem abrir socket nenhum:

```bash
make -C tests/c1
./tests/c1/test_protocol
```

Sao cinco casos: ida e volta, CRC detectando corrupcao, buffer menor que o
cabecalho, tamanho de payload inconsistente e payload vazio. Sai com codigo 0 se
todos passarem.

## Script de teste do professor

```bash
bash script_test.sh
```

Duas coisas importam:

- Rode com `bash`, nao com `./script_test.sh`. O `#!/usr/bin/env bash` esta na
  linha 6 do arquivo, e shebang so vale na linha 1, entao o script seria
  executado por `/bin/sh` e quebraria em coisas que nao tem a ver com o codigo.
- A variavel `ROOT` na linha 9 aponta para um caminho fixo. Ajuste para o
  diretorio do projeto antes de rodar.

Rodando assim, o resultado e `PASS: 10` e `FAIL: 0`.

## Observacoes

- Os arquivos `.uuid` sao criados no diretorio de onde o programa e executado.
- O Super Peer roda ate receber `Ctrl+C`.
- A pasta `tests/c1/logs/` precisa existir: e para onde o script do professor
  manda o log do servidor.
