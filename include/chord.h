#ifndef CHORD_H
#define CHORD_H

#include <stdint.h>
#include <stddef.h>

#include "node.h"

/* Tamanho do nome do no e do texto das respostas de CHORD_LOOKUP e TOPOLOGY */
#define CHORD_NAME_LEN 64
#define CHORD_TEXT_LEN 32768

/* Maior no serializado: id + porta + ip_len + ip + name_len + nome */
#define CHORD_NODE_MAX (NODE_ID_LEN + 2 + 1 + (NODE_IP_LEN - 1) + 1 + (CHORD_NAME_LEN - 1))

/* No do anel: NodeID, endereco e nome usado no log */
typedef struct {
    uint8_t id[NODE_ID_LEN];
    char ip[NODE_IP_LEN];
    uint16_t port;
    char name[CHORD_NAME_LEN];
} ChordNode;

/* Retorna 1 se os dois nos tem o mesmo NodeID, senao 0. */
int chord_same(const ChordNode *a, const ChordNode *b);

/* Serializa o no (id | port | ip_len | ip | name_len | name, sem '\0') em
 * buffer, que precisa de CHORD_NODE_MAX bytes. Retorna os bytes escritos. */
uint32_t chord_node_write(const ChordNode *no, uint8_t *buffer);

/* Retorna 1 se ainda ha n bytes a partir de pos num buffer de size bytes. */
int chord_has_bytes(uint32_t size, uint32_t pos, uint32_t n);

/* Le um no a partir de *pos e avanca *pos; valida ip_len (7 a 15), IP,
 * porta diferente de zero e name_len (1 a 63). Retorna 0 ou -1. */
int chord_node_read(const uint8_t *buffer, uint32_t size, uint32_t *pos, ChordNode *no);

/* Inicia o estado do anel com este no e cria a thread periodica (join pelo
 * bootstrap, depois stabilize e fix_fingers a cada 1 s). Sem bootstrap
 * (porta 0) ou com o bootstrap igual a este no, cria um anel novo.
 * Retorna 0 ou -1. */
int chord_start(const Node *no, const char *nome, const char *bootstrap_ip,
                uint16_t bootstrap_port);

/* Trata FIND_SUCCESSOR: payload = id[32]. Escreve em resposta
 * found (uint8) + no (o successor se found = 1, o proximo a perguntar se 0).
 * Retorna 0 ou -1 (payload invalido). Trava o mutex do anel, sem rede. */
int chord_handle_find_successor(const uint8_t *payload, uint32_t tamanho,
                                uint8_t resposta[1 + CHORD_NODE_MAX], uint32_t *tamanho_resposta);

/* Trata GET_PREDECESSOR: payload vazio. Escreve has (uint8) + no, se has = 1.
 * Retorna 0 ou -1. Trava o mutex do anel, sem rede. */
int chord_handle_get_predecessor(uint32_t tamanho, uint8_t resposta[1 + CHORD_NODE_MAX],
                                 uint32_t *tamanho_resposta);

/* Trata NOTIFY: payload = no que avisa. Ele vira predecessor se nao ha
 * predecessor ou se esta em (predecessor, self). Retorna 0 ou -1. */
int chord_handle_notify(const uint8_t *payload, uint32_t tamanho);

/* Trata CHORD_LOOKUP: payload = object_id[32]. Faz o find_successor iterativo
 * (usa rede, ate 32 saltos) e escreve em texto o "Lookup path" e o "Owner",
 * tambem impressos no log. Retorna 0 ou -1. */
int chord_handle_lookup(const uint8_t *payload, uint32_t tamanho, char *texto, size_t tamanho_texto);

/* Trata TOPOLOGY: payload vazio. Escreve em texto o no, o successor, o
 * predecessor e a finger table (nos distintos), tambem impressos no log.
 * Retorna 0 ou -1. */
int chord_handle_topology(uint32_t tamanho, char *texto, size_t tamanho_texto);

#endif
