# Checkpoint 1 - Aluno 2 - Codigo

Arquivos: `node.h`, `node.c`, `superpeer.h`, `superpeer.c`

## node.h

```c
#ifndef NODE_H
#define NODE_H

#include <stdint.h>

#define NODE_ID_LEN 32
#define NODE_ID_HEX_LEN 65
#define NODE_UUID_LEN 16
#define NODE_IP_LEN 16
#define NODE_PATH_LEN 256

/* Papel do processo */
typedef enum {
    ROLE_PEER,
    ROLE_SUPERPEER
} NodeRole;

/* Estado da comunicacao do no */
typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_AUTHENTICATED
} NodeState;

/* Configuracao lida da linha de comando */
typedef struct {
    char ip[NODE_IP_LEN];
    uint16_t port;
    NodeRole role;
    char uuid_path[NODE_PATH_LEN];
    char superpeer_ip[NODE_IP_LEN];
    uint16_t superpeer_port;
} NodeConfig;

/* Identidade do processo */
typedef struct {
    uint8_t node_id[NODE_ID_LEN];
    char ip[NODE_IP_LEN];
    uint16_t port;
    NodeRole role;
    NodeState state;
} Node;

int node_parse_args(int argc, char *argv[], NodeConfig *config);
void node_print_usage(const char *program);

int node_load_uuid(const char *path, uint8_t uuid[NODE_UUID_LEN]);
void node_compute_id(const char *ip, uint16_t port,
                     const uint8_t uuid[NODE_UUID_LEN],
                     uint8_t node_id[NODE_ID_LEN]);

int node_init(Node *node, const NodeConfig *config);

int node_id_compare(const uint8_t a[NODE_ID_LEN], const uint8_t b[NODE_ID_LEN]);
int node_id_is_zero(const uint8_t node_id[NODE_ID_LEN]);
void node_id_to_hex(const uint8_t node_id[NODE_ID_LEN], char hex[NODE_ID_HEX_LEN]);
void node_print_id(const uint8_t node_id[NODE_ID_LEN]);

const char *node_role_name(NodeRole role);
const char *node_state_name(NodeState state);
void node_print(const Node *node);

#endif
```

## node.c

