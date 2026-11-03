#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <openssl/sha.h>

#include "include/network.h"
#include "include/node.h"
#include "include/metadata.h"
#include "include/chord.h"
#include "include/gossip.h"
#include "include/bully.h"
#include "include/cache.h"
#include "include/smr.h"

/* Campos da entrada sem o checksum: index | txid | operation | object_id | timestamp | version */
#define SMR_FIELDS_LEN (8 + 16 + 1 + 32 + 8 + 8)

/* Maior quantidade de followers por envio */
#define SMR_MEMBERS_MAX 64

/* Entrada completa mantida em memoria (cabecalho SMR + payload serializado para IST) */
typedef struct {
    SmrLogEntry entry;
    uint8_t *raw_payload;
    uint32_t raw_len;
} SmrStoredLog;

/* Transacao pendente no participante durante o Two-Phase Commit */
typedef struct {
    TxState state;
    SmrLogEntry entry;
    FileMetadata meta;
    uint8_t provider[NODE_ID_LEN];
    uint8_t *raw_payload;
    uint32_t raw_len;
} PendingTx;

/* Log, transacao preparada, relogio de Lamport, sequencia e o mutex que os protege */
static SmrStoredLog smr_log[SMR_LOG_MAX];
static int smr_count = 0;
static uint64_t smr_applied = 0;
static uint64_t smr_clock = 0;
static uint16_t smr_seq = 0;
static uint8_t smr_self_id[NODE_ID_LEN];
static PendingTx smr_pending;
static pthread_mutex_t smr_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Ordem total no coordenador: travado do inicio ao fim do smr_put */
static pthread_mutex_t smr_order_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Imprime bytes em hexadecimal, sem quebra de linha */
static void smr_print_hex(const uint8_t *bytes, size_t tamanho)
{
    size_t i;

    for (i = 0; i < tamanho; i++) {
        printf("%02x", bytes[i]);
    }
}

/* Imprime a entrada no formato do checkpoint */
static void smr_print(const SmrLogEntry *e, const char *filename)
{
    printf("LOG #%llu\n", (unsigned long long)e->log_index);
    printf("PUT %s\n", filename);
    printf("TransactionID: ");
    smr_print_hex(e->transaction_id, sizeof(e->transaction_id));
    printf("  Version: %llu  Checksum: ", (unsigned long long)e->version);
    smr_print_hex(e->checksum, sizeof(e->checksum));
    printf("\n");
}

/* TransactionID [C-02]: segundos (48 bits) | 64 bits altos do NodeID | sequencia (16 bits) */
static void smr_make_txid(uint8_t txid[16], uint16_t seq)
{
    uint64_t segundos = (uint64_t)time(NULL);
    int i;

    for (i = 0; i < 6; i++) {
        txid[i] = (uint8_t)(segundos >> (8 * (5 - i)));
    }
    memcpy(txid + 6, smr_self_id, 8);
    txid[14] = (uint8_t)(seq >> 8);
    txid[15] = (uint8_t)seq;
}

/* Grava os campos da entrada sem o checksum (SMR_FIELDS_LEN bytes) */
static void smr_write_fields(const SmrLogEntry *e, uint8_t *buffer)
{
    uint32_t pos = 0;

    memcpy(buffer + pos, &e->log_index, 8);
    pos += 8;
    memcpy(buffer + pos, e->transaction_id, 16);
    pos += 16;
    buffer[pos++] = e->operation;
    memcpy(buffer + pos, e->object_id, 32);
    pos += 32;
    memcpy(buffer + pos, &e->timestamp, 8);
    pos += 8;
    memcpy(buffer + pos, &e->version, 8);
}

/* SHA-256 dos campos, do metadado serializado e do provedor */
static int smr_checksum(const SmrLogEntry *e, const uint8_t *meta_buf, uint32_t meta_len,
                        const uint8_t provider[NODE_ID_LEN], uint8_t saida[32])
{
    size_t tamanho = SMR_FIELDS_LEN + (size_t)meta_len + NODE_ID_LEN;
    uint8_t *buffer = (uint8_t *)malloc(tamanho);

    if (buffer == NULL) {
        return -1;
    }
    smr_write_fields(e, buffer);
    memcpy(buffer + SMR_FIELDS_LEN, meta_buf, meta_len);
    memcpy(buffer + SMR_FIELDS_LEN + meta_len, provider, NODE_ID_LEN);
    SHA256(buffer, tamanho, saida);
    free(buffer);
    return 0;
}

