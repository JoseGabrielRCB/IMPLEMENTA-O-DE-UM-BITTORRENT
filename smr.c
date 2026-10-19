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
#include "include/smr.h"

/* Campos da entrada sem o checksum: index | txid | operation | object_id | timestamp | version */
#define SMR_FIELDS_LEN (8 + 16 + 1 + 32 + 8 + 8)

/* Maior quantidade de followers por envio */
#define SMR_MEMBERS_MAX 64

/* Log, relogio de Lamport, sequencia e o mutex que os protege */
static SmrLogEntry smr_log[SMR_LOG_MAX];
static int smr_count = 0;
static uint64_t smr_applied = 0;
static uint64_t smr_clock = 0;
static uint16_t smr_seq = 0;
static uint8_t smr_self_id[NODE_ID_LEN];
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

/* Monta o payload do LOG_APPEND num buffer alocado (liberar com free) */
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

/* Le o payload do LOG_APPEND; meta_buf aponta para dentro do payload */
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

/* Aplica a entrada (igual ao STORE) e avanca o log; usar com smr_mutex travado */
static int smr_apply_locked(const SmrLogEntry *e, const FileMetadata *meta,
                            const uint8_t provider[NODE_ID_LEN])
{
    if (smr_count >= SMR_LOG_MAX) {
        printf("SMR: log cheio, LOG #%llu descartado\n", (unsigned long long)e->log_index);
        return -1;
    }
    smr_print(e, meta->filename);
    if (metadata_table_insert(meta) != METADATA_OK) {
        printf("SMR: falha ao aplicar LOG #%llu\n", (unsigned long long)e->log_index);
        return -1;
    }
    metadata_add_provider(meta->object_id, provider);
    smr_log[smr_count++] = *e;
    smr_applied = e->log_index;
    return 0;
}

/* Cria a entrada com last_applied + 1 e aplica localmente */
static int smr_put_local(const FileMetadata *meta, const uint8_t *meta_buf, uint32_t meta_len,
                         const uint8_t provider[NODE_ID_LEN], SmrLogEntry *e)
{
    int resultado;

    if (pthread_mutex_lock(&smr_mutex) != 0) {
        return -1;
    }
    memset(e, 0, sizeof(*e));
    e->log_index = smr_applied + 1;
    e->timestamp = ++smr_clock;
    smr_make_txid(e->transaction_id, ++smr_seq);
    e->operation = SMR_OP_PUT;
    memcpy(e->object_id, meta->object_id, 32);
    e->version = meta->version;

    resultado = smr_checksum(e, meta_buf, meta_len, provider, e->checksum);
    if (resultado == 0) {
        resultado = smr_apply_locked(e, meta, provider);
    }
    pthread_mutex_unlock(&smr_mutex);
    return resultado;
}

/* Envia o LOG_APPEND a um follower e guarda o tipo da resposta */
static int smr_send(const ChordNode *destino, const SmrLogEntry *e, uint8_t *payload,
                    uint32_t tamanho, uint16_t *tipo_resposta)
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
    pedido.header.tipo_mensagem = MSG_LOG_APPEND;
    memcpy(pedido.header.no_origem, smr_self_id, NODE_ID_LEN);
    memcpy(pedido.header.no_destino, destino->id, NODE_ID_LEN);
    memcpy(pedido.header.id_transacao, e->transaction_id, 16);
    pedido.header.timestamp = e->timestamp;
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

/* Manda a entrada a cada membro ALIVE, um de cada vez */
static void smr_replicate(const SmrLogEntry *e, const uint8_t *meta_buf, uint32_t meta_len,
                          const uint8_t provider[NODE_ID_LEN])
{
    ChordNode membros[SMR_MEMBERS_MAX];
    uint32_t tamanho = 0;
    uint16_t tipo = 0;
    uint8_t *payload;
    int total;
    int i;

    payload = smr_write_payload(e, meta_buf, meta_len, provider, &tamanho);
    if (payload == NULL) {
        return;
    }
    total = gossip_alive_members(membros, SMR_MEMBERS_MAX);
    for (i = 0; i < total; i++) {
        if (smr_send(&membros[i], e, payload, tamanho, &tipo) != 0) {
            printf("LOG_APPEND #%llu: %s nao respondeu\n", (unsigned long long)e->log_index,
                   membros[i].name);
        } else if (tipo != MSG_ACK) {
            printf("LOG_APPEND #%llu: %s recusou\n", (unsigned long long)e->log_index,
                   membros[i].name);
        }
    }
    free(payload);
}

/* Guarda o NodeID deste no */
int smr_start(const Node *no)
{
    memcpy(smr_self_id, no->node_id, NODE_ID_LEN);
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

/* Coordenador: registra, aplica e replica o PUT em ordem total */
int smr_put(const FileMetadata *meta, const uint8_t provider[NODE_ID_LEN])
{
    SmrLogEntry e;
    uint8_t *meta_buf = NULL;
    uint32_t meta_len = 0;
    int resultado = -1;

    if (meta == NULL || provider == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&smr_order_mutex) != 0) {
        return -1;
    }
    if (metadata_serialize(meta, &meta_buf, &meta_len) == METADATA_OK) {
        resultado = smr_put_local(meta, meta_buf, meta_len, provider, &e);
        if (resultado == 0) {
            smr_replicate(&e, meta_buf, meta_len, provider);
        }
        free(meta_buf);
    }
    pthread_mutex_unlock(&smr_order_mutex);
    return resultado;
}

/* Confere a entrada recebida e aplica se for a proxima; usar com smr_mutex travado */
static int smr_append_locked(const SmrLogEntry *e, const FileMetadata *meta,
                             const uint8_t provider[NODE_ID_LEN])
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
    return smr_apply_locked(e, meta, provider);
}

/* Follower: valida o LOG_APPEND do coordenador e aplica na ordem */
int smr_handle_log_append(const uint8_t origem[NODE_ID_LEN], const uint8_t *payload,
                          uint32_t tamanho)
{
    SmrLogEntry e;
    ChordNode coord;
    FileMetadata meta;
    const uint8_t *meta_buf = NULL;
    uint32_t meta_len = 0;
    uint8_t provider[NODE_ID_LEN];
    uint8_t calculado[32];
    int resultado;

    if (!bully_get_coordinator(&coord) || node_id_compare(coord.id, origem) != 0) {
        printf("LOG_APPEND recusado: origem nao e o coordenador\n");
        return -1;
    }
    if (smr_read_payload(payload, tamanho, &e, &meta_buf, &meta_len, provider) != 0 ||
        e.operation != SMR_OP_PUT) {
        printf("LOG_APPEND recusado: payload invalido\n");
        return -1;
    }
    if (smr_checksum(&e, meta_buf, meta_len, provider, calculado) != 0 ||
        memcmp(calculado, e.checksum, sizeof(calculado)) != 0) {
        printf("LOG_APPEND #%llu recusado: checksum diferente\n", (unsigned long long)e.log_index);
        return -1;
    }
    if (metadata_deserialize(meta_buf, meta_len, &meta) != METADATA_OK) {
        printf("LOG_APPEND recusado: metadado invalido\n");
        return -1;
    }
    if (memcmp(meta.object_id, e.object_id, 32) != 0) {
        printf("LOG_APPEND recusado: ObjectID diferente do metadado\n");
        metadata_free(&meta);
        return -1;
    }

    if (pthread_mutex_lock(&smr_mutex) != 0) {
        metadata_free(&meta);
        return -1;
    }
    resultado = smr_append_locked(&e, &meta, provider);
    pthread_mutex_unlock(&smr_mutex);
    metadata_free(&meta);
    return resultado;
}
