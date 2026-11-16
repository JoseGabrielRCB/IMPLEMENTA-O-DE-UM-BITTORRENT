# Correções do Checkpoint 6 (QA)

Revisão do Checkpoint 6 contra a Seção 16.11 do enunciado (integração e teste final). Fiz o QA na branch `checkpoint-6` e me limitei ao que o enunciado pede. Encontrei e corrigi **5 problemas**.

---

## 1. A réplica dos chunks era simulada

- **O que o enunciado pede:** réplica dos chunks (Seção 16.11), fator de replicação 3 (Seção 8.6) e troca de réplica quando uma falha (Seção 11.6).
- **O que estava errado:**
  - O upload gravava os chunks só no `storage/` da pasta de onde o cliente rodava.
  - Cada Super Peer lia esse mesmo `storage/`. Por isso o teste só passava com todos os processos na mesma pasta.
  - Com os Super Peers em pastas diferentes, todas as réplicas respondiam ERROR e o download falhava.
  - Quando um peer fazia LOOKUP sem ter feito JOIN, era registrado com o endereço fixo `127.0.0.1:55102`. Esse endereço aparecia como provedor sem que existisse ali um peer guardando os chunks.
- **Correção:**
  - Criei a mensagem interna `MSG_CHUNK_STORE` (tipo 29).
  - Depois que o STORE do metadado volta com ACK, o peer envia cada chunk comprimido ao Super Peer.
  - O Super Peer descomprime o chunk (LZ4), confere o SHA-256, grava em `storage/<porta>/<hash>.lz4` e repassa o chunk aos outros Super Peers vivos.
  - Na réplica, o campo que manda repassar vai desligado, para o chunk não ser repassado de novo.
  - Com 3 Super Peers, cada chunk fica com 3 cópias.
  - O `DOWNLOAD_REQ` lê o chunk de `storage/<porta>/`.
  - Removi o registro automático com endereço fixo. A lista de provedores é o próprio Super Peer mais os Super Peers vivos no Gossip.
- **Arquivos:**
  - `include/protocol.h` e `protocol.c`: novo tipo de mensagem.
  - `superpeer.c`: `tratar_chunk_store`, `replicar_chunk`, `tratar_download_req`, `tratar_lookup_responder` e `validar_origem`.
  - `peer.c`: `enviar_chunks`.

## 2. O SHA-256 do chunk era calculado sobre o dado comprimido

- **O que o enunciado pede:** o fluxo `LZ4 Decode → SHA-256 → Validação` (Seção 10.4). O hash do chunk é calculado sobre o conteúdo descomprimido (Seção 11.3).
- **O que estava errado:**
  - No upload, o hash era calculado sobre o chunk já comprimido.
  - No download, o hash era comparado antes de descomprimir.
- **Correção:**
  - No upload, o hash é calculado sobre o chunk original.
  - No download, o chunk é descomprimido primeiro e o hash é comparado depois.
- **Arquivo:** `peer.c`, na criação dos chunks e na thread de download.

## 3. O heartbeat do Gossip recomeçava do zero quando um Super Peer reiniciava

- **O que o enunciado pede:** heartbeat a cada 5 s e timeout de 15 s (Seção 16.8). Um nó que volta entra como FOLLOWER (Seção 15.7).
- **O que estava errado:**
  - Quando um Super Peer reiniciava, o contador de heartbeat dele voltava a 0.
  - Os outros nós só aceitam um heartbeat maior que o último que viram. Por isso ignoravam o nó religado, que continuava aparecendo como morto.
  - Assim, o SP3 recusava o SP2 como coordenador e se elegia também, e por alguns segundos havia **dois coordenadores**.
- **Correção:** o contador passa a começar no horário de início do processo (`time(NULL)`), que funciona como número de encarnação. O nó religado é reconhecido no primeiro heartbeat.
- **Arquivo:** `superpeer.c`, em `gossip_start`.

## 4. A documentação dos checkpoints tinha sido apagada

- **O que o enunciado pede:** um documento por checkpoint (Seção 16.12, Observação 2) e "Documentação" (Seção 16.11).
- **O que estava errado:**
  - O commit do CP6 apagou `docs/cp3`, `docs/cp4` e `docs/cp5`.
  - Não existia documento do CP6.
  - O `arquitetura.md` citava `bully.c`, `smr.c` e `cache.c`, que não existem mais.
