# Checkpoint 1: Aluno 2 (node.c e superpeer.c)

Você está ajudando o Aluno 2 de um trabalho em dupla de Programação Distribuída (UEMS), escrito em C.
Este arquivo define TUDO o que pode ser feito agora. O que não estiver aqui está fora do escopo.

## 1. Regra principal

- Implemente SOMENTE o Checkpoint 1 da parte do Aluno 2.
- NÃO antecipe nada de checkpoints futuros. Não crie "ganchos", campos, funções, TODOs ou comentários "para depois".
- Prefira sempre a solução mais simples que funcione. Nada de algoritmos elaborados, estruturas genéricas, otimizações ou abstrações extras.
- Na dúvida sobre se algo está no escopo, NÃO faça: pare e pergunte ao usuário.

## 2. Arquivos

Você só pode criar ou editar:

```
node.h
node.c
superpeer.h
superpeer.c
```

- `network.c`, `protocol.c` e `peer.c` (e seus `.h`) são do Aluno 1. NÃO crie, NÃO edite, NÃO reescreva.
- Use apenas as funções que os `.h` do Aluno 1 já expõem. Se faltar algo ou os arquivos não existirem, PARE e pergunte ao usuário. Não invente a implementação de rede ou de protocolo.
- Não crie `main` sem perguntar antes onde fica o ponto de entrada (o comando de compilação usa `*.c`, e dois `main` quebram o build).
- Não crie Makefile, scripts, pastas ou arquivos extras sem pedido do usuário.

## 3. Bibliotecas permitidas

Apenas:

