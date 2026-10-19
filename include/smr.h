#ifndef SMR_H
#define SMR_H

#include <stdint.h>

#include "node.h"
#include "metadata.h"

/* Capacidade do log em memoria e codigo da operacao PUT */
#define SMR_LOG_MAX 1024
#define SMR_OP_PUT 1

/* Parte fixa do payload do LOG_APPEND, antes do metadado:
 * log_index | transaction_id | operation | object_id | timestamp | version | checksum | meta_len */
#define SMR_FIXED_LEN (8 + 16 + 1 + 32 + 8 + 8 + 32 + 4)

/* Entrada do log de operacoes (Secao 12.3) */
typedef struct {
    uint64_t log_index;          /* sequencia global, comeca em 1 */
    uint8_t  transaction_id[16];
    uint8_t  operation;          /* SMR_OP_PUT = 1 */
    uint8_t  object_id[32];
    uint64_t timestamp;          /* relogio logico de Lamport */
    uint64_t version;            /* versao do metadado apos aplicar */
    uint8_t  checksum[32];       /* SHA-256 do comando */
} SmrLogEntry;

/* Inicia o log vazio com o NodeID deste no. Retorna 0. */
int smr_start(const Node *no);

/* Retorna o ultimo LogIndex aplicado (0 se o log esta vazio). */
uint64_t smr_last_applied(void);

/* No coordenador: cria a entrada PUT com last_applied + 1, aplica o metadado
 * (provedor = provider) e manda LOG_APPEND a cada membro ALIVE, um de cada
 * vez, com smr_order_mutex travado do inicio ao fim.
 * Retorna 0 se aplicou localmente ou -1. */
int smr_put(const FileMetadata *meta, const uint8_t provider[NODE_ID_LEN]);

/* Trata LOG_APPEND vindo de origem (aceito so do coordenador atual).
 * Aplica a entrada se for a proxima, aceita sem reaplicar se ja foi aplicada.
 * Retorna 0 (responder ACK) ou -1 (responder ERROR). Nao usa rede. */
int smr_handle_log_append(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                          uint32_t tamanho);

#endif