- **Correção:**
  - Restaurei `docs/` a partir da branch `checkpoint-5`.
  - Criei `docs/cp6/CP6_Integracao_e_Teste_Final.md`. Ele descreve o que cada aluno demonstra, os testes, as correções e as decisões C-02, C-07, C-08, C-16 e C-18.
  - No `arquitetura.md`, corrigi a seção 14, que descrevia a ordem errada do hash. Também acrescentei as seções 15 (em qual arquivo ficou cada módulo antigo) e 16 (`CHUNK_STORE`, `storage/<porta>` e o número de encarnação do heartbeat).
- **Arquivos:** `docs/`, `arquitetura.md` e `README.md`.

## 5. O teste do CP6 não derrubava nenhum nó

- **O que o enunciado pede:** "Testes" (Seção 16.11), com os itens de verificação do CP4 e do CP5.
- **O que estava errado:**
  - O `tests/c6/run.sh` só subia os nós e procurava textos no log.
  - As verificações de Gossip, Bully e IST passavam só com a subida normal.
  - O teste não comparava o arquivo baixado com o original.
- **Correção:** reescrevi o `tests/c6/run.sh` com 29 verificações:
  - **Aluno 1:**
    - 3 chunks, LZ4, cache LFU com MISS e depois HIT, e 3 cópias de cada chunk;
    - download comparado com o original;
    - `kill -9` no SP2, depois um download que troca de réplica e gera um arquivo idêntico ao original.
  - **Aluno 2:**
    - Chord, LOG do SMR, PREPARE e COMMIT do 2PC;
    - `SUSPECT SP2` no Gossip;
    - IST: dois uploads com o SP2 fora; ao voltar, ele detecta as versões que faltam, recebe esses logs e chega a READY;
    - Bully: `kill -9` no coordenador, depois ELECTION, vitória do maior NodeID e anúncio do novo COORDINATOR;
    - 2PC com `SIMULATE_PREPARE_FAIL=1`, que termina em ABORT sem nenhum COMMIT.
  - Em todos os logs, confere que nenhum processo travou.
- **Arquivo:** `tests/c6/run.sh`.

## Outros ajustes

- `.gitignore`: a linha `storage/*.lz4` virou `storage/`, para o git ignorar também as subpastas `storage/<porta>/`.

---

## Resultados dos testes (depois das correções)

| Bateria | Resultado |
|---|---|
| `tests/c1/test_protocol` | 5/5 |
| `bash tests/run.sh` (C2) | 8/8 |
| `bash tests/c3/run.sh` | 5/5 |
| `bash tests/c4/run.sh` | 4/4 |
| `bash tests/c5/run.sh` | 5/5 |
| `bash tests/c6/run.sh` | **29/29** |
| `tests/c6` compilado com `-fsanitize=address,undefined` | 29/29, nenhum relatório do sanitizer |

Também testei a réplica com os Super Peers em pastas separadas. Cada chunk ficou com 3 cópias, e com o SP2 derrubado o download terminou com o arquivo idêntico ao original.

## Pontos encontrados e não alterados

- O `tests/c4/run.sh` usa `--cmd put` e `--cmd state`, comandos que o `peer.c` nunca implementou. Os testes passam porque conferem outros textos no log. Não alterei porque esse script é do CP4.
- O build com `-Wpedantic` mostra um aviso de possível truncamento de texto em `peer.c:771`. O aviso já existia antes e não afeta o funcionamento.

---

## Arquivos para o commit

- **Modificados:**
  - `.gitignore`
  - `README.md`
  - `arquitetura.md`
  - `include/protocol.h`
  - `protocol.c`
  - `peer.c`
  - `superpeer.c`
  - `tests/c6/run.sh`
- **Restaurados:** `docs/cp3/`, `docs/cp4/`, `docs/cp5/`. Já estão no stage.
- **Novos:**
  - `docs/cp6/CP6_Integracao_e_Teste_Final.md`
  - `CORRECOES_CP6.md` (este arquivo)

```bash
git add .gitignore README.md arquitetura.md include/protocol.h protocol.c peer.c superpeer.c \
        tests/c6/run.sh docs/ CORRECOES_CP6.md
git commit -m "CP6: corrige replica dos chunks, hash do chunk, heartbeat do gossip, docs e teste final"
git push
```
