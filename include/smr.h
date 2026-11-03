#ifndef SMR_H
#define SMR_H

#include <stdint.h>

#include "node.h"
#include "metadata.h"
#include "chord.h"

/* Capacidade do log em memoria e codigo da operacao PUT */
#define SMR_LOG_MAX 1024
#define SMR_OP_PUT 1

/* Parte fixa do payload do LOG_APPEND / PREPARE, antes do metadado:
 * log_index | transaction_id | operation | object_id | timestamp | version | checksum | meta_len */
#define SMR_FIXED_LEN (8 + 16 + 1 + 32 + 8 + 8 + 32 + 4)

/* Estados da transacao no protocolo Two-Phase Commit (Secao 14.3) */
typedef enum {
    TX_IDLE = 0,
    TX_PREPARING = 1,
    TX_READY = 2,
    TX_COMMIT = 3,
    TX_ABORT = 4,
    TX_DONE = 5
} TxState;

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

/* No coordenador: executa Two-Phase Commit (PREPARE -> YES/NO -> COMMIT/ABORT)
 * com todos os Super Peers ALIVE e aplica a entrada PUT em ordem total.
 * Retorna 0 se confirmou (COMMIT) ou -1 se abortou (ABORT). */
int smr_put(const FileMetadata *meta, const uint8_t provider[NODE_ID_LEN]);

/* Trata LOG_APPEND vindo de origem (aceito so do coordenador atual).
 * Retorna 0 (responder ACK) ou -1 (responder ERROR). */
int smr_handle_log_append(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                          uint32_t tamanho);

/* Participante 2PC: trata PREPARE vindo do coordenador, valida a operacao,
 * grava a transacao pendente como READY e retorna MSG_VOTE_YES ou MSG_VOTE_NO. */
uint16_t smr_handle_prepare(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                            uint32_t tamanho);

/* Participante 2PC: trata COMMIT(TransactionID), aplica a operacao preparada
 * no log SMR e na tabela de metadados. Retorna 0 (ACK) ou -1 (ERROR). */
int smr_handle_commit(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                      uint32_t tamanho);

/* Participante 2PC: trata ABORT(TransactionID), descarta a transacao preparada.
 * Retorna 0 (ACK) ou -1 (ERROR). */
int smr_handle_abort(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                     uint32_t tamanho);

/* Coordenador IST: recebe a versao (last_applied) do no solicitante e monta
 * em *resposta (alocada; liberar com free) apenas as entradas de log faltantes
 * (from_version + 1 .. last_applied). Retorna 0 ou -1. */
int smr_handle_state_transfer(const uint8_t *payload, uint32_t tamanho,
                              uint8_t **resposta, uint32_t *tamanho_resposta);

/* Solicitante IST: pede ao coordenador as entradas de log a partir do seu
 * last_applied atual e aplica todas em sequencia ate ficar READY.
 * Retorna 0 se ja estava sincronizado ou se sincronizou com sucesso, ou -1. */
int smr_sync_with_coordinator(const ChordNode *coord);

#endif
