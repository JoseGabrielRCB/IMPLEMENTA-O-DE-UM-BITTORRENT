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