```c
#define _POSIX_C_SOURCE 200809L

#include "node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <openssl/sha.h>

/* Le 16 bytes aleatorios de /dev/urandom */
static int node_generate_uuid(uint8_t uuid[NODE_UUID_LEN])
{
    FILE *file = fopen("/dev/urandom", "rb");
    size_t lidos;

    if (file == NULL) {
        perror("fopen /dev/urandom");
        return -1;
    }

    lidos = fread(uuid, 1, NODE_UUID_LEN, file);
    fclose(file);

    if (lidos != NODE_UUID_LEN) {
        fprintf(stderr, "Erro: nao foi possivel ler %d bytes de /dev/urandom\n", NODE_UUID_LEN);
        return -1;
    }
    return 0;
}

/* Le o UUID do arquivo ou gera um novo e salva */
int node_load_uuid(const char *path, uint8_t uuid[NODE_UUID_LEN])
{
    FILE *file;
    size_t bytes;

    if (path == NULL || uuid == NULL) {
        return -1;
    }

    file = fopen(path, "rb");
    if (file != NULL) {
        bytes = fread(uuid, 1, NODE_UUID_LEN, file);
        fclose(file);
        if (bytes != NODE_UUID_LEN) {
            fprintf(stderr, "Erro: arquivo de UUID invalido: %s\n", path);
            return -1;
        }
        return 0;
    }

    if (node_generate_uuid(uuid) != 0) {
        return -1;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Erro: nao foi possivel criar %s: %s\n", path, strerror(errno));
        return -1;
    }

    bytes = fwrite(uuid, 1, NODE_UUID_LEN, file);
    if (fclose(file) != 0 || bytes != NODE_UUID_LEN) {
        fprintf(stderr, "Erro: nao foi possivel salvar o UUID em %s\n", path);
        return -1;
    }
    return 0;
}

/* NodeID = SHA256(IP || Porta || UUID) */
void node_compute_id(const char *ip, uint16_t port,
                     const uint8_t uuid[NODE_UUID_LEN],
                     uint8_t node_id[NODE_ID_LEN])
{
    uint8_t buffer[NODE_IP_LEN + 2 + NODE_UUID_LEN];
    size_t ip_len;

    if (ip == NULL || uuid == NULL || node_id == NULL) {
        return;
    }

    ip_len = strlen(ip);
    if (ip_len > NODE_IP_LEN) {
        ip_len = NODE_IP_LEN;
    }

    memcpy(buffer, ip, ip_len);
    buffer[ip_len] = (uint8_t)(port >> 8);
    buffer[ip_len + 1] = (uint8_t)(port & 0xFF);
    memcpy(buffer + ip_len + 2, uuid, NODE_UUID_LEN);

    SHA256(buffer, ip_len + 2 + NODE_UUID_LEN, node_id);
}

/* Compara dois NodeIDs byte a byte */
int node_id_compare(const uint8_t a[NODE_ID_LEN], const uint8_t b[NODE_ID_LEN])
{
    return memcmp(a, b, NODE_ID_LEN);
}

/* Diz se o NodeID e formado so por zeros */
int node_id_is_zero(const uint8_t node_id[NODE_ID_LEN])
{
    int i;

    for (i = 0; i < NODE_ID_LEN; i++) {
        if (node_id[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Converte o NodeID para texto hexadecimal */
void node_id_to_hex(const uint8_t node_id[NODE_ID_LEN], char hex[NODE_ID_HEX_LEN])
{
    int i;

    for (i = 0; i < NODE_ID_LEN; i++) {
        snprintf(hex + (i * 2), 3, "%02x", node_id[i]);
    }
}

/* Imprime o NodeID em hexadecimal */
void node_print_id(const uint8_t node_id[NODE_ID_LEN])
{
    char hex[NODE_ID_HEX_LEN];

    node_id_to_hex(node_id, hex);
    printf("%s", hex);
}

const char *node_role_name(NodeRole role)
{
    return (role == ROLE_SUPERPEER) ? "SUPERPEER" : "PEER";
}

const char *node_state_name(NodeState state)
{
    switch (state) {
    case STATE_DISCONNECTED:
        return "DISCONNECTED";
    case STATE_CONNECTING:
        return "CONNECTING";
    case STATE_CONNECTED:
        return "CONNECTED";
    case STATE_AUTHENTICATED:
        return "AUTHENTICATED";
    }
    return "DISCONNECTED";
}

/* Valida a porta e converte para numero */
static int node_parse_port(const char *texto, uint16_t *port)
{
    char *fim = NULL;
    long valor;

    errno = 0;
    valor = strtol(texto, &fim, 10);
    if (errno != 0 || fim == texto || *fim != '\0' || valor < 1 || valor > 65535) {
        return -1;
    }

    *port = (uint16_t)valor;
    return 0;
}

/* Valida o IP e copia para o destino */
static int node_parse_ip(const char *texto, char *destino, size_t tamanho)
{
    struct in_addr endereco;

    if (strlen(texto) >= tamanho) {
        return -1;
    }
    if (inet_pton(AF_INET, texto, &endereco) != 1) {
        return -1;
    }

    snprintf(destino, tamanho, "%s", texto);
    return 0;
}

void node_print_usage(const char *program)
{
    fprintf(stderr, "Uso:\n");
    fprintf(stderr, "  %s superpeer <ip> <porta> <arquivo_uuid>\n", program);
    fprintf(stderr, "  %s peer <ip> <porta> <arquivo_uuid> <ip_superpeer> <porta_superpeer>\n",
            program);
}

/* Le a configuracao dos argumentos da linha de comando */
int node_parse_args(int argc, char *argv[], NodeConfig *config)
{
    if (config == NULL || argc < 2) {
        fprintf(stderr, "Erro: argumentos insuficientes\n");
        return -1;
    }

    memset(config, 0, sizeof(*config));

    if (strcmp(argv[1], "superpeer") == 0) {
        config->role = ROLE_SUPERPEER;
        if (argc != 5) {
            fprintf(stderr, "Erro: o superpeer precisa de 3 argumentos\n");
            return -1;
        }
    } else if (strcmp(argv[1], "peer") == 0) {
        config->role = ROLE_PEER;
        if (argc != 7) {
            fprintf(stderr, "Erro: o peer precisa de 5 argumentos\n");
            return -1;
        }
    } else {
        fprintf(stderr, "Erro: papel invalido: %s\n", argv[1]);
        return -1;
    }

    if (node_parse_ip(argv[2], config->ip, sizeof(config->ip)) != 0) {
        fprintf(stderr, "Erro: IP invalido: %s\n", argv[2]);
        return -1;
    }
    if (node_parse_port(argv[3], &config->port) != 0) {
        fprintf(stderr, "Erro: porta invalida: %s\n", argv[3]);
        return -1;
    }
    if (strlen(argv[4]) == 0 || strlen(argv[4]) >= sizeof(config->uuid_path)) {
        fprintf(stderr, "Erro: caminho do arquivo de UUID invalido\n");
        return -1;
    }
    snprintf(config->uuid_path, sizeof(config->uuid_path), "%s", argv[4]);

    if (config->role == ROLE_PEER) {
        if (node_parse_ip(argv[5], config->superpeer_ip, sizeof(config->superpeer_ip)) != 0) {
            fprintf(stderr, "Erro: IP do superpeer invalido: %s\n", argv[5]);
            return -1;
        }
        if (node_parse_port(argv[6], &config->superpeer_port) != 0) {
            fprintf(stderr, "Erro: porta do superpeer invalida: %s\n", argv[6]);
            return -1;
        }
    }

    return 0;
}

/* Monta a identidade do processo a partir da configuracao */
int node_init(Node *node, const NodeConfig *config)
{
    uint8_t uuid[NODE_UUID_LEN];

    if (node == NULL || config == NULL) {
        return -1;
    }

    if (node_load_uuid(config->uuid_path, uuid) != 0) {
        return -1;
    }

    memset(node, 0, sizeof(*node));
    snprintf(node->ip, sizeof(node->ip), "%s", config->ip);
    node->port = config->port;
    node->role = config->role;
    node->state = STATE_DISCONNECTED;
    node_compute_id(node->ip, node->port, uuid, node->node_id);

    return 0;
}

/* Imprime a identidade do processo */
void node_print(const Node *node)
{
    if (node == NULL) {
        return;
    }

    printf("Papel:  %s\n", node_role_name(node->role));
    printf("IP:     %s\n", node->ip);
    printf("Porta:  %u\n", node->port);
    printf("Estado: %s\n", node_state_name(node->state));
    printf("NodeID: ");
    node_print_id(node->node_id);
    printf("\n");
}
```

