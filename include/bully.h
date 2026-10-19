#ifndef BULLY_H
#define BULLY_H

#include <stdint.h>

#include "node.h"
#include "chord.h"

/* Tempos da eleicao, em segundos */
#define BULLY_WAIT_SEC 5
#define BULLY_PERIOD_SEC 1
#define BULLY_START_SEC 10
#define BULLY_ANNOUNCE_SEC 5

/* Estados do no na eleicao (Secao 15.3) */
typedef enum {
    BULLY_FOLLOWER,
    BULLY_CANDIDATE,
    BULLY_COORDINATOR
} BullyState;

/* Inicia o Bully com este no e cria a thread periodica: depois de
 * BULLY_START_SEC, a cada 1 s confere o coordenador, refaz a eleicao quando
 * preciso e, se este no e o coordenador, reanuncia a cada 5 s.
 * Retorna 0 ou -1. */
int bully_start(const Node *no, const char *nome);

/* Trata ELECTION: payload = no que envia (formato do chord_node_write).
 * Retorna 1 se quem enviou tem NodeID menor (responder OK; este no inicia a
 * propria eleicao), 0 se e maior (responder ACK) ou -1 (payload invalido).
 * Trava o mutex do Bully, sem rede. */
int bully_handle_election(const uint8_t *payload, uint32_t tamanho);

/* Trata COORDINATOR: payload = no que envia. Grava o coordenador e vira
 * FOLLOWER. Retorna 0 ou -1 (payload invalido). Trava o mutex, sem rede. */
int bully_handle_coordinator(const uint8_t *payload, uint32_t tamanho);

/* Copia o coordenador atual em out. Retorna 1 se ha coordenador, senao 0. */
int bully_get_coordinator(ChordNode *out);

#endif