/* Monta o payload do LOG_APPEND / PREPARE num buffer alocado (liberar com free) */
static uint8_t *smr_write_payload(const SmrLogEntry *e, const uint8_t *meta_buf, uint32_t meta_len,
                                  const uint8_t provider[NODE_ID_LEN], uint32_t *tamanho)
{
    uint32_t total = SMR_FIXED_LEN + meta_len + NODE_ID_LEN;
    uint8_t *payload = (uint8_t *)malloc(total);

    if (payload == NULL) {
        return NULL;
    }
    smr_write_fields(e, payload);
    memcpy(payload + SMR_FIELDS_LEN, e->checksum, 32);
    memcpy(payload + SMR_FIELDS_LEN + 32, &meta_len, 4);
    memcpy(payload + SMR_FIXED_LEN, meta_buf, meta_len);
    memcpy(payload + SMR_FIXED_LEN + meta_len, provider, NODE_ID_LEN);
    *tamanho = total;
    return payload;
}

/* Le o payload do LOG_APPEND / PREPARE; meta_buf aponta para dentro do payload */
static int smr_read_payload(const uint8_t *payload, uint32_t tamanho, SmrLogEntry *e,
                            const uint8_t **meta_buf, uint32_t *meta_len,
                            uint8_t provider[NODE_ID_LEN])
{
    uint32_t pos = 0;

    if (payload == NULL || tamanho < SMR_FIXED_LEN + NODE_ID_LEN) {
        return -1;
    }
    memset(e, 0, sizeof(*e));
    memcpy(&e->log_index, payload + pos, 8);
    pos += 8;
    memcpy(e->transaction_id, payload + pos, 16);
    pos += 16;
    e->operation = payload[pos++];
    memcpy(e->object_id, payload + pos, 32);
    pos += 32;
    memcpy(&e->timestamp, payload + pos, 8);
    pos += 8;
    memcpy(&e->version, payload + pos, 8);
    pos += 8;
    memcpy(e->checksum, payload + pos, 32);
    pos += 32;
    memcpy(meta_len, payload + pos, 4);

    if (e->log_index == 0 || *meta_len != tamanho - SMR_FIXED_LEN - NODE_ID_LEN) {
        return -1;
    }
    *meta_buf = payload + SMR_FIXED_LEN;
    memcpy(provider, payload + SMR_FIXED_LEN + *meta_len, NODE_ID_LEN);
    return 0;
}

/* Limpa qualquer transacao pendente; usar com smr_mutex travado */
static void smr_clear_pending_locked(void)
{
    metadata_free(&smr_pending.meta);
    free(smr_pending.raw_payload);
    memset(&smr_pending, 0, sizeof(smr_pending));
    smr_pending.state = TX_IDLE;
}

/* Aplica a entrada no banco de metadados, atualiza a cache LFU e guarda no log;
 * usar com smr_mutex travado */
static int smr_apply_locked(const SmrLogEntry *e, const FileMetadata *meta,
                            const uint8_t provider[NODE_ID_LEN],
                            const uint8_t *raw_payload, uint32_t raw_len,
                            int populate_cache)
{
    uint8_t *copia_raw = NULL;

    if (smr_count >= SMR_LOG_MAX) {
        printf("SMR: log cheio, LOG #%llu descartado\n", (unsigned long long)e->log_index);
        return -1;
    }
    if (raw_payload != NULL && raw_len > 0) {
        copia_raw = (uint8_t *)malloc(raw_len);
        if (copia_raw == NULL) {
            return -1;
        }
        memcpy(copia_raw, raw_payload, raw_len);
    }

    smr_print(e, meta->filename);
    if (metadata_table_insert(meta) != METADATA_OK) {
        printf("SMR: falha ao aplicar LOG #%llu\n", (unsigned long long)e->log_index);
        free(copia_raw);
        return -1;
    }
    metadata_add_provider(meta->object_id, provider);
    if (populate_cache) {
        cache_put(meta);
    }

    smr_log[smr_count].entry = *e;
    smr_log[smr_count].raw_payload = copia_raw;
    smr_log[smr_count].raw_len = raw_len;
    smr_count++;
    smr_applied = e->log_index;
    return 0;
}

