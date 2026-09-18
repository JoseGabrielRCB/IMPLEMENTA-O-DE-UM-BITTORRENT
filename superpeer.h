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
