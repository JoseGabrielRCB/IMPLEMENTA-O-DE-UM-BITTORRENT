#ifndef GOSSIP_H
#define GOSSIP_H

#include <stdint.h>

#include "node.h"
#include "chord.h"

/* Estados de um Super Peer na membership */
typedef enum {
    GOSSIP_ALIVE = 0,
    GOSSIP_SUSPECT = 1,
    GOSSIP_FAILED = 2,
    GOSSIP_REMOVED = 3
} GossipState;

/* Inicia a membership dos Super Peers e cria a thread periodica:
 * a cada 5 s manda HEARTBEAT para os membros vivos e troca a tabela (GOSSIP)
 * com um deles (ou com o bootstrap enquanto nao conhece ninguem); a cada 1 s
 * avanca ALIVE -> SUSPECT (15 s sem heartbeat) -> FAILED (30 s) -> REMOVED (45 s).
 * Retorna 0 ou -1. */
int gossip_start(const Node *no, const char *nome, const char *bootstrap_ip,
                 uint16_t bootstrap_port);

/* Trata HEARTBEAT: payload = no (formato do chord_node_write) + contador (uint64).
 * Renova o prazo de quem enviou ou o acrescenta na membership.
 * Retorna 0 ou -1 (payload invalido). Trava o mutex da membership, sem rede. */
int gossip_handle_heartbeat(const uint8_t *payload, uint32_t tamanho);

/* Trata GOSSIP: payload = count (uint8) + count x (no | estado (uint8) | contador (uint64)).
 * Valida a tabela inteira, mescla com a local e escreve a tabela local em
 * *resposta (alocada; quem chamou libera com free). Retorna 0 ou -1. */
int gossip_handle_gossip(const uint8_t *payload, uint32_t tamanho, uint8_t **resposta,
                         uint32_t *tamanho_resposta);

/* Copia em out (espaco para max nos) os membros ALIVE, sem este no.
 * Retorna quantos copiou. Trava o mutex da membership, sem rede. */
int gossip_alive_members(ChordNode *out, int max);

/* Retorna o estado (GossipState) do membro com esse NodeID, ou -1 se ele
 * nao esta na membership. Trava o mutex da membership, sem rede. */
int gossip_member_state(const uint8_t id[NODE_ID_LEN]);

#endif