/* Envia uma mensagem do protocolo SMR/2PC a um Super Peer e devolve o tipo da resposta */
static int smr_send(const ChordNode *destino, uint16_t tipo_msg, const uint8_t txid[16],
                    uint64_t timestamp, uint8_t *payload, uint32_t tamanho,
                    uint16_t *tipo_resposta)
{
    Mensagem pedido;
    Mensagem resposta;
    int fd;
    int resultado;

    fd = conectar_no_servidor(destino->ip, destino->port);
    if (fd < 0) {
        return -1;
    }

    memset(&pedido, 0, sizeof(pedido));
    memset(&resposta, 0, sizeof(resposta));
    pedido.header.versao_protocolo = VERSAO_PROTOCOLO;
    pedido.header.tipo_mensagem = tipo_msg;
    memcpy(pedido.header.no_origem, smr_self_id, NODE_ID_LEN);
    memcpy(pedido.header.no_destino, destino->id, NODE_ID_LEN);
    if (txid != NULL) {
        memcpy(pedido.header.id_transacao, txid, 16);
    }
    pedido.header.timestamp = timestamp;
    pedido.header.tamanho_payload = tamanho;
    pedido.payload = payload;

    resultado = enviar_mensagem(fd, &pedido);
    if (resultado == 0) {
        resultado = receber_mensagem(fd, &resposta);
    }
    close(fd);

    if (resultado != 0) {
        return -1;
    }
    *tipo_resposta = resposta.header.tipo_mensagem;
    liberar_mensagem(&resposta);
    return 0;
}

/* Guarda o NodeID deste no */
int smr_start(const Node *no)
{
    memcpy(smr_self_id, no->node_id, NODE_ID_LEN);
    memset(&smr_pending, 0, sizeof(smr_pending));
    smr_pending.state = TX_IDLE;
    printf("SMR: log de operacoes em memoria (ate %d entradas)\n", SMR_LOG_MAX);
    return 0;
}

/* Ultimo LogIndex aplicado */
uint64_t smr_last_applied(void)
{
    uint64_t valor = 0;

    if (pthread_mutex_lock(&smr_mutex) != 0) {
        return 0;
    }
    valor = smr_applied;
    pthread_mutex_unlock(&smr_mutex);
    return valor;
}

