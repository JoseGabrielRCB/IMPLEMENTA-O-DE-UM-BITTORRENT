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