## superpeer.h

```c
#ifndef SUPERPEER_H
#define SUPERPEER_H

#include <stdint.h>
#include <time.h>

#include "node.h"

#define MAX_MEMBERS 64

/* Retornos das operacoes da tabela */
#define MEMBER_OK 0
#define MEMBER_ERR_FULL (-1)
#define MEMBER_ERR_DUPLICATE (-2)
#define MEMBER_ERR_NOT_FOUND (-3)
#define MEMBER_ERR_INVALID (-4)

/* Estado do membro na tabela */
typedef enum {
    MEMBER_ALIVE
} MemberState;

/* Entrada da tabela de membros */
typedef struct {
    uint8_t node_id[NODE_ID_LEN];
    char ip[NODE_IP_LEN];
    uint16_t port;
    MemberState state;
    time_t last_heartbeat;
    uint64_t version;
} Member;

int member_table_add(const uint8_t node_id[NODE_ID_LEN], const char *ip, uint16_t port);
int member_table_contains(const uint8_t node_id[NODE_ID_LEN]);
int member_table_remove(const uint8_t node_id[NODE_ID_LEN]);
void member_table_print(void);

#endif
```

## superpeer.c

```c
#define _POSIX_C_SOURCE 200809L

#include "superpeer.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* Tabela de membros e o mutex que a protege */
static Member members[MAX_MEMBERS];
static int member_count = 0;
static pthread_mutex_t members_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Procura o NodeID na tabela, usar com o mutex travado */
static int member_find_index(const uint8_t node_id[NODE_ID_LEN])
{
    int i;

    for (i = 0; i < member_count; i++) {
        if (node_id_compare(members[i].node_id, node_id) == 0) {
            return i;
        }
    }
    return -1;
}

/* Imprime a tabela, usar com o mutex travado */
static void member_print_table(void)
{
    char hex[NODE_ID_HEX_LEN];
    int i;

    printf("Tabela de membros (%d/%d)\n", member_count, MAX_MEMBERS);
    for (i = 0; i < member_count; i++) {
        node_id_to_hex(members[i].node_id, hex);
        printf("  [%d] %s %s:%u ALIVE last_heartbeat=%lld version=%llu\n",
               i, hex, members[i].ip, members[i].port,
               (long long)members[i].last_heartbeat,
               (unsigned long long)members[i].version);
    }
}

/* Insere um membro novo na tabela */
int member_table_add(const uint8_t node_id[NODE_ID_LEN], const char *ip, uint16_t port)
{
    int resultado;

    if (node_id == NULL || ip == NULL || strlen(ip) >= NODE_IP_LEN) {
        return MEMBER_ERR_INVALID;
    }
    if (pthread_mutex_lock(&members_mutex) != 0) {
        return MEMBER_ERR_INVALID;
    }

    if (member_find_index(node_id) >= 0) {
        resultado = MEMBER_ERR_DUPLICATE;
    } else if (member_count >= MAX_MEMBERS) {
        resultado = MEMBER_ERR_FULL;
    } else {
        Member *membro = &members[member_count];

        memcpy(membro->node_id, node_id, NODE_ID_LEN);
        snprintf(membro->ip, sizeof(membro->ip), "%s", ip);
        membro->port = port;
        membro->state = MEMBER_ALIVE;
        membro->last_heartbeat = time(NULL);
        membro->version = 1;
        member_count++;
        resultado = MEMBER_OK;
        member_print_table();
    }

    pthread_mutex_unlock(&members_mutex);
    return resultado;
}

/* Diz se o NodeID ja esta registrado */
int member_table_contains(const uint8_t node_id[NODE_ID_LEN])
{
    int encontrado;

    if (node_id == NULL) {
        return 0;
    }
    if (pthread_mutex_lock(&members_mutex) != 0) {
        return 0;
    }

    encontrado = (member_find_index(node_id) >= 0) ? 1 : 0;

    pthread_mutex_unlock(&members_mutex);
    return encontrado;
}

/* Remove um membro da tabela */
int member_table_remove(const uint8_t node_id[NODE_ID_LEN])
{
    int resultado;
    int indice;

    if (node_id == NULL) {
        return MEMBER_ERR_INVALID;
    }
    if (pthread_mutex_lock(&members_mutex) != 0) {
        return MEMBER_ERR_INVALID;
    }

    indice = member_find_index(node_id);
    if (indice < 0) {
        resultado = MEMBER_ERR_NOT_FOUND;
    } else {
        members[indice] = members[member_count - 1];
        member_count--;
        resultado = MEMBER_OK;
        member_print_table();
    }

    pthread_mutex_unlock(&members_mutex);
    return resultado;
}

/* Imprime a tabela protegida pelo mutex */
void member_table_print(void)
{
    if (pthread_mutex_lock(&members_mutex) != 0) {
        return;
    }
    member_print_table();
    pthread_mutex_unlock(&members_mutex);
}
```