/* Coordenador: executa Two-Phase Commit (PREPARE -> YES/NO -> COMMIT/ABORT) e aplica em ordem total */
int smr_put(const FileMetadata *meta, const uint8_t provider[NODE_ID_LEN])
{
    ChordNode membros[SMR_MEMBERS_MAX];
    SmrLogEntry e;
    uint8_t *meta_buf = NULL;
    uint32_t meta_len = 0;
    uint8_t *payload = NULL;
    uint32_t tamanho = 0;
    uint16_t resposta = 0;
    const char *sim_fail;
    int total;
    int todos_yes = 1;
    int resultado = -1;
    int i;

    if (meta == NULL || provider == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&smr_order_mutex) != 0) {
        return -1;
    }
    if (metadata_serialize(meta, &meta_buf, &meta_len) != METADATA_OK) {
        pthread_mutex_unlock(&smr_order_mutex);
        return -1;
    }

    if (pthread_mutex_lock(&smr_mutex) != 0) {
        free(meta_buf);
        pthread_mutex_unlock(&smr_order_mutex);
        return -1;
    }
    memset(&e, 0, sizeof(e));
    e.log_index = smr_applied + 1;
    e.timestamp = ++smr_clock;
    smr_make_txid(e.transaction_id, ++smr_seq);
    e.operation = SMR_OP_PUT;
    memcpy(e.object_id, meta->object_id, 32);
    e.version = meta->version;
    pthread_mutex_unlock(&smr_mutex);

    if (smr_checksum(&e, meta_buf, meta_len, provider, e.checksum) != 0) {
        free(meta_buf);
        pthread_mutex_unlock(&smr_order_mutex);
        return -1;
    }
    payload = smr_write_payload(&e, meta_buf, meta_len, provider, &tamanho);
    if (payload == NULL) {
        free(meta_buf);
        pthread_mutex_unlock(&smr_order_mutex);
        return -1;
    }

    /* Fase 1 do 2PC: PREPARE(TransactionID) para todos os participantes ALIVE */
    total = gossip_alive_members(membros, SMR_MEMBERS_MAX);
    printf("2PC [IDLE -> PREPARING] LOG #%llu TransactionID=", (unsigned long long)e.log_index);
    smr_print_hex(e.transaction_id, 16);
    printf(" (%d participantes)\n", total);

    sim_fail = getenv("SIMULATE_PREPARE_FAIL");
    if (sim_fail != NULL && strcmp(sim_fail, "1") == 0) {
        printf("2PC: falha simulada durante a fase PREPARE\n");
        todos_yes = 0;
    }

    for (i = 0; i < total && todos_yes; i++) {
        printf("2PC PREPARE -> %s\n", membros[i].name);
        if (smr_send(&membros[i], MSG_PREPARE, e.transaction_id, e.timestamp,
                     payload, tamanho, &resposta) == 0 &&
            (resposta == MSG_VOTE_YES || resposta == MSG_ACK)) {
            printf("2PC YES <- %s\n", membros[i].name);
        } else {
            printf("2PC NO <- %s\n", membros[i].name);
            todos_yes = 0;
        }
    }

    /* Fase 2 do 2PC: COMMIT se todos votaram YES, caso contrario ABORT */
    if (todos_yes) {
        printf("2PC [READY -> COMMIT] LOG #%llu confirmado\n", (unsigned long long)e.log_index);
        if (pthread_mutex_lock(&smr_mutex) == 0) {
            resultado = smr_apply_locked(&e, meta, provider, payload, tamanho, 1);
            pthread_mutex_unlock(&smr_mutex);
        }
        if (resultado == 0) {
            for (i = 0; i < total; i++) {
                printf("2PC COMMIT -> %s\n", membros[i].name);
                smr_send(&membros[i], MSG_COMMIT, e.transaction_id, e.timestamp,
                         e.transaction_id, 16, &resposta);
            }
            printf("2PC [COMMIT -> DONE] LOG #%llu\n", (unsigned long long)e.log_index);
        }
    } else {
        printf("2PC [PREPARING -> ABORT] LOG #%llu abortado\n", (unsigned long long)e.log_index);
        for (i = 0; i < total; i++) {
            printf("2PC ABORT -> %s\n", membros[i].name);
            smr_send(&membros[i], MSG_ABORT, e.transaction_id, e.timestamp,
                     e.transaction_id, 16, &resposta);
        }
        resultado = -1;
    }

    free(payload);
    free(meta_buf);
    pthread_mutex_unlock(&smr_order_mutex);
    return resultado;
}

/* Confere a entrada recebida e aplica se for a proxima; usar com smr_mutex travado */
static int smr_append_locked(const SmrLogEntry *e, const FileMetadata *meta,
                             const uint8_t provider[NODE_ID_LEN],
                             const uint8_t *raw_payload, uint32_t raw_len)
{
    if (e->log_index <= smr_applied) {
        return 0;
    }
    if (e->log_index > smr_applied + 1) {
        printf("LOG #%llu recusado: esperado #%llu\n", (unsigned long long)e->log_index,
               (unsigned long long)(smr_applied + 1));
        return -1;
    }
    smr_clock = ((e->timestamp > smr_clock) ? e->timestamp : smr_clock) + 1;
    return smr_apply_locked(e, meta, provider, raw_payload, raw_len, 0);
}