- biblioteca padrão C (`stdio.h`, `stdlib.h`, `string.h`, `stdint.h`, `stdbool.h`, `time.h`, `errno.h`)
- POSIX (`unistd.h`, `fcntl.h`, `sys/types.h`, `sys/stat.h`)
- POSIX Sockets (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`), só se necessário além do que o Aluno 1 fornece
- POSIX Threads (`pthread.h`)
- OpenSSL `libcrypto`, SOMENTE para SHA-256: `#include <openssl/sha.h>` e a função `SHA256()`

Proibido: qualquer outra biblioteca (inclusive `libuuid`, glib, bibliotecas de JSON/INI etc.). A `liblz4` não deve ser usada neste checkpoint.

## 4. O que implementar

### 4.1 NodeID (node.c)

- `NodeID = SHA256(IP || Porta || UUID)`, guardado em `uint8_t node_id[32]`.
- UUID: 16 bytes aleatórios lidos de `/dev/urandom`.
- O UUID é gerado UMA única vez e salvo em arquivo. Se o arquivo existir, leia dele e NÃO gere outro. Isso é obrigatório.
- Comparação de NodeIDs: `memcmp` sobre os 32 bytes.
- Função para imprimir o NodeID em hexadecimal.

### 4.2 Configuração (node.c)

- Por argumentos de linha de comando: IP, porta, papel (peer ou superpeer), caminho do arquivo do UUID e, para o peer, IP e porta do Super Peer.
- Validar os argumentos (porta entre 1 e 65535, IP válido com `inet_pton`). Em erro, mensagem clara e saída com código diferente de zero.
- Nada de arquivo de configuração, parser ou variáveis de ambiente.

### 4.3 Identificação do processo (node.c)

Uma struct simples com a identidade do nó:

- NodeID
- IP
- porta
- papel: `ROLE_PEER` ou `ROLE_SUPERPEER`
- estado de comunicação, usando apenas: `DISCONNECTED`, `CONNECTING`, `CONNECTED`, `AUTHENTICATED`

Ao iniciar, o processo imprime sua identidade (papel, IP, porta, NodeID).

### 4.4 Registro do nó

- O Peer envia `JOIN` ao Super Peer.
- O Super Peer valida e responde `ACK` (aceito) ou `ERROR` (recusado).
- Validação = handshake simples: versão do protocolo igual à esperada e NodeID diferente de zero.
- Um NodeID já registrado não é duplicado.
- `AUTHENTICATED` significa apenas que esse handshake passou. NÃO implemente TLS, JWT, senhas, chaves ou qualquer criptografia.
- `LEAVE` remove o nó da tabela.
- Mensagens usadas neste checkpoint: SOMENTE `JOIN`, `LEAVE`, `ACK`, `ERROR`.

### 4.5 Tabela básica de membros (superpeer.c)

Campos de cada entrada:

- `NodeID` (`uint8_t[32]`)
- `IP`
- `Port`
- `State` (apenas `ALIVE`)
- `LastHeartbeat` (preencher com `time(NULL)` no registro, nada mais)
- `Version` (`uint64_t`, começa em 1)

Regras:

- Vetor de tamanho fixo (`#define MAX_MEMBERS 64`). Nada de hash table, lista ligada ou árvore.
- Operações: inserir, buscar por NodeID, remover, imprimir.
- Um `pthread_mutex_t` protege toda leitura e escrita da tabela.
- Tabela cheia: retornar erro, nunca escrever fora do vetor.
- Após cada inserção ou remoção, imprimir a tabela.

### 4.6 Criação do Super Peer (superpeer.c)

1. Carrega a identidade (usando node.c).
2. Abre o socket de escuta usando as funções do Aluno 1.
3. Para cada conexão aceita, cria uma thread (`pthread_create` + `pthread_detach`).
4. Recebe a mensagem pelas funções do Aluno 1 (header interpretado e checksum validado por ele).
5. Imprime o tipo da mensagem e o NodeID de origem.
6. `JOIN`: registra e responde `ACK`/`ERROR`. `LEAVE`: remove e responde `ACK`. Outro tipo: responde `ERROR`.
7. Fecha a conexão e libera tudo o que alocou.

Uma thread por conexão é suficiente. Nada de thread pool, `epoll`, `select` ou filas.

## 5. Fora do escopo (NÃO fazer)

Não implemente nada além da seção 4. Em especial, nada de: finger table, lookup, anel, successor/predecessor, heartbeat periódico, timeout, detecção de falha, eleição, coordenador, metadata, hash table de arquivos, log de operações, replicação, transações, cache, compressão, fragmentação, upload ou download.
Não mencione esses itens no código nem nos comentários.

## 6. Segurança de memória (critério de aceitação: zero segmentation fault)

- Sempre checar retorno de `malloc`, `read`, `write`, `fopen`, `pthread_*` e das funções do Aluno 1.
- Copiar strings com tamanho limitado (`snprintf`, `memcpy` com tamanho conferido). Proibido `strcpy`, `strcat`, `sprintf`, `gets`.
- Validar índices e tamanhos antes de acessar vetores e buffers.
- Todo `malloc` tem `free` correspondente. Todo socket aberto é fechado.
- Nada de variáveis globais além da tabela de membros e seu mutex.

## 7. Estilo e comentários

- C11 com `#define _POSIX_C_SOURCE 200809L` no topo dos `.c`.
- Funções curtas, nomes claros, código legível para um estudante.
- Comentários em português, SIMPLES e SUCINTOS: uma linha dizendo o que o bloco faz. Nada de comentários longos, explicações teóricas ou blocos decorativos.
- Não usar travessão em comentários ou mensagens.

Exemplo do tamanho de comentário esperado:

```c
/* Le o UUID do arquivo ou gera um novo */
```

## 8. Compilação e teste

Compilar SEMPRE com:

```bash
gcc -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g -O1 *.c -llz4 -lpthread -lcrypto
```

- Zero warnings e zero erros do sanitizer.
- Teste esperado: um Super Peer e um Peer em terminais separados. O Peer envia `JOIN`, o Super Peer imprime o header recebido, registra o nó e imprime a tabela, e o Peer recebe `ACK`.
- Reiniciar o mesmo nó deve manter o mesmo NodeID (UUID lido do arquivo).

## 9. Ao terminar

Responda com:

1. arquivos alterados;
2. comando usado para compilar e resultado;
3. como rodar o teste;
4. lista curta das decisões tomadas (formato da configuração, onde o UUID é salvo, o que a validação do `JOIN` confere), para o documento do checkpoint.

Não sugira próximos passos nem funcionalidades de outros checkpoints.
