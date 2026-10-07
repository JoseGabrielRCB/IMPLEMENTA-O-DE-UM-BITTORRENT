#ifndef NODE_H
#define NODE_H

#include <stdint.h>
#include <stddef.h>

/* Tamanhos da identidade do no */
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

/* Le o formato posicional: superpeer <ip> <porta> <uuid> ou
 * peer <ip> <porta> <uuid> <ip_superpeer> <porta_superpeer>.
 * Retorna 0 ou -1 (e imprime o motivo em stderr). */
int node_parse_args(int argc, char *argv[], NodeConfig *config);

/* Imprime em stderr o uso do formato posicional. */
void node_print_usage(const char *program);

/* Valida a porta em texto (1 a 65535) e grava em *port. Retorna 0 ou -1. */
int node_parse_port(const char *texto, uint16_t *port);

/* Valida o IPv4 em texto e copia para destino (tamanho bytes). Retorna 0 ou -1. */
int node_parse_ip(const char *texto, char *destino, size_t tamanho);

/* Le o UUID do arquivo; se ele nao existe, gera um novo e salva no arquivo.
 * Retorna 0 ou -1. */
int node_load_uuid(const char *path, uint8_t uuid[NODE_UUID_LEN]);

/* Calcula o NodeID = SHA256(IP || porta big-endian || UUID). */
void node_compute_id(const char *ip, uint16_t port,
                     const uint8_t uuid[NODE_UUID_LEN],
                     uint8_t node_id[NODE_ID_LEN]);

/* Monta a identidade a partir da configuracao (carrega o UUID e calcula o NodeID).
 * O estado comeca em STATE_DISCONNECTED. Retorna 0 ou -1. */
int node_init(Node *node, const NodeConfig *config);

/* Compara dois NodeIDs como inteiros big-endian (memcmp).
 * Retorna negativo, zero ou positivo. */
int node_id_compare(const uint8_t a[NODE_ID_LEN], const uint8_t b[NODE_ID_LEN]);

/* Retorna 1 se o NodeID e todo zero, senao 0. */
int node_id_is_zero(const uint8_t node_id[NODE_ID_LEN]);

/* Escreve o NodeID em hexadecimal, com '\0' no fim (hex com NODE_ID_HEX_LEN bytes). */
void node_id_to_hex(const uint8_t node_id[NODE_ID_LEN], char hex[NODE_ID_HEX_LEN]);

/* Imprime o NodeID em hexadecimal, sem quebra de linha. */
void node_print_id(const uint8_t node_id[NODE_ID_LEN]);

/* Nome do papel para o log ("PEER" ou "SUPERPEER"). */
const char *node_role_name(NodeRole role);

/* Nome do estado para o log. */
const char *node_state_name(NodeState state);

/* Imprime papel, IP, porta, estado e NodeID do no. */
void node_print(const Node *node);

#endif