/* Valida o payload completo de uma entrada (usado por LOG_APPEND e PREPARE) */
static int smr_validate_entry(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                              uint32_t tamanho, SmrLogEntry *e, FileMetadata *meta,
                              uint8_t provider[NODE_ID_LEN])
{
    ChordNode coord;
    const uint8_t *meta_buf = NULL;
    uint32_t meta_len = 0;
    uint8_t calculado[32];

    if (!bully_get_coordinator(&coord) || node_id_compare(coord.id, origem) != 0) {
        printf("SMR recusado: origem nao e o coordenador\n");
        return -1;
    }
    if (smr_read_payload(payload, tamanho, e, &meta_buf, &meta_len, provider) != 0 ||
        e->operation != SMR_OP_PUT) {
        printf("SMR recusado: payload invalido\n");
        return -1;
    }
    if (smr_checksum(e, meta_buf, meta_len, provider, calculado) != 0 ||
        memcmp(calculado, e->checksum, sizeof(calculado)) != 0) {
        printf("SMR #%llu recusado: checksum diferente\n", (unsigned long long)e->log_index);
        return -1;
    }
    if (metadata_deserialize(meta_buf, meta_len, meta) != METADATA_OK) {
        printf("SMR recusado: metadado invalido\n");
        return -1;
    }
    if (memcmp(meta->object_id, e->object_id, 32) != 0) {
        printf("SMR recusado: ObjectID diferente do metadado\n");
        metadata_free(meta);
        return -1;
    }
    return 0;
}

/* Follower: valida o LOG_APPEND do coordenador e aplica na ordem */
int smr_handle_log_append(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                          uint32_t tamanho)
{
    SmrLogEntry e;
    FileMetadata meta;
    uint8_t provider[NODE_ID_LEN];
    int resultado;

    if (smr_validate_entry(origem, payload, tamanho, &e, &meta, provider) != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&smr_mutex) != 0) {
        metadata_free(&meta);
        return -1;
    }
    resultado = smr_append_locked(&e, &meta, provider, payload, tamanho);
    pthread_mutex_unlock(&smr_mutex);
    metadata_free(&meta);
    return resultado;
}

/* Participante 2PC: Fase 1 (PREPARE -> YES ou NO) */
uint16_t smr_handle_prepare(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                            uint32_t tamanho)
{
    SmrLogEntry e;
    FileMetadata meta;
    uint8_t provider[NODE_ID_LEN];
    uint8_t *copia_raw;
    const char *sim_no;

    sim_no = getenv("SIMULATE_VOTE_NO");
    if (sim_no != NULL && strcmp(sim_no, "1") == 0) {
        printf("2PC PREPARE recusado: voto NO simulado (ABORT)\n");
        return MSG_VOTE_NO;
    }

    if (smr_validate_entry(origem, payload, tamanho, &e, &meta, provider) != 0) {
        printf("2PC PREPARE -> NO (validacao falhou)\n");
        return MSG_VOTE_NO;
    }

    copia_raw = (uint8_t *)malloc(tamanho);
    if (copia_raw == NULL) {
        metadata_free(&meta);
        return MSG_VOTE_NO;
    }
    memcpy(copia_raw, payload, tamanho);

    if (pthread_mutex_lock(&smr_mutex) != 0) {
        free(copia_raw);
        metadata_free(&meta);
        return MSG_VOTE_NO;
    }

    if (e.log_index > smr_applied + 1) {
        printf("2PC PREPARE #%llu -> NO (esperado #%llu)\n",
               (unsigned long long)e.log_index, (unsigned long long)(smr_applied + 1));
        pthread_mutex_unlock(&smr_mutex);
        free(copia_raw);
        metadata_free(&meta);
        return MSG_VOTE_NO;
    }

    smr_clear_pending_locked();
    smr_pending.state = TX_READY;
    smr_pending.entry = e;
    smr_pending.meta = meta;
    memcpy(smr_pending.provider, provider, NODE_ID_LEN);
    smr_pending.raw_payload = copia_raw;
    smr_pending.raw_len = tamanho;

    printf("2PC PREPARE #%llu -> YES (estado READY)\n", (unsigned long long)e.log_index);
    pthread_mutex_unlock(&smr_mutex);
    return MSG_VOTE_YES;
}

/* Participante 2PC: Fase 2 (COMMIT) */
int smr_handle_commit(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                      uint32_t tamanho)
{
    ChordNode coord;
    int resultado = -1;

    if (payload == NULL || tamanho != 16) {
        return -1;
    }
    if (!bully_get_coordinator(&coord) || node_id_compare(coord.id, origem) != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&smr_mutex) != 0) {
        return -1;
    }

    if (smr_pending.state == TX_READY &&
        memcmp(smr_pending.entry.transaction_id, payload, 16) == 0) {
        printf("2PC COMMIT #%llu recebido -> aplicando operacao\n",
               (unsigned long long)smr_pending.entry.log_index);
        resultado = smr_append_locked(&smr_pending.entry, &smr_pending.meta,
                                      smr_pending.provider, smr_pending.raw_payload,
                                      smr_pending.raw_len);
        smr_clear_pending_locked();
    } else {
        printf("2PC COMMIT ignorado: sem transacao READY correspondente\n");
    }

    pthread_mutex_unlock(&smr_mutex);
    return resultado;
}

/* Participante 2PC: Fase 2 (ABORT) */
int smr_handle_abort(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                     uint32_t tamanho)
{
    ChordNode coord;

    if (payload == NULL || tamanho != 16) {
        return -1;
    }
    if (!bully_get_coordinator(&coord) || node_id_compare(coord.id, origem) != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&smr_mutex) != 0) {
        return -1;
    }

    if (smr_pending.state == TX_READY &&
        memcmp(smr_pending.entry.transaction_id, payload, 16) == 0) {
        printf("2PC ABORT #%llu recebido -> descartando transacao preparada\n",
               (unsigned long long)smr_pending.entry.log_index);
        smr_clear_pending_locked();
    } else {
        printf("2PC ABORT recebido -> estado limpo (IDLE)\n");
    }

    pthread_mutex_unlock(&smr_mutex);
    return 0;
}

/* Coordenador IST: envia apenas os logs posteriores a versao do solicitante (Secao 13.3) */
int smr_handle_state_transfer(const uint8_t *payload, uint32_t tamanho,
                              uint8_t **resposta, uint32_t *tamanho_resposta)
{
    uint64_t from_version;
    uint32_t count = 0;
    uint32_t total_bytes = sizeof(uint64_t) + sizeof(uint32_t);
    uint8_t *out;
    uint32_t pos = 0;
    int i;

    if (payload == NULL || tamanho != sizeof(uint64_t) ||
        resposta == NULL || tamanho_resposta == NULL) {
        return -1;
    }
    memcpy(&from_version, payload, sizeof(uint64_t));

    if (pthread_mutex_lock(&smr_mutex) != 0) {
        return -1;
    }

    for (i = 0; i < smr_count; i++) {
        if (smr_log[i].entry.log_index > from_version && smr_log[i].raw_payload != NULL) {
            total_bytes += sizeof(uint32_t) + smr_log[i].raw_len;
            count++;
        }
    }

    out = (uint8_t *)malloc(total_bytes);
    if (out == NULL) {
        pthread_mutex_unlock(&smr_mutex);
        return -1;
    }

    memcpy(out + pos, &smr_applied, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    memcpy(out + pos, &count, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    for (i = 0; i < smr_count; i++) {
        if (smr_log[i].entry.log_index > from_version && smr_log[i].raw_payload != NULL) {
            memcpy(out + pos, &smr_log[i].raw_len, sizeof(uint32_t));
            pos += sizeof(uint32_t);
            memcpy(out + pos, smr_log[i].raw_payload, smr_log[i].raw_len);
            pos += smr_log[i].raw_len;
        }
    }

    if (count > 0) {
        printf("IST: enviando %u logs pendentes (versao %llu -> %llu)\n",
               count, (unsigned long long)(from_version + 1), (unsigned long long)smr_applied);
    }

    pthread_mutex_unlock(&smr_mutex);
    *resposta = out;
    *tamanho_resposta = total_bytes;
    return 0;
}

/* Solicitante IST: sincroniza logs faltantes com o coordenador */
int smr_sync_with_coordinator(const ChordNode *coord)
{
    uint64_t local_ver;
    uint64_t coord_ver = 0;
    uint32_t count = 0;
    uint32_t pos = 0;
    uint32_t i;
    Mensagem pedido;
    Mensagem resposta;
    int fd;
    int resultado;

    if (coord == NULL || node_id_compare(coord->id, smr_self_id) == 0) {
        return 0;
    }

    local_ver = smr_last_applied();
    fd = conectar_no_servidor(coord->ip, coord->port);
    if (fd < 0) {
        return -1;
    }

    memset(&pedido, 0, sizeof(pedido));
    memset(&resposta, 0, sizeof(resposta));
    pedido.header.versao_protocolo = VERSAO_PROTOCOLO;
    pedido.header.tipo_mensagem = MSG_STATE_TRANSFER;
    memcpy(pedido.header.no_origem, smr_self_id, NODE_ID_LEN);
    memcpy(pedido.header.no_destino, coord->id, NODE_ID_LEN);
    pedido.header.timestamp = (uint64_t)time(NULL);
    pedido.header.tamanho_payload = sizeof(uint64_t);
    pedido.payload = (uint8_t *)&local_ver;

    resultado = enviar_mensagem(fd, &pedido);
    if (resultado == 0) {
        resultado = receber_mensagem(fd, &resposta);
    }
    close(fd);

    if (resultado != 0 || resposta.header.tipo_mensagem != MSG_ACK ||
        resposta.payload == NULL ||
        resposta.header.tamanho_payload < sizeof(uint64_t) + sizeof(uint32_t)) {
        liberar_mensagem(&resposta);
        return -1;
    }

    memcpy(&coord_ver, resposta.payload + pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    memcpy(&count, resposta.payload + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (count == 0) {
        printf("IST: STATE_TRANSFER verificado com %s (local version=%llu, coordinator version=%llu, READY)\n",
               coord->name, (unsigned long long)local_ver, (unsigned long long)coord_ver);
        liberar_mensagem(&resposta);
        return 0;
    }

    printf("IST: SYNCHRONIZING (local version=%llu, coordinator version=%llu, versions missing=%u)\n",
           (unsigned long long)local_ver, (unsigned long long)coord_ver, count);
    printf("IST: receive logs (%llu .. %llu) de %s\n",
           (unsigned long long)(local_ver + 1), (unsigned long long)coord_ver, coord->name);

    for (i = 0; i < count; i++) {
        uint32_t entry_len = 0;
        SmrLogEntry e;
        FileMetadata meta;
        const uint8_t *meta_buf = NULL;
        uint32_t meta_len = 0;
        uint8_t provider[NODE_ID_LEN];
        uint8_t calculado[32];

        if (pos + sizeof(uint32_t) > resposta.header.tamanho_payload) {
            liberar_mensagem(&resposta);
            return -1;
        }
        memcpy(&entry_len, resposta.payload + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + entry_len > resposta.header.tamanho_payload) {
            liberar_mensagem(&resposta);
            return -1;
        }

        if (smr_read_payload(resposta.payload + pos, entry_len, &e,
                             &meta_buf, &meta_len, provider) != 0 ||
            smr_checksum(&e, meta_buf, meta_len, provider, calculado) != 0 ||
            memcmp(calculado, e.checksum, sizeof(calculado)) != 0 ||
            metadata_deserialize(meta_buf, meta_len, &meta) != METADATA_OK) {
            liberar_mensagem(&resposta);
            return -1;
        }

        if (pthread_mutex_lock(&smr_mutex) == 0) {
            smr_append_locked(&e, &meta, provider, resposta.payload + pos, entry_len);
            pthread_mutex_unlock(&smr_mutex);
        }
        metadata_free(&meta);
        pos += entry_len;
    }

    printf("IST: READY (sincronizado ate LOG #%llu)\n", (unsigned long long)smr_last_applied());
    liberar_mensagem(&resposta);
    return 0;
}
