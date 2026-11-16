#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <lz4.h>

#include "include/node.h"
#include "include/network.h"
#include "include/metadata.h"
#include "include/chord.h"
#include "include/gossip.h"
#include "include/bully.h"
#include "include/smr.h"
#include "include/cache.h"

/* Valores usados quando o no e iniciado pelas opcoes longas */
#define IP_PADRAO "127.0.0.1"
#define PORTA_PADRAO "55101"
#define NOME_PADRAO "superpeer"
#define NOME_MAX 64
#define LINHA_MAX 256

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
int member_table_get_provider_info(const uint8_t node_id[NODE_ID_LEN], ProviderInfo *info);
void member_table_print(void);

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

/* Copia IP e porta do membro para o ProviderInfo */
int member_table_get_provider_info(const uint8_t node_id[NODE_ID_LEN], ProviderInfo *info)
{
    int indice;

    if (node_id == NULL || info == NULL) {
        return MEMBER_ERR_INVALID;
    }
    if (pthread_mutex_lock(&members_mutex) != 0) {
        return MEMBER_ERR_INVALID;
    }
    indice = member_find_index(node_id);
    if (indice < 0) {
        pthread_mutex_unlock(&members_mutex);
        return MEMBER_ERR_NOT_FOUND;
    }
    memset(info, 0, sizeof(*info));
    snprintf(info->ip, sizeof(info->ip), "%s", members[indice].ip);
    info->port = members[indice].port;
    pthread_mutex_unlock(&members_mutex);
    return MEMBER_OK;
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

/* Dados repassados para a thread de cada conexao */
typedef struct {
    int socket;
    const Node *superpeer;
} Conexao;

/* Monta e envia uma resposta com payload opcional */
static int responder_com_payload(const Conexao *conexao, uint16_t tipo,
                                 const uint8_t destino[NODE_ID_LEN],
                                 uint8_t *payload, uint32_t tamanho)
{
    Mensagem resposta;

    memset(&resposta, 0, sizeof(resposta));
    resposta.header.versao_protocolo = VERSAO_PROTOCOLO;
    resposta.header.tipo_mensagem = tipo;
    memcpy(resposta.header.no_origem, conexao->superpeer->node_id, NODE_ID_LEN);
    memcpy(resposta.header.no_destino, destino, NODE_ID_LEN);
    resposta.header.timestamp = (uint64_t)time(NULL);
    resposta.header.tamanho_payload = tamanho;
    resposta.payload = payload;

    return enviar_mensagem(conexao->socket, &resposta);
}

/* Monta e envia uma resposta sem payload */
static int responder(const Conexao *conexao, uint16_t tipo, const uint8_t destino[NODE_ID_LEN])
{
    return responder_com_payload(conexao, tipo, destino, NULL, 0);
}

/* Le o endereco "ip:porta" que vem no payload do JOIN */
static int ler_endereco(const Mensagem *msg, char *ip, size_t tamanho_ip, uint16_t *porta)
{
    char texto[32];
    char *separador;
    uint32_t tamanho = msg->header.tamanho_payload;

    if (msg->payload == NULL || tamanho == 0 || tamanho >= sizeof(texto)) {
        return -1;
    }

    memcpy(texto, msg->payload, tamanho);
    texto[tamanho] = '\0';

    separador = strchr(texto, ':');
    if (separador == NULL) {
        return -1;
    }
    *separador = '\0';

    if (node_parse_ip(texto, ip, tamanho_ip) != 0) {
        return -1;
    }
    return node_parse_port(separador + 1, porta);
}

/* Valida o JOIN, registra o no e responde */
static void tratar_join(const Conexao *conexao, const Mensagem *msg)
{
    char ip[NODE_IP_LEN];
    uint16_t porta;
    int resultado;

    if (msg->header.versao_protocolo != VERSAO_PROTOCOLO) {
        printf("JOIN recusado: versao de protocolo %u\n", msg->header.versao_protocolo);
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    if (node_id_is_zero(msg->header.no_origem)) {
        printf("JOIN recusado: NodeID zerado\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    if (ler_endereco(msg, ip, sizeof(ip), &porta) != 0) {
        printf("JOIN recusado: endereco invalido no payload\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    resultado = member_table_add(msg->header.no_origem, ip, porta);
    if (resultado == MEMBER_ERR_DUPLICATE) {
        printf("JOIN: NodeID ja registrado, tabela inalterada\n");
    } else if (resultado != MEMBER_OK) {
        printf("JOIN recusado: nao foi possivel registrar\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    {
        ChordNode coord;
        if (bully_get_coordinator(&coord) &&
            node_id_compare(coord.id, conexao->superpeer->node_id) != 0) {
            smr_sync_with_coordinator(&coord);
        }
    }

    responder(conexao, MSG_ACK, msg->header.no_origem);
}

/* Remove o no da tabela e responde */
static void tratar_leave(const Conexao *conexao, const Mensagem *msg)
{
    if (member_table_remove(msg->header.no_origem) != MEMBER_OK) {
        printf("LEAVE: NodeID nao estava na tabela\n");
    }
    responder(conexao, MSG_ACK, msg->header.no_origem);
}

/* Validacao comum de STORE e LOOKUP */
static int validar_origem(const Mensagem *msg, const char *tipo)
{
    if (msg->header.versao_protocolo != VERSAO_PROTOCOLO) {
        printf("%s recusado: versao de protocolo %u\n", tipo, msg->header.versao_protocolo);
        return -1;
    }
    if (node_id_is_zero(msg->header.no_origem)) {
        printf("%s recusado: NodeID zerado\n", tipo);
        return -1;
    }
    return 0;
}

static int validar_chord(const Mensagem *msg);

/* Le o metadado do STORE, confere os chunks e marca dono e versao */
static int ler_metadado_store(const Mensagem *msg, FileMetadata *meta)
{
    if (metadata_deserialize(msg->payload, msg->header.tamanho_payload, meta) != METADATA_OK) {
        printf("STORE recusado: metadado invalido no payload\n");
        return -1;
    }
    if (metadata_check_chunks(meta) != METADATA_OK) {
        printf("STORE recusado: chunk_count nao bate com o tamanho\n");
        metadata_free(meta);
        return -1;
    }

    /* o dono e quem enviou e a versao comeca em 1 */
    memcpy(meta->owner, msg->header.no_origem, NODE_ID_LEN);
    meta->version = 1;
    return 0;
}

/* Repassa o STORE ao coordenador e devolve o tipo da resposta dele */
static uint16_t repassar_store(const Mensagem *msg, const ChordNode *coord)
{
    Mensagem pedido = *msg;
    Mensagem resposta;
    uint16_t tipo;
    int resultado = -1;
    int fd;

    /* no_destino = coordenador marca o STORE como repassado */
    memcpy(pedido.header.no_destino, coord->id, NODE_ID_LEN);
    memset(&resposta, 0, sizeof(resposta));

    fd = conectar_no_servidor(coord->ip, coord->port);
    if (fd >= 0) {
        resultado = enviar_mensagem(fd, &pedido);
        if (resultado == 0) {
            resultado = receber_mensagem(fd, &resposta);
        }
        close(fd);
    }
    if (resultado != 0) {
        printf("STORE: sem coordenador\n");
        return MSG_ERROR;
    }

    tipo = resposta.header.tipo_mensagem;
    liberar_mensagem(&resposta);
    printf("STORE repassado ao coordenador %s: %s\n", coord->name, nome_do_tipo(tipo));
    return tipo;
}

/* Valida o STORE; o coordenador registra no log e os demais repassam a ele */
static void tratar_store(const Conexao *conexao, const Mensagem *msg)
{
    const uint8_t *self = conexao->superpeer->node_id;
    int repassado = node_id_compare(msg->header.no_destino, self) == 0;
    FileMetadata meta;
    ChordNode coord;
    uint16_t tipo;

    /* STORE repassado vem de outro Super Peer, sem JOIN do peer aqui */
    if ((repassado ? validar_chord(msg) : validar_origem(msg, "STORE")) != 0 ||
        ler_metadado_store(msg, &meta) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    if (!bully_get_coordinator(&coord)) {
        printf("STORE: sem coordenador\n");
        tipo = MSG_ERROR;
    } else if (node_id_compare(coord.id, self) == 0) {
        tipo = (smr_put(&meta, msg->header.no_origem) == 0) ? MSG_ACK : MSG_ERROR;
    } else if (repassado) {
        printf("STORE recusado: repassado, mas este no nao e o coordenador\n");
        tipo = MSG_ERROR;
    } else {
        tipo = repassar_store(msg, &coord);
    }

    metadata_free(&meta);
    responder(conexao, tipo, msg->header.no_origem);
}

/* Adiciona um provedor na lista se ainda nao estiver presente */
static void adicionar_provedor_unico(ProviderInfo p_infos[MAX_PROVIDERS], uint32_t *count,
                                     const char *ip, uint16_t port)
{
    uint32_t i;

    if (*count >= MAX_PROVIDERS || ip == NULL || port == 0) {
        return;
    }
    for (i = 0; i < *count; i++) {
        if (p_infos[i].port == port && strcmp(p_infos[i].ip, ip) == 0) {
            return;
        }
    }
    memset(&p_infos[*count], 0, sizeof(ProviderInfo));
    snprintf(p_infos[*count].ip, sizeof(p_infos[*count].ip), "%s", ip);
    p_infos[*count].port = port;
    (*count)++;
}

/* Responde o LOOKUP com o metadado seguido de count + ProviderInfo de cada provedor.
 * Provedores sao os Super Peers vivos, que recebem a replica dos chunks no upload
 * (CHUNK_STORE). Um provedor sem o chunk responde ERROR e o cliente troca de replica. */
static void tratar_lookup_responder(const Conexao *conexao, const Mensagem *msg, const char *nome,
                                    const uint8_t *payload, uint32_t tamanho_payload)
{
    ProviderInfo p_infos[MAX_PROVIDERS];
    ChordNode vivos[MAX_PROVIDERS];
    uint32_t real_p_count = 0;
    uint32_t final_payload_size;
    uint8_t *final_payload;
    int n_vivos = 0;
    int i;

    adicionar_provedor_unico(p_infos, &real_p_count,
                             conexao->superpeer->ip, conexao->superpeer->port);

    n_vivos = gossip_alive_members(vivos, MAX_PROVIDERS);
    for (i = 0; i < n_vivos; i++) {
        adicionar_provedor_unico(p_infos, &real_p_count, vivos[i].ip, vivos[i].port);
    }

    final_payload_size = tamanho_payload + (uint32_t)sizeof(uint32_t) +
                         real_p_count * (uint32_t)sizeof(ProviderInfo);
    final_payload = (uint8_t *)malloc(final_payload_size);
    if (final_payload == NULL) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    memcpy(final_payload, payload, tamanho_payload);
    memcpy(final_payload + tamanho_payload, &real_p_count, sizeof(uint32_t));
    if (real_p_count > 0) {
        memcpy(final_payload + tamanho_payload + sizeof(uint32_t), p_infos,
               real_p_count * sizeof(ProviderInfo));
    }
    printf("LOOKUP: arquivo %s encontrado (%u provedores)\n", nome, real_p_count);
    responder_com_payload(conexao, MSG_ACK, msg->header.no_origem, final_payload, final_payload_size);
    free(final_payload);
}

/* Busca o metadado pelo nome e devolve serializado */
static void tratar_lookup(const Conexao *conexao, const Mensagem *msg)
{
    char nome[METADATA_NAME_LEN];
    uint32_t tamanho = msg->header.tamanho_payload;
    FileMetadata meta;
    uint8_t *payload = NULL;
    uint32_t tamanho_payload = 0;

    if (validar_origem(msg, "LOOKUP") != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    if (msg->payload == NULL || tamanho == 0 || tamanho > METADATA_NAME_MAX ||
        memchr(msg->payload, '\0', tamanho) != NULL) {
        printf("LOOKUP recusado: nome invalido no payload\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    memcpy(nome, msg->payload, tamanho);
    nome[tamanho] = '\0';

    if (cache_get_by_name(nome, &meta) != 0) {
        if (metadata_table_find_by_name(nome, &meta) != METADATA_OK) {
            printf("LOOKUP: arquivo %s nao encontrado\n", nome);
            responder(conexao, MSG_ERROR, msg->header.no_origem);
            return;
        }
        cache_put(&meta);
    }
    if (metadata_serialize(&meta, &payload, &tamanho_payload) != METADATA_OK) {
        printf("LOOKUP: nao foi possivel serializar o metadado\n");
        metadata_free(&meta);
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    tratar_lookup_responder(conexao, msg, nome, payload, tamanho_payload);
    free(payload);
    metadata_free(&meta);
}

/* Validacao comum das mensagens entre Super Peers */
static int validar_chord(const Mensagem *msg)
{
    const char *tipo = nome_do_tipo(msg->header.tipo_mensagem);

    if (msg->header.versao_protocolo != VERSAO_PROTOCOLO) {
        printf("%s recusado: versao de protocolo %u\n", tipo, msg->header.versao_protocolo);
        return -1;
    }
    if (node_id_is_zero(msg->header.no_origem)) {
        printf("%s recusado: NodeID zerado\n", tipo);
        return -1;
    }
    return 0;
}

/* Trata FIND_SUCCESSOR, GET_PREDECESSOR, NOTIFY, CHORD_LOOKUP e TOPOLOGY */
static void tratar_chord(const Conexao *conexao, const Mensagem *msg)
{
    uint8_t binario[1 + CHORD_NODE_MAX];
    char texto[CHORD_TEXT_LEN];
    uint8_t *payload = binario;
    uint32_t tamanho = msg->header.tamanho_payload;
    uint32_t tamanho_resposta = 0;
    int resultado = -1;

    if (validar_chord(msg) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    switch (msg->header.tipo_mensagem) {
    case MSG_FIND_SUCCESSOR:
        resultado = chord_handle_find_successor(msg->payload, tamanho, binario, &tamanho_resposta);
        break;
    case MSG_GET_PREDECESSOR:
        resultado = chord_handle_get_predecessor(tamanho, binario, &tamanho_resposta);
        break;
    case MSG_NOTIFY:
        resultado = chord_handle_notify(msg->payload, tamanho);
        break;
    case MSG_CHORD_LOOKUP:
        resultado = chord_handle_lookup(msg->payload, tamanho, texto, sizeof(texto));
        payload = (uint8_t *)texto;
        tamanho_resposta = (resultado == 0) ? (uint32_t)strlen(texto) : 0;
        break;
    case MSG_TOPOLOGY:
        resultado = chord_handle_topology(tamanho, texto, sizeof(texto));
        payload = (uint8_t *)texto;
        tamanho_resposta = (resultado == 0) ? (uint32_t)strlen(texto) : 0;
        break;
    default:
        break;
    }

    if (resultado != 0) {
        printf("%s recusado: payload invalido\n", nome_do_tipo(msg->header.tipo_mensagem));
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    responder_com_payload(conexao, MSG_ACK, msg->header.no_origem,
                          (tamanho_resposta > 0) ? payload : NULL, tamanho_resposta);
}

/* Trata HEARTBEAT e GOSSIP entre Super Peers */
static void tratar_gossip(const Conexao *conexao, const Mensagem *msg)
{
    uint8_t *tabela = NULL;
    uint32_t tamanho_tabela = 0;
    int resultado;

    if (validar_chord(msg) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    if (msg->header.tipo_mensagem == MSG_HEARTBEAT) {
        resultado = gossip_handle_heartbeat(msg->payload, msg->header.tamanho_payload);
    } else {
        resultado = gossip_handle_gossip(msg->payload, msg->header.tamanho_payload,
                                         &tabela, &tamanho_tabela);
    }

    if (resultado != 0) {
        printf("%s recusado: payload invalido\n", nome_do_tipo(msg->header.tipo_mensagem));
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    responder_com_payload(conexao, MSG_ACK, msg->header.no_origem, tabela, tamanho_tabela);
    free(tabela);
}

/* Trata ELECTION (responde OK se quem enviou e menor) e COORDINATOR */
static void tratar_bully(const Conexao *conexao, const Mensagem *msg)
{
    uint16_t tipo = MSG_ACK;
    int resultado;

    if (validar_chord(msg) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    if (msg->header.tipo_mensagem == MSG_ELECTION) {
        resultado = bully_handle_election(msg->payload, msg->header.tamanho_payload);
        if (resultado == 1) {
            tipo = MSG_OK;
        }
    } else {
        resultado = bully_handle_coordinator(msg->payload, msg->header.tamanho_payload);
    }

    if (resultado < 0) {
        printf("%s recusado: payload invalido\n", nome_do_tipo(msg->header.tipo_mensagem));
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    responder(conexao, tipo, msg->header.no_origem);
}

/* Trata LOG_APPEND do coordenador */
static void tratar_log_append(const Conexao *conexao, const Mensagem *msg)
{
    if (validar_chord(msg) != 0 ||
        smr_handle_log_append(msg->header.no_origem, msg->payload, msg->header.tamanho_payload) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    responder(conexao, MSG_ACK, msg->header.no_origem);
}

/* Trata PREPARE, COMMIT e ABORT do Two-Phase Commit */
static void tratar_2pc(const Conexao *conexao, const Mensagem *msg)
{
    uint16_t tipo_resposta = MSG_ACK;

    if (validar_chord(msg) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    switch (msg->header.tipo_mensagem) {
    case MSG_PREPARE:
        tipo_resposta = smr_handle_prepare(msg->header.no_origem, msg->payload,
                                           msg->header.tamanho_payload);
        break;
    case MSG_COMMIT:
        if (smr_handle_commit(msg->header.no_origem, msg->payload,
                              msg->header.tamanho_payload) != 0) {
            tipo_resposta = MSG_ERROR;
        }
        break;
    case MSG_ABORT:
        if (smr_handle_abort(msg->header.no_origem, msg->payload,
                             msg->header.tamanho_payload) != 0) {
            tipo_resposta = MSG_ERROR;
        }
        break;
    default:
        tipo_resposta = MSG_ERROR;
        break;
    }

    responder(conexao, tipo_resposta, msg->header.no_origem);
}

/* Trata STATE_TRANSFER (Incremental State Transfer) */
static void tratar_state_transfer(const Conexao *conexao, const Mensagem *msg)
{
    uint8_t *resposta = NULL;
    uint32_t tamanho_resposta = 0;

    if (validar_chord(msg) != 0 ||
        smr_handle_state_transfer(msg->payload, msg->header.tamanho_payload,
                                  &resposta, &tamanho_resposta) != 0) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    responder_com_payload(conexao, MSG_ACK, msg->header.no_origem, resposta, tamanho_resposta);
    free(resposta);
}

/* Trata DOWNLOAD_REQ servindo o chunk comprimido a partir de storage/<hex>.lz4 */
static void tratar_download_req(const Conexao *conexao, const Mensagem *msg)
{
    uint8_t chunk_hash[METADATA_HASH_LEN];
    uint32_t chunk_index = 0;
    char hash_hex[65];
    char caminho[128];
    FILE *fc;
    long comp_size;
    uint8_t *resposta_buf;
    int j;

    if (msg->payload == NULL || msg->header.tamanho_payload < METADATA_HASH_LEN) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    memcpy(chunk_hash, msg->payload, METADATA_HASH_LEN);
    if (msg->header.tamanho_payload >= METADATA_HASH_LEN + sizeof(uint32_t)) {
        memcpy(&chunk_index, msg->payload + METADATA_HASH_LEN, sizeof(uint32_t));
    }

    for (j = 0; j < 32; j++) {
        sprintf(&hash_hex[j * 2], "%02x", chunk_hash[j]);
    }
    hash_hex[64] = '\0';
    snprintf(caminho, sizeof(caminho), "storage/%u/%s.lz4",
             (unsigned)conexao->superpeer->port, hash_hex);

    fc = fopen(caminho, "rb");
    if (fc == NULL) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    fseek(fc, 0, SEEK_END);
    comp_size = ftell(fc);
    fseek(fc, 0, SEEK_SET);
    if (comp_size <= 0) {
        fclose(fc);
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    resposta_buf = (uint8_t *)malloc((size_t)comp_size);
    if (resposta_buf == NULL) {
        fclose(fc);
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    if (fread(resposta_buf, 1, (size_t)comp_size, fc) != (size_t)comp_size) {
        free(resposta_buf);
        fclose(fc);
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    fclose(fc);

    printf("DOWNLOAD_REP: chunk %u enviado (%ld bytes comprimidos)\n", chunk_index, comp_size);
    responder_com_payload(conexao, MSG_DOWNLOAD_REP, msg->header.no_origem,
                          resposta_buf, (uint32_t)comp_size);
    free(resposta_buf);
}

/* Envia um CHUNK_STORE (replica) a outro Super Peer; devolve 0 se ele respondeu ACK */
static int replicar_chunk(const ChordNode *destino, const Mensagem *original)
{
    Mensagem pedido = *original;
    Mensagem resposta;
    int ok = -1;
    int fd;

    pedido.payload = (uint8_t *)malloc(original->header.tamanho_payload);
    if (pedido.payload == NULL) {
        return -1;
    }
    memcpy(pedido.payload, original->payload, original->header.tamanho_payload);
    pedido.payload[METADATA_HASH_LEN] = 0; /* replica nao e repassada de novo */
    memset(&resposta, 0, sizeof(resposta));

    fd = conectar_no_servidor(destino->ip, destino->port);
    if (fd >= 0) {
        if (enviar_mensagem(fd, &pedido) == 0 && receber_mensagem(fd, &resposta) == 0 &&
            resposta.header.tipo_mensagem == MSG_ACK) {
            ok = 0;
        }
        liberar_mensagem(&resposta);
        close(fd);
    }
    free(pedido.payload);
    return ok;
}

/* Trata CHUNK_STORE: payload = SHA-256 do chunk original (32) + flag de repasse (1)
 * + chunk comprimido. Valida (LZ4 decode -> SHA-256), grava em storage/<porta>/
 * e, se veio do cliente, replica para os outros Super Peers vivos. */
static void tratar_chunk_store(const Conexao *conexao, const Mensagem *msg)
{
    const uint32_t cabecalho = METADATA_HASH_LEN + 1;
    uint32_t tamanho = msg->header.tamanho_payload;
    const uint8_t *hash;
    const uint8_t *dados;
    uint32_t comp_size;
    uint8_t *buffer;
    uint8_t calc[METADATA_HASH_LEN];
    char hash_hex[65];
    char caminho[128];
    ChordNode vivos[MAX_PROVIDERS];
    int decomp;
    int repassar;
    int n_vivos;
    int replicas = 0;
    int i;
    FILE *fc;

    if (msg->header.versao_protocolo != VERSAO_PROTOCOLO || msg->payload == NULL ||
        tamanho <= cabecalho) {
        printf("CHUNK_STORE recusado: payload invalido\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    hash = msg->payload;
    repassar = msg->payload[METADATA_HASH_LEN] != 0;
    dados = msg->payload + cabecalho;
    comp_size = tamanho - cabecalho;

    /* Compressed Chunk -> LZ4 Decode -> SHA-256 -> Validacao (Secao 10.4) */
    buffer = (uint8_t *)malloc(CHUNK_SIZE);
    if (buffer == NULL) {
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    decomp = LZ4_decompress_safe((const char *)dados, (char *)buffer, (int)comp_size, CHUNK_SIZE);
    if (decomp <= 0) {
        free(buffer);
        printf("CHUNK_STORE recusado: LZ4 invalido\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    SHA256(buffer, (size_t)decomp, calc);
    free(buffer);
    if (memcmp(calc, hash, METADATA_HASH_LEN) != 0) {
        printf("CHUNK_STORE recusado: SHA-256 do chunk nao confere\n");
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }

    for (i = 0; i < METADATA_HASH_LEN; i++) {
        sprintf(&hash_hex[i * 2], "%02x", hash[i]);
    }
    hash_hex[64] = '\0';
    mkdir("storage", 0755);
    snprintf(caminho, sizeof(caminho), "storage/%u", (unsigned)conexao->superpeer->port);
    mkdir(caminho, 0755);
    snprintf(caminho, sizeof(caminho), "storage/%u/%s.lz4",
             (unsigned)conexao->superpeer->port, hash_hex);

    fc = fopen(caminho, "wb");
    if (fc == NULL || fwrite(dados, 1, comp_size, fc) != comp_size) {
        if (fc != NULL) {
            fclose(fc);
        }
        printf("CHUNK_STORE: nao foi possivel gravar %s\n", caminho);
        responder(conexao, MSG_ERROR, msg->header.no_origem);
        return;
    }
    fclose(fc);
    printf("CHUNK_STORE: chunk %.16s... armazenado em %s (%u bytes comprimidos)\n",
           hash_hex, caminho, comp_size);

    if (repassar) {
        n_vivos = gossip_alive_members(vivos, MAX_PROVIDERS);
        for (i = 0; i < n_vivos; i++) {
            if (vivos[i].port == conexao->superpeer->port &&
                strcmp(vivos[i].ip, conexao->superpeer->ip) == 0) {
                continue;
            }
            if (replicar_chunk(&vivos[i], msg) == 0) {
                replicas++;
                printf("CHUNK_STORE: replica do chunk %.16s... enviada para %s\n",
                       hash_hex, vivos[i].name);
            } else {
                printf("CHUNK_STORE: falha ao replicar chunk %.16s... para %s\n",
                       hash_hex, vivos[i].name);
            }
        }
        printf("CHUNK_STORE: chunk %.16s... com %d copias (local + %d replicas)\n",
               hash_hex, replicas + 1, replicas);
    }
    responder(conexao, MSG_ACK, msg->header.no_origem);
}

/* Atende uma conexao aceita e encerra o socket */
static void *atender_conexao(void *arg)
{
    Conexao conexao = *(Conexao *)arg;
    Mensagem msg;
    char hex[NODE_ID_HEX_LEN];

    free(arg);
    memset(&msg, 0, sizeof(msg));

    if (receber_mensagem(conexao.socket, &msg) == 0) {
        node_id_to_hex(msg.header.no_origem, hex);
        printf("RX %s de %s\n", nome_do_tipo(msg.header.tipo_mensagem), hex);

        switch (msg.header.tipo_mensagem) {
        case MSG_JOIN:
            tratar_join(&conexao, &msg);
            break;
        case MSG_LEAVE:
            tratar_leave(&conexao, &msg);
            break;
        case MSG_PING:
            responder(&conexao, MSG_PONG, msg.header.no_origem);
            break;
        case MSG_STORE:
            tratar_store(&conexao, &msg);
            break;
        case MSG_LOOKUP:
            tratar_lookup(&conexao, &msg);
            break;
        case MSG_FIND_SUCCESSOR:
        case MSG_GET_PREDECESSOR:
        case MSG_NOTIFY:
        case MSG_CHORD_LOOKUP:
        case MSG_TOPOLOGY:
            tratar_chord(&conexao, &msg);
            break;
        case MSG_HEARTBEAT:
        case MSG_GOSSIP:
            tratar_gossip(&conexao, &msg);
            break;
        case MSG_ELECTION:
        case MSG_COORDINATOR:
            tratar_bully(&conexao, &msg);
            break;
        case MSG_LOG_APPEND:
            tratar_log_append(&conexao, &msg);
            break;
        case MSG_PREPARE:
        case MSG_COMMIT:
        case MSG_ABORT:
            tratar_2pc(&conexao, &msg);
            break;
        case MSG_STATE_TRANSFER:
            tratar_state_transfer(&conexao, &msg);
            break;
        case MSG_DOWNLOAD_REQ:
            tratar_download_req(&conexao, &msg);
            break;
        case MSG_CHUNK_STORE:
            tratar_chunk_store(&conexao, &msg);
            break;
        default:
            printf("Tipo de mensagem nao suportado\n");
            responder(&conexao, MSG_ERROR, msg.header.no_origem);
            break;
        }
    }

    liberar_mensagem(&msg);
    close(conexao.socket);
    return NULL;
}

/* Mostra os dois formatos de argumento aceitos */
static void mostrar_uso(const char *programa)
{
    node_print_usage(programa);
    fprintf(stderr, "  %s --port <porta> [--name <nome>] [--config <arquivo>]\n", programa);
}

/* Le o endereco "ip:porta" do bootstrap */
static int ler_bootstrap(const char *valor, NodeConfig *config)
{
    char texto[32];
    char *separador;

    if (strlen(valor) >= sizeof(texto)) {
        return -1;
    }
    snprintf(texto, sizeof(texto), "%s", valor);

    separador = strchr(texto, ':');
    if (separador == NULL) {
        return -1;
    }
    *separador = '\0';

    if (node_parse_ip(texto, config->superpeer_ip, sizeof(config->superpeer_ip)) != 0) {
        return -1;
    }
    return node_parse_port(separador + 1, &config->superpeer_port);
}

/* Linha CSV nome,papel,ip,porta,...: a primeira vira bootstrap (se nao houver
 * bootstrap=) e a da porta deste no da o nome */
static int ler_linha_csv(const char *linha, NodeConfig *config, const char *porta,
                         char *nome, size_t tamanho_nome, int tem_nome)
{
    char nome_csv[NOME_MAX];
    char papel[16];
    char ip[NODE_IP_LEN];
    char porta_csv[8];
    char endereco[32];

    if (sscanf(linha, "%63[^,],%15[^,],%15[^,],%7[^,]", nome_csv, papel, ip, porta_csv) != 4) {
        return 0;
    }
    if (config->superpeer_port == 0) {
        snprintf(endereco, sizeof(endereco), "%s:%s", ip, porta_csv);
        if (ler_bootstrap(endereco, config) != 0) {
            fprintf(stderr, "Erro: linha invalida: %s\n", linha);
            return -1;
        }
    }
    if (!tem_nome && strcmp(porta_csv, porta) == 0) {
        snprintf(nome, tamanho_nome, "%s", nome_csv);
    }
    return 0;
}

/* Le o arquivo chave=valor ou CSV; porta e nome so valem se nao vieram na linha de comando */
static int ler_config(const char *caminho, NodeConfig *config, char *porta, size_t tamanho_porta,
                      int tem_porta, char *nome, size_t tamanho_nome, int tem_nome)
{
    char linha[LINHA_MAX];
    char *valor;
    FILE *arquivo = fopen(caminho, "r");
    int resultado = 0;

    if (arquivo == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir %s\n", caminho);
        return -1;
    }

    while (resultado == 0 && fgets(linha, sizeof(linha), arquivo) != NULL) {
        linha[strcspn(linha, "\r\n")] = '\0';
        valor = strchr(linha, '=');
        if (linha[0] == '#') {
            continue;
        }
        if (valor == NULL) {
            resultado = ler_linha_csv(linha, config, porta, nome, tamanho_nome, tem_nome);
            continue;
        }
        *valor = '\0';
        valor++;

        if (strcmp(linha, "port") == 0 && !tem_porta) {
            snprintf(porta, tamanho_porta, "%s", valor);
        } else if (strcmp(linha, "name") == 0 && !tem_nome) {
            snprintf(nome, tamanho_nome, "%s", valor);
        } else if (strcmp(linha, "bootstrap") == 0 && ler_bootstrap(valor, config) != 0) {
            fprintf(stderr, "Erro: bootstrap invalido: %s\n", valor);
            resultado = -1;
        }
    }

    fclose(arquivo);
    return resultado;
}

/* Le a configuracao no formato de opcoes longas */
static int ler_opcoes(int argc, char *argv[], NodeConfig *config, char *nome, size_t tamanho_nome)
{
    char porta[8];
    const char *caminho_config = NULL;
    int tem_porta = 0;
    int tem_nome = 0;
    int opt;
    struct option longas[] = {
        {"config", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"name", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };

    memset(config, 0, sizeof(*config));
    config->role = ROLE_SUPERPEER;
    snprintf(porta, sizeof(porta), "%s", PORTA_PADRAO);
    nome[0] = '\0';

    while ((opt = getopt_long(argc, argv, "c:p:n:", longas, NULL)) != -1) {
        switch (opt) {
        case 'c':
            caminho_config = optarg;
            break;
        case 'p':
            snprintf(porta, sizeof(porta), "%s", optarg);
            tem_porta = 1;
            break;
        case 'n':
            snprintf(nome, tamanho_nome, "%s", optarg);
            tem_nome = 1;
            break;
        default:
            return -1;
        }
    }

    if (caminho_config != NULL &&
        ler_config(caminho_config, config, porta, sizeof(porta), tem_porta,
                   nome, tamanho_nome, tem_nome) != 0) {
        return -1;
    }

    if (node_parse_ip(IP_PADRAO, config->ip, sizeof(config->ip)) != 0) {
        return -1;
    }
    if (node_parse_port(porta, &config->port) != 0) {
        fprintf(stderr, "Erro: porta invalida: %s\n", porta);
        return -1;
    }

    /* Sem nome, usa SP<porta>; cada porta tem o seu arquivo de UUID */
    if (nome[0] == '\0') {
        snprintf(nome, tamanho_nome, "SP%u", config->port);
    }
    snprintf(config->uuid_path, sizeof(config->uuid_path), "superpeer_%u.uuid", config->port);
    return 0;
}

/* Sobe o super peer e atende uma conexao por thread */
int main(int argc, char *argv[])
{
    NodeConfig config;
    Node superpeer;
    char nome[NOME_MAX];
    int servidor_fd;

    /* buffer de linha para o log sair na hora quando redirecionado */
    setvbuf(stdout, NULL, _IOLBF, 0);

    snprintf(nome, sizeof(nome), "%s", NOME_PADRAO);

    /* o primeiro argumento diz qual dos dois formatos foi usado */
    if (argc > 1 && argv[1][0] != '-') {
        if (node_parse_args(argc, argv, &config) != 0) {
            mostrar_uso(argv[0]);
            return 1;
        }
        if (config.role != ROLE_SUPERPEER) {
            fprintf(stderr, "Erro: este executavel so roda como superpeer\n");
            return 1;
        }
    } else if (ler_opcoes(argc, argv, &config, nome, sizeof(nome)) != 0) {
        mostrar_uso(argv[0]);
        return 1;
    }

    if (node_init(&superpeer, &config) != 0) {
        return 1;
    }
    node_print(&superpeer);

    servidor_fd = criar_servidor(superpeer.port);
    if (servidor_fd < 0) {
        fprintf(stderr, "Erro: nao foi possivel escutar na porta %u\n", superpeer.port);
        return 1;
    }
    superpeer.state = STATE_CONNECTED;
    printf("Node %s started\n", nome);
    printf("Escutando na porta %u\n", superpeer.port);

    if (chord_start(&superpeer, nome, config.superpeer_ip, config.superpeer_port) != 0) {
        fprintf(stderr, "Erro: nao foi possivel iniciar o Chord\n");
        close(servidor_fd);
        return 1;
    }
    if (gossip_start(&superpeer, nome, config.superpeer_ip, config.superpeer_port) != 0) {
        fprintf(stderr, "Erro: nao foi possivel iniciar o Gossip\n");
        close(servidor_fd);
        return 1;
    }
    if (smr_start(&superpeer) != 0 || bully_start(&superpeer, nome) != 0) {
        fprintf(stderr, "Erro: nao foi possivel iniciar o Bully e o SMR\n");
        close(servidor_fd);
        return 1;
    }

    for (;;) {
        Conexao *conexao;
        pthread_t thread;
        int cliente_fd = aceitar_cliente(servidor_fd);

        if (cliente_fd < 0) {
            continue;
        }

        conexao = (Conexao *)malloc(sizeof(Conexao));
        if (conexao == NULL) {
            close(cliente_fd);
            continue;
        }
        conexao->socket = cliente_fd;
        conexao->superpeer = &superpeer;

        if (pthread_create(&thread, NULL, atender_conexao, conexao) != 0) {
            fprintf(stderr, "Erro: nao foi possivel criar a thread\n");
            free(conexao);
            close(cliente_fd);
            continue;
        }
        pthread_detach(thread);
    }

    close(servidor_fd);
    return 0;
}

/* ==========================================================================
 * CAMADA CHORD DHT (integrado de chord.c)
 * ========================================================================== */

#define CHORD_FINGERS 256
#define CHORD_PERIOD_SEC 1
#define CHORD_MAX_HOPS 32
#define CHORD_IP_MIN 7

/* Dados repassados para a thread periodica */
typedef struct {
    int has_bootstrap;
    ChordNode bootstrap;
} ChordStart;

/* Estado do Chord e o mutex que o protege */
static ChordNode chord_self;
static ChordNode chord_successor;
static ChordNode chord_predecessor;
static int chord_has_predecessor = 0;
static ChordNode chord_finger[CHORD_FINGERS];
static pthread_mutex_t chord_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Diz se os dois nos tem o mesmo NodeID */
int chord_same(const ChordNode *a, const ChordNode *b)
{
    return node_id_compare(a->id, b->id) == 0;
}

/* x em (a, b) no anel; com a == b vale o anel inteiro menos a */
static int chord_in_open(const uint8_t x[NODE_ID_LEN], const uint8_t a[NODE_ID_LEN],
                         const uint8_t b[NODE_ID_LEN])
{
    int ab = node_id_compare(a, b);

    if (ab < 0) {
        return node_id_compare(a, x) < 0 && node_id_compare(x, b) < 0;
    }
    if (ab > 0) {
        return node_id_compare(a, x) < 0 || node_id_compare(x, b) < 0;
    }
    return node_id_compare(x, a) != 0;
}

/* x em (a, b] no anel; com a == b vale o anel inteiro */
static int chord_in_half(const uint8_t x[NODE_ID_LEN], const uint8_t a[NODE_ID_LEN],
                         const uint8_t b[NODE_ID_LEN])
{
    if (node_id_compare(x, b) == 0) {
        return 1;
    }
    return chord_in_open(x, a, b);
}

/* out = id + 2^i modulo 2^256, com vai-um sobre os bytes big-endian */
static void chord_add_pow2(const uint8_t id[NODE_ID_LEN], int i, uint8_t out[NODE_ID_LEN])
{
    int byte = NODE_ID_LEN - 1 - (i / 8);
    unsigned int soma = 1u << (i % 8);

    memcpy(out, id, NODE_ID_LEN);
    while (byte >= 0 && soma != 0) {
        soma += out[byte];
        out[byte] = (uint8_t)(soma & 0xFF);
        soma >>= 8;
        byte--;
    }
}

/* Grava o no no buffer e devolve quantos bytes usou */
uint32_t chord_node_write(const ChordNode *no, uint8_t *buffer)
{
    size_t ip_len = strnlen(no->ip, NODE_IP_LEN - 1);
    size_t name_len = strnlen(no->name, CHORD_NAME_LEN - 1);
    uint32_t pos = 0;

    memcpy(buffer + pos, no->id, NODE_ID_LEN);
    pos += NODE_ID_LEN;
    memcpy(buffer + pos, &no->port, sizeof(no->port));
    pos += sizeof(no->port);
    buffer[pos++] = (uint8_t)ip_len;
    memcpy(buffer + pos, no->ip, ip_len);
    pos += (uint32_t)ip_len;
    buffer[pos++] = (uint8_t)name_len;
    memcpy(buffer + pos, no->name, name_len);
    pos += (uint32_t)name_len;
    return pos;
}

/* Confere se ainda ha n bytes para ler a partir de pos */
int chord_has_bytes(uint32_t size, uint32_t pos, uint32_t n)
{
    return pos <= size && size - pos >= n;
}

/* Le um no do buffer a partir de pos, validando cada campo */
int chord_node_read(const uint8_t *buffer, uint32_t size, uint32_t *pos, ChordNode *no)
{
    char ip[NODE_IP_LEN];
    uint8_t ip_len;
    uint8_t name_len;

    memset(no, 0, sizeof(*no));
    if (buffer == NULL || !chord_has_bytes(size, *pos, NODE_ID_LEN + sizeof(no->port) + 1)) {
        return -1;
    }
    memcpy(no->id, buffer + *pos, NODE_ID_LEN);
    *pos += NODE_ID_LEN;
    memcpy(&no->port, buffer + *pos, sizeof(no->port));
    *pos += sizeof(no->port);

    ip_len = buffer[(*pos)++];
    if (ip_len < CHORD_IP_MIN || ip_len >= NODE_IP_LEN || !chord_has_bytes(size, *pos, ip_len + 1u)) {
        return -1;
    }
    memcpy(ip, buffer + *pos, ip_len);
    ip[ip_len] = '\0';
    *pos += ip_len;
    if (node_parse_ip(ip, no->ip, sizeof(no->ip)) != 0 || no->port == 0) {
        return -1;
    }

    name_len = buffer[(*pos)++];
    if (name_len < 1 || name_len >= CHORD_NAME_LEN || !chord_has_bytes(size, *pos, name_len) ||
        memchr(buffer + *pos, '\0', name_len) != NULL) {
        return -1;
    }
    memcpy(no->name, buffer + *pos, name_len);
    no->name[name_len] = '\0';
    *pos += name_len;
    return 0;
}

/* Abre conexao, envia o pedido, espera o ACK e fecha */
static int chord_request(const ChordNode *destino, uint16_t tipo, uint8_t *payload,
                         uint32_t tamanho, Mensagem *resposta)
{
    Mensagem pedido;
    int fd;
    int resultado;

    memset(resposta, 0, sizeof(*resposta));
    fd = conectar_no_servidor(destino->ip, destino->port);
    if (fd < 0) {
        printf("Chord: %s nao respondeu\n", destino->name);
        return -1;
    }

    memset(&pedido, 0, sizeof(pedido));
    pedido.header.versao_protocolo = VERSAO_PROTOCOLO;
    pedido.header.tipo_mensagem = tipo;
    memcpy(pedido.header.no_origem, chord_self.id, NODE_ID_LEN);
    memcpy(pedido.header.no_destino, destino->id, NODE_ID_LEN);
    pedido.header.timestamp = (uint64_t)time(NULL);
    pedido.header.tamanho_payload = tamanho;
    pedido.payload = payload;

    resultado = enviar_mensagem(fd, &pedido);
    if (resultado == 0) {
        resultado = receber_mensagem(fd, resposta);
    }
    close(fd);

    if (resultado != 0) {
        liberar_mensagem(resposta);
        printf("Chord: %s nao respondeu\n", destino->name);
        return -1;
    }
    if (resposta->header.tipo_mensagem != MSG_ACK) {
        liberar_mensagem(resposta);
        printf("Chord: %s recusou %s\n", destino->name, nome_do_tipo(tipo));
        return -1;
    }
    return 0;
}

/* Pergunta ao no remoto o successor de id */
static int chord_ask_successor(const ChordNode *destino, const uint8_t id[NODE_ID_LEN],
                               ChordNode *no, int *found)
{
    uint8_t pedido[NODE_ID_LEN];
    Mensagem resposta;
    uint32_t tamanho;
    uint32_t pos = 1;

    memcpy(pedido, id, NODE_ID_LEN);
    if (chord_request(destino, MSG_FIND_SUCCESSOR, pedido, NODE_ID_LEN, &resposta) != 0) {
        return -1;
    }

    tamanho = resposta.header.tamanho_payload;
    if (resposta.payload == NULL || tamanho < 1 || resposta.payload[0] > 1 ||
        chord_node_read(resposta.payload, tamanho, &pos, no) != 0 || pos != tamanho) {
        printf("Chord: resposta invalida de %s\n", destino->name);
        liberar_mensagem(&resposta);
        return -1;
    }
    *found = resposta.payload[0];
    liberar_mensagem(&resposta);
    return 0;
}

/* Pergunta ao no remoto quem e o predecessor dele */
static int chord_ask_predecessor(const ChordNode *destino, ChordNode *pred, int *has)
{
    Mensagem resposta;
    uint32_t tamanho;
    uint32_t pos = 1;
    int valido;

    if (chord_request(destino, MSG_GET_PREDECESSOR, NULL, 0, &resposta) != 0) {
        return -1;
    }

    tamanho = resposta.header.tamanho_payload;
    valido = resposta.payload != NULL && tamanho >= 1 && resposta.payload[0] <= 1;
    if (valido && resposta.payload[0] == 1) {
        valido = chord_node_read(resposta.payload, tamanho, &pos, pred) == 0 && pos == tamanho;
    } else if (valido) {
        valido = (tamanho == 1);
    }
    if (!valido) {
        printf("Chord: resposta invalida de %s\n", destino->name);
        liberar_mensagem(&resposta);
        return -1;
    }
    *has = resposta.payload[0];
    liberar_mensagem(&resposta);
    return 0;
}

/* Avisa o no remoto que este no pode ser o predecessor dele */
static void chord_send_notify(const ChordNode *destino)
{
    uint8_t pedido[CHORD_NODE_MAX];
    uint32_t tamanho = chord_node_write(&chord_self, pedido);
    Mensagem resposta;

    if (chord_request(destino, MSG_NOTIFY, pedido, tamanho, &resposta) == 0) {
        liberar_mensagem(&resposta);
    }
}

/* Passo local do lookup: achou o successor ou indica o proximo no a perguntar */
static int chord_local_step(const uint8_t id[NODE_ID_LEN], ChordNode *no, int *found)
{
    int i;

    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return -1;
    }

    *no = chord_successor;
    *found = chord_in_half(id, chord_self.id, chord_successor.id);

    /* closest_preceding_node: primeira finger em (self, id), de 255 ate 0 */
    for (i = CHORD_FINGERS - 1; i >= 0 && !*found; i--) {
        if (chord_in_open(chord_finger[i].id, chord_self.id, id)) {
            *no = chord_finger[i];
            break;
        }
    }

    pthread_mutex_unlock(&chord_mutex);
    return 0;
}

/* Segue o lookup iterativo a partir de um no remoto, anotando o caminho */
static int chord_follow(ChordNode atual, const uint8_t id[NODE_ID_LEN],
                        ChordNode caminho[CHORD_MAX_HOPS + 1], int *passos, ChordNode *dono)
{
    int found = 0;

    while (!found) {
        if (*passos >= CHORD_MAX_HOPS) {
            printf("Chord: lookup passou de %d saltos\n", CHORD_MAX_HOPS);
            return -1;
        }
        caminho[(*passos)++] = atual;
        if (chord_ask_successor(&atual, id, &atual, &found) != 0) {
            return -1;
        }
    }
    *dono = atual;
    return 0;
}

/* find_successor iterativo; o caminho comeca neste no e termina no dono */
static int chord_find_successor(const uint8_t id[NODE_ID_LEN],
                                ChordNode caminho[CHORD_MAX_HOPS + 1], int *passos,
                                ChordNode *dono)
{
    ChordNode proximo;
    int found;

    *passos = 0;
    caminho[(*passos)++] = chord_self;

    if (chord_local_step(id, &proximo, &found) != 0) {
        return -1;
    }
    if (found) {
        *dono = proximo;
    } else if (chord_follow(proximo, id, caminho, passos, dono) != 0) {
        return -1;
    }

    if (!chord_same(&caminho[*passos - 1], dono)) {
        caminho[(*passos)++] = *dono;
    }
    return 0;
}

/* Imprime um no no formato nome (ip:porta) */
static void chord_print_node(const char *rotulo, const ChordNode *no)
{
    printf("%s: %s (%s:%u)\n", rotulo, no->name, no->ip, no->port);
}

/* Troca o successor e imprime se mudou */
static void chord_set_successor(const ChordNode *no)
{
    int mudou;

    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return;
    }
    mudou = !chord_same(&chord_successor, no);
    chord_successor = *no;
    pthread_mutex_unlock(&chord_mutex);

    if (mudou) {
        chord_print_node("Successor", no);
    }
}

/* Entra no anel pedindo ao bootstrap o successor deste no, a cada 1 s ate conseguir */
static void chord_join(const ChordNode *bootstrap)
{
    ChordNode caminho[CHORD_MAX_HOPS + 1];
    ChordNode successor;
    int passos;

    for (;;) {
        passos = 0;
        if (chord_follow(*bootstrap, chord_self.id, caminho, &passos, &successor) == 0) {
            break;
        }
        sleep(CHORD_PERIOD_SEC);
    }

    printf("JOIN %s\n", chord_self.name);
    if (!chord_same(&successor, &chord_self)) {
        chord_set_successor(&successor);
    }
}

/* Escolhe o proximo no vivo no anel quando o successor atual falha */
static void chord_recover_successor(const ChordNode *falho)
{
    ChordNode vivos[64];
    ChordNode melhor = chord_self;
    int n_vivos = gossip_alive_members(vivos, 64);
    int achou = 0;
    int i;

    for (i = 0; i < n_vivos; i++) {
        if (chord_same(&vivos[i], &chord_self) || chord_same(&vivos[i], falho)) {
            continue;
        }
        if (!achou || chord_in_open(vivos[i].id, chord_self.id, melhor.id)) {
            melhor = vivos[i];
            achou = 1;
        }
    }
    chord_set_successor(&melhor);
}

/* Pergunta ao successor o predecessor dele, ajusta o successor e manda NOTIFY */
static void chord_stabilize(void)
{
    ChordNode successor;
    ChordNode x;
    int has_x = 0;

    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return;
    }
    successor = chord_successor;
    if (chord_same(&successor, &chord_self)) {
        x = chord_predecessor;
        has_x = chord_has_predecessor;
    }
    pthread_mutex_unlock(&chord_mutex);

    /* Successor remoto: pergunta pela rede; se falhar, recupera pelo proximo no vivo */
    if (!chord_same(&successor, &chord_self) &&
        chord_ask_predecessor(&successor, &x, &has_x) != 0) {
        chord_recover_successor(&successor);
        return;
    }

    if (has_x && chord_in_open(x.id, chord_self.id, successor.id)) {
        successor = x;
        chord_set_successor(&successor);
    }
    if (!chord_same(&successor, &chord_self)) {
        chord_send_notify(&successor);
    }
}

/* Diz se o no ja apareceu numa finger anterior */
static int chord_finger_seen(const ChordNode finger[CHORD_FINGERS], int i)
{
    int j;

    for (j = 0; j < i; j++) {
        if (chord_same(&finger[j], &finger[i])) {
            return 1;
        }
    }
    return 0;
}

/* Acrescenta texto formatado ao buffer sem estourar */
static void chord_append(char *texto, size_t tamanho, size_t *pos, const char *formato, ...)
{
    va_list args;
    int escritos;

    if (*pos >= tamanho) {
        return;
    }
    va_start(args, formato);
    escritos = vsnprintf(texto + *pos, tamanho - *pos, formato, args);
    va_end(args);

    if (escritos < 0) {
        return;
    }
    *pos += (size_t)escritos;
    if (*pos >= tamanho) {
        *pos = tamanho - 1;
    }
}

/* Monta o texto da finger table, uma linha por no distinto */
static void chord_format_fingers(const ChordNode finger[CHORD_FINGERS], char *texto,
                                 size_t tamanho, size_t *pos)
{
    int i;

    chord_append(texto, tamanho, pos, "Finger table:\n");
    for (i = 0; i < CHORD_FINGERS; i++) {
        if (!chord_finger_seen(finger, i)) {
            chord_append(texto, tamanho, pos, "  finger[%d] -> %s\n", i, finger[i].name);
        }
    }
}

/* Recalcula as 256 fingers e imprime a tabela se mudou */
static void chord_fix_fingers(void)
{
    ChordNode novo[CHORD_FINGERS];
    ChordNode caminho[CHORD_MAX_HOPS + 1];
    uint8_t alvo[NODE_ID_LEN];
    char texto[CHORD_TEXT_LEN];
    size_t pos = 0;
    int passos;
    int mudou = 0;
    int i;

    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return;
    }
    memcpy(novo, chord_finger, sizeof(novo));
    pthread_mutex_unlock(&chord_mutex);

    /* Sem resposta, a finger antiga e mantida */
    for (i = 0; i < CHORD_FINGERS; i++) {
        chord_add_pow2(chord_self.id, i, alvo);
        chord_find_successor(alvo, caminho, &passos, &novo[i]);
    }

    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return;
    }
    for (i = 0; i < CHORD_FINGERS && !mudou; i++) {
        mudou = !chord_same(&chord_finger[i], &novo[i]);
    }
    memcpy(chord_finger, novo, sizeof(novo));
    pthread_mutex_unlock(&chord_mutex);

    if (mudou) {
        chord_format_fingers(novo, texto, sizeof(texto), &pos);
        printf("%s", texto);
    }
}

/* Rotina periodica: join, depois stabilize e fix_fingers a cada periodo */
static void *chord_loop(void *arg)
{
    ChordStart inicio = *(ChordStart *)arg;

    free(arg);
    if (inicio.has_bootstrap) {
        chord_join(&inicio.bootstrap);
    }

    for (;;) {
        chord_stabilize();
        chord_fix_fingers();
        sleep(CHORD_PERIOD_SEC);
    }
    return NULL;
}

/* Inicia o estado do anel e a thread periodica */
int chord_start(const Node *no, const char *nome, const char *bootstrap_ip,
                uint16_t bootstrap_port)
{
    ChordStart *inicio;
    pthread_t thread;
    int i;

    memset(&chord_self, 0, sizeof(chord_self));
    memcpy(chord_self.id, no->node_id, NODE_ID_LEN);
    snprintf(chord_self.ip, sizeof(chord_self.ip), "%s", no->ip);
    chord_self.port = no->port;
    snprintf(chord_self.name, sizeof(chord_self.name), "%s", nome);

    /* Cria o anel com um no so: successor e fingers apontam para si */
    chord_successor = chord_self;
    chord_has_predecessor = 0;
    for (i = 0; i < CHORD_FINGERS; i++) {
        chord_finger[i] = chord_self;
    }

    inicio = (ChordStart *)malloc(sizeof(ChordStart));
    if (inicio == NULL) {
        return -1;
    }
    memset(inicio, 0, sizeof(*inicio));

    /* Bootstrap ausente ou igual a este no: o anel comeca aqui */
    if (bootstrap_port != 0 &&
        !(bootstrap_port == no->port && strcmp(bootstrap_ip, no->ip) == 0)) {
        inicio->has_bootstrap = 1;
        snprintf(inicio->bootstrap.ip, sizeof(inicio->bootstrap.ip), "%s", bootstrap_ip);
        inicio->bootstrap.port = bootstrap_port;
        snprintf(inicio->bootstrap.name, sizeof(inicio->bootstrap.name), "%s:%u",
                 bootstrap_ip, bootstrap_port);
    } else {
        printf("Chord: anel criado por %s\n", chord_self.name);
    }
    chord_print_node("Successor", &chord_successor);

    if (pthread_create(&thread, NULL, chord_loop, inicio) != 0) {
        free(inicio);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

/* FIND_SUCCESSOR: responde found + successor ou found + proximo no */
int chord_handle_find_successor(const uint8_t *payload, uint32_t tamanho,
                                uint8_t resposta[1 + CHORD_NODE_MAX], uint32_t *tamanho_resposta)
{
    ChordNode no;
    int found;

    if (payload == NULL || tamanho != NODE_ID_LEN) {
        return -1;
    }
    if (chord_local_step(payload, &no, &found) != 0) {
        return -1;
    }
    resposta[0] = (uint8_t)found;
    *tamanho_resposta = 1 + chord_node_write(&no, resposta + 1);
    return 0;
}

/* GET_PREDECESSOR: responde has + predecessor */
int chord_handle_get_predecessor(uint32_t tamanho, uint8_t resposta[1 + CHORD_NODE_MAX],
                                 uint32_t *tamanho_resposta)
{
    ChordNode pred;
    int has;

    if (tamanho != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return -1;
    }
    pred = chord_predecessor;
    has = chord_has_predecessor;
    pthread_mutex_unlock(&chord_mutex);

    resposta[0] = (uint8_t)has;
    *tamanho_resposta = 1;
    if (has) {
        *tamanho_resposta += chord_node_write(&pred, resposta + 1);
    }
    return 0;
}

/* NOTIFY: o no que avisou vira predecessor se estiver em (predecessor, self) */
int chord_handle_notify(const uint8_t *payload, uint32_t tamanho)
{
    ChordNode no;
    uint32_t pos = 0;
    int mudou = 0;

    if (chord_node_read(payload, tamanho, &pos, &no) != 0 || pos != tamanho) {
        return -1;
    }
    if (chord_same(&no, &chord_self)) {
        return 0;
    }
    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return -1;
    }
    if (!chord_has_predecessor || chord_in_open(no.id, chord_predecessor.id, chord_self.id)) {
        mudou = !chord_has_predecessor || !chord_same(&chord_predecessor, &no);
        chord_predecessor = no;
        chord_has_predecessor = 1;
    }
    pthread_mutex_unlock(&chord_mutex);

    if (mudou) {
        chord_print_node("Predecessor", &no);
    }
    return 0;
}

/* CHORD_LOOKUP: faz o find_successor iterativo e monta o caminho */
int chord_handle_lookup(const uint8_t *payload, uint32_t tamanho, char *texto, size_t tamanho_texto)
{
    ChordNode caminho[CHORD_MAX_HOPS + 1];
    ChordNode dono;
    size_t pos = 0;
    int passos;
    int i;

    if (payload == NULL || tamanho != NODE_ID_LEN || tamanho_texto == 0) {
        return -1;
    }
    if (chord_find_successor(payload, caminho, &passos, &dono) != 0) {
        printf("Lookup: nao foi possivel achar o dono\n");
        return -1;
    }

    texto[0] = '\0';
    chord_append(texto, tamanho_texto, &pos, "Lookup path:\n");
    for (i = 0; i < passos; i++) {
        if (i > 0) {
            chord_append(texto, tamanho_texto, &pos, " ↓\n");
        }
        chord_append(texto, tamanho_texto, &pos, "%s\n", caminho[i].name);
    }
    chord_append(texto, tamanho_texto, &pos, "Owner: %s\n", dono.name);
    printf("%s", texto);
    return 0;
}

/* TOPOLOGY: monta o texto com o estado do no */
int chord_handle_topology(uint32_t tamanho, char *texto, size_t tamanho_texto)
{
    ChordNode successor;
    ChordNode pred;
    ChordNode finger[CHORD_FINGERS];
    size_t pos = 0;
    int has;

    if (tamanho != 0 || tamanho_texto == 0) {
        return -1;
    }
    if (pthread_mutex_lock(&chord_mutex) != 0) {
        return -1;
    }
    successor = chord_successor;
    pred = chord_predecessor;
    has = chord_has_predecessor;
    memcpy(finger, chord_finger, sizeof(finger));
    pthread_mutex_unlock(&chord_mutex);

    texto[0] = '\0';
    chord_append(texto, tamanho_texto, &pos, "Node: %s (%s:%u)\n",
                 chord_self.name, chord_self.ip, chord_self.port);
    chord_append(texto, tamanho_texto, &pos, "Successor: %s (%s:%u)\n",
                 successor.name, successor.ip, successor.port);
    if (has) {
        chord_append(texto, tamanho_texto, &pos, "Predecessor: %s (%s:%u)\n",
                     pred.name, pred.ip, pred.port);
    } else {
        chord_append(texto, tamanho_texto, &pos, "Predecessor: nenhum\n");
    }
    chord_format_fingers(finger, texto, tamanho_texto, &pos);
    printf("%s", texto);
    return 0;
}

/* ==========================================================================
 * CAMADA GOSSIP E HEARTBEAT (integrado de gossip.c)
 * ========================================================================== */

#define GOSSIP_MAX 64
#define GOSSIP_HEARTBEAT_SEC 5
#define GOSSIP_TIMEOUT_SEC 15

/* Maior entrada serializada: no + estado + contador de heartbeat */
#define GOSSIP_ENTRY_MAX (CHORD_NODE_MAX + 1 + sizeof(uint64_t))

/* Entrada da membership dos Super Peers */
typedef struct {
    ChordNode node;
    uint8_t state;
    uint64_t heartbeat;
    time_t last_seen;
} GossipMember;

/* Dados repassados para a thread periodica */
typedef struct {
    int has_bootstrap;
    ChordNode bootstrap;
} GossipStart;

/* Membership, contador proprio de heartbeat e o mutex que os protege */
static ChordNode gossip_self;
static uint64_t gossip_heartbeat = 0;
static GossipMember gossip_members[GOSSIP_MAX];
static int gossip_count = 0;
static pthread_mutex_t gossip_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Nome do estado para o log */
static const char *gossip_state_name(uint8_t state)
{
    static const char *nomes[] = {"ALIVE", "SUSPECT", "FAILED", "REMOVED"};

    return (state <= GOSSIP_REMOVED) ? nomes[state] : "UNKNOWN";
}

/* Procura o NodeID na membership, usar com o mutex travado */
static int gossip_find(const uint8_t id[NODE_ID_LEN])
{
    int i;

    for (i = 0; i < gossip_count; i++) {
        if (node_id_compare(gossip_members[i].node.id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* Aplica uma informacao recebida sobre um no, usar com o mutex travado */
static void gossip_apply(const ChordNode *no, uint8_t state, uint64_t heartbeat, const char *origem)
{
    GossipMember *membro;
    int indice;

    if (chord_same(no, &gossip_self) || state > GOSSIP_FAILED) {
        return;
    }

    indice = gossip_find(no->id);
    if (indice < 0) {
        if (gossip_count >= GOSSIP_MAX) {
            return;
        }
        membro = &gossip_members[gossip_count++];
        membro->node = *no;
        membro->state = state;
        membro->heartbeat = heartbeat;
        membro->last_seen = time(NULL);
        printf("Gossip: membro %s (%s:%u) %s\n", no->name, no->ip, no->port,
               gossip_state_name(state));
        return;
    }

    membro = &gossip_members[indice];

    /* Heartbeat mais novo de um no vivo: renova o prazo */
    if (state == GOSSIP_ALIVE && heartbeat > membro->heartbeat) {
        if (membro->state != GOSSIP_ALIVE) {
            printf("Gossip: %s voltou (%s -> ALIVE)\n", no->name, gossip_state_name(membro->state));
        }
        membro->node = *no;
        membro->state = GOSSIP_ALIVE;
        membro->heartbeat = heartbeat;
        membro->last_seen = time(NULL);
        return;
    }

    /* Suspeita de outro no sobre o mesmo heartbeat: adota o estado pior */
    if (state > membro->state && heartbeat >= membro->heartbeat && membro->state != GOSSIP_REMOVED) {
        membro->state = state;
        membro->heartbeat = heartbeat;
        printf("Gossip: %s %s por %s\n", no->name, gossip_state_name(state), origem);
    }
}

/* Avanca ALIVE -> SUSPECT -> FAILED -> REMOVED conforme o tempo sem heartbeat */
static void gossip_check_timeouts(void)
{
    time_t agora = time(NULL);
    long sem_sinal;
    int i;

    if (pthread_mutex_lock(&gossip_mutex) != 0) {
        return;
    }
    for (i = 0; i < gossip_count; i++) {
        GossipMember *membro = &gossip_members[i];

        sem_sinal = (long)(agora - membro->last_seen);
        if (membro->state == GOSSIP_ALIVE && sem_sinal > GOSSIP_TIMEOUT_SEC) {
            membro->state = GOSSIP_SUSPECT;
            printf("SUSPECT %s (%s:%u): sem heartbeat ha %ld s\n",
                   membro->node.name, membro->node.ip, membro->node.port, sem_sinal);
        } else if (membro->state == GOSSIP_SUSPECT && sem_sinal > 2 * GOSSIP_TIMEOUT_SEC) {
            membro->state = GOSSIP_FAILED;
            printf("FAILED %s (%s:%u): sem heartbeat ha %ld s\n",
                   membro->node.name, membro->node.ip, membro->node.port, sem_sinal);
        } else if (membro->state == GOSSIP_FAILED && sem_sinal > 3 * GOSSIP_TIMEOUT_SEC) {
            membro->state = GOSSIP_REMOVED;
            printf("REMOVED %s (%s:%u): retirado da membership\n",
                   membro->node.name, membro->node.ip, membro->node.port);
        }
    }
    pthread_mutex_unlock(&gossip_mutex);
}

/* Grava uma entrada: no | estado | heartbeat */
static uint32_t gossip_entry_write(const ChordNode *no, uint8_t state, uint64_t heartbeat,
                                   uint8_t *buffer)
{
    uint32_t pos = chord_node_write(no, buffer);

    buffer[pos++] = state;
    memcpy(buffer + pos, &heartbeat, sizeof(heartbeat));
    return pos + (uint32_t)sizeof(heartbeat);
}

/* Serializa a membership (este no primeiro) num buffer alocado */
static int gossip_table_write(uint8_t **buffer, uint32_t *tamanho)
{
    uint8_t *saida;
    uint32_t pos = 1;
    uint8_t total = 1;
    int i;

    if (pthread_mutex_lock(&gossip_mutex) != 0) {
        return -1;
    }
    saida = (uint8_t *)malloc(1 + (size_t)(GOSSIP_MAX + 1) * GOSSIP_ENTRY_MAX);
    if (saida == NULL) {
        pthread_mutex_unlock(&gossip_mutex);
        return -1;
    }

    pos += gossip_entry_write(&gossip_self, GOSSIP_ALIVE, gossip_heartbeat, saida + pos);
    for (i = 0; i < gossip_count; i++) {
        if (gossip_members[i].state != GOSSIP_REMOVED) {
            pos += gossip_entry_write(&gossip_members[i].node, gossip_members[i].state,
                                      gossip_members[i].heartbeat, saida + pos);
            total++;
        }
    }
    pthread_mutex_unlock(&gossip_mutex);

    saida[0] = total;
    *buffer = saida;
    *tamanho = pos;
    return 0;
}

/* Le uma entrada a partir de pos, validando cada campo */
static int gossip_entry_read(const uint8_t *buffer, uint32_t tamanho, uint32_t *pos,
                             GossipMember *entrada)
{
    memset(entrada, 0, sizeof(*entrada));
    if (chord_node_read(buffer, tamanho, pos, &entrada->node) != 0 ||
        !chord_has_bytes(tamanho, *pos, 1 + sizeof(uint64_t))) {
        return -1;
    }
    entrada->state = buffer[(*pos)++];
    memcpy(&entrada->heartbeat, buffer + *pos, sizeof(uint64_t));
    *pos += sizeof(uint64_t);
    return (entrada->state <= GOSSIP_FAILED) ? 0 : -1;
}

/* Valida a tabela recebida inteira e so depois mescla com a local */
static int gossip_table_merge(const uint8_t *buffer, uint32_t tamanho)
{
    GossipMember entradas[GOSSIP_MAX + 1];
    uint32_t pos = 1;
    int total;
    int i;

    if (buffer == NULL || tamanho < 1 || buffer[0] < 1 || buffer[0] > GOSSIP_MAX + 1) {
        return -1;
    }
    total = buffer[0];
    for (i = 0; i < total; i++) {
        if (gossip_entry_read(buffer, tamanho, &pos, &entradas[i]) != 0) {
            return -1;
        }
    }
    if (pos != tamanho) {
        return -1;
    }

    if (pthread_mutex_lock(&gossip_mutex) != 0) {
        return -1;
    }
    if (total > 0 && entradas[0].state == GOSSIP_ALIVE) {
        int idx0 = gossip_find(entradas[0].node.id);
        if (idx0 >= 0 && entradas[0].heartbeat < gossip_members[idx0].heartbeat) {
            gossip_members[idx0].heartbeat = 0;
        }
    }
    for (i = 0; i < total; i++) {
        gossip_apply(&entradas[i].node, entradas[i].state, entradas[i].heartbeat,
                     entradas[0].node.name);
    }
    pthread_mutex_unlock(&gossip_mutex);
    return 0;
}

/* Abre conexao, envia o pedido, espera o ACK e fecha */
static int gossip_request(const ChordNode *destino, uint16_t tipo, uint8_t *payload,
                          uint32_t tamanho, Mensagem *resposta)
{
    Mensagem pedido;
    int fd;
    int resultado;

    memset(resposta, 0, sizeof(*resposta));
    fd = conectar_no_servidor(destino->ip, destino->port);
    if (fd < 0) {
        printf("Gossip: %s nao respondeu\n", destino->name);
        return -1;
    }

    memset(&pedido, 0, sizeof(pedido));
    pedido.header.versao_protocolo = VERSAO_PROTOCOLO;
    pedido.header.tipo_mensagem = tipo;
    memcpy(pedido.header.no_origem, gossip_self.id, NODE_ID_LEN);
    memcpy(pedido.header.no_destino, destino->id, NODE_ID_LEN);
    pedido.header.timestamp = (uint64_t)time(NULL);
    pedido.header.tamanho_payload = tamanho;
    pedido.payload = payload;

    resultado = enviar_mensagem(fd, &pedido);
    if (resultado == 0) {
        resultado = receber_mensagem(fd, resposta);
    }
    close(fd);

    if (resultado != 0) {
        liberar_mensagem(resposta);
        printf("Gossip: %s nao respondeu\n", destino->name);
        return -1;
    }
    if (resposta->header.tipo_mensagem != MSG_ACK) {
        liberar_mensagem(resposta);
        printf("Gossip: %s recusou %s\n", destino->name, nome_do_tipo(tipo));
        return -1;
    }
    return 0;
}

/* Envia o heartbeat direto para um membro */
static void gossip_send_heartbeat(const ChordNode *destino, uint64_t heartbeat)
{
    uint8_t pedido[GOSSIP_ENTRY_MAX];
    uint32_t tamanho = chord_node_write(&gossip_self, pedido);
    Mensagem resposta;

    memcpy(pedido + tamanho, &heartbeat, sizeof(heartbeat));
    tamanho += (uint32_t)sizeof(heartbeat);
    if (gossip_request(destino, MSG_HEARTBEAT, pedido, tamanho, &resposta) == 0) {
        liberar_mensagem(&resposta);
    }
}

/* Troca a membership com um no (push-pull) */
static void gossip_exchange(const ChordNode *destino)
{
    uint8_t *tabela;
    uint32_t tamanho;
    Mensagem resposta;

    if (gossip_table_write(&tabela, &tamanho) != 0) {
        return;
    }
    if (gossip_request(destino, MSG_GOSSIP, tabela, tamanho, &resposta) == 0) {
        if (gossip_table_merge(resposta.payload, resposta.header.tamanho_payload) != 0) {
            printf("Gossip: resposta invalida de %s\n", destino->name);
        }
        liberar_mensagem(&resposta);
    }
    free(tabela);
}

/* Rodada: heartbeat para todos os membros vivos e gossip com um deles */
static void gossip_round(const GossipStart *inicio, unsigned int *semente)
{
    ChordNode alvos[GOSSIP_MAX];
    int total = 0;
    int vivos = 0;
    int escolhido = -1;
    uint64_t heartbeat;
    int i;

    if (pthread_mutex_lock(&gossip_mutex) != 0) {
        return;
    }
    heartbeat = ++gossip_heartbeat;
    for (i = 0; i < gossip_count; i++) {
        if (gossip_members[i].state == GOSSIP_ALIVE || gossip_members[i].state == GOSSIP_SUSPECT) {
            alvos[total++] = gossip_members[i].node;
            if (gossip_members[i].state == GOSSIP_ALIVE) {
                vivos++;
            }
        }
    }
    pthread_mutex_unlock(&gossip_mutex);

    if (total > 0) {
        printf("Gossip: heartbeat %llu enviado a %d membros\n", (unsigned long long)heartbeat, total);
    }
    for (i = 0; i < total; i++) {
        gossip_send_heartbeat(&alvos[i], heartbeat);
    }

    /* Sorteia um membro vivo; sem nenhum, usa o bootstrap */
    if (vivos > 0) {
        escolhido = (int)(rand_r(semente) % (unsigned int)total);
        gossip_exchange(&alvos[escolhido]);
    } else if (inicio->has_bootstrap) {
        gossip_exchange(&inicio->bootstrap);
    }
}

/* Rotina periodica: confere timeouts a cada 1 s e faz uma rodada a cada 5 s */
static void *gossip_loop(void *arg)
{
    GossipStart inicio = *(GossipStart *)arg;
    unsigned int semente = (unsigned int)time(NULL) ^ gossip_self.port;
    unsigned int segundos = 0;

    free(arg);
    for (;;) {
        if (segundos % GOSSIP_HEARTBEAT_SEC == 0) {
            gossip_round(&inicio, &semente);
        }
        gossip_check_timeouts();
        sleep(1);
        segundos++;
    }
    return NULL;
}

/* Inicia a membership e a thread de heartbeat e gossip */
int gossip_start(const Node *no, const char *nome, const char *bootstrap_ip,
                 uint16_t bootstrap_port)
{
    GossipStart *inicio;
    pthread_t thread;

    memset(&gossip_self, 0, sizeof(gossip_self));
    memcpy(gossip_self.id, no->node_id, NODE_ID_LEN);
    snprintf(gossip_self.ip, sizeof(gossip_self.ip), "%s", no->ip);
    gossip_self.port = no->port;
    snprintf(gossip_self.name, sizeof(gossip_self.name), "%s", nome);

    /* O contador de heartbeat comeca no horario de inicio (numero de encarnacao):
     * um no reiniciado envia heartbeats maiores que os da execucao anterior e os
     * outros o reconhecem como ALIVE no primeiro heartbeat (Secoes 7.5 e 15.7). */
    gossip_heartbeat = (uint64_t)time(NULL);

    inicio = (GossipStart *)malloc(sizeof(GossipStart));
    if (inicio == NULL) {
        return -1;
    }
    memset(inicio, 0, sizeof(*inicio));

    /* Bootstrap igual a este no nao serve de contato inicial */
    if (bootstrap_port != 0 &&
        !(bootstrap_port == no->port && strcmp(bootstrap_ip, no->ip) == 0)) {
        inicio->has_bootstrap = 1;
        snprintf(inicio->bootstrap.ip, sizeof(inicio->bootstrap.ip), "%s", bootstrap_ip);
        inicio->bootstrap.port = bootstrap_port;
        snprintf(inicio->bootstrap.name, sizeof(inicio->bootstrap.name), "%s:%u",
                 bootstrap_ip, bootstrap_port);
    }
    printf("Gossip: heartbeat a cada %d s, timeout de %d s\n", GOSSIP_HEARTBEAT_SEC,
           GOSSIP_TIMEOUT_SEC);

    if (pthread_create(&thread, NULL, gossip_loop, inicio) != 0) {
        free(inicio);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

/* HEARTBEAT: no + contador; renova o prazo de quem enviou (mesmo apos reinicio) */
int gossip_handle_heartbeat(const uint8_t *payload, uint32_t tamanho)
{
    ChordNode no;
    uint64_t heartbeat;
    uint32_t pos = 0;
    int indice;

    if (chord_node_read(payload, tamanho, &pos, &no) != 0 || tamanho - pos != sizeof(uint64_t)) {
        return -1;
    }
    memcpy(&heartbeat, payload + pos, sizeof(uint64_t));

    if (pthread_mutex_lock(&gossip_mutex) != 0) {
        return -1;
    }
    indice = gossip_find(no.id);
    if (indice >= 0 && heartbeat < gossip_members[indice].heartbeat) {
        gossip_members[indice].heartbeat = 0;
    }
    gossip_apply(&no, GOSSIP_ALIVE, heartbeat, no.name);
    pthread_mutex_unlock(&gossip_mutex);
    return 0;
}

/* GOSSIP: mescla a tabela recebida e responde com a propria */
int gossip_handle_gossip(const uint8_t *payload, uint32_t tamanho, uint8_t **resposta,
                         uint32_t *tamanho_resposta)
{
    if (gossip_table_merge(payload, tamanho) != 0) {
        return -1;
    }
    return gossip_table_write(resposta, tamanho_resposta);
}

/* Copia os membros ALIVE em out e retorna quantos */
int gossip_alive_members(ChordNode *out, int max)
{
    int total = 0;
    int i;

    if (out == NULL || max <= 0 || pthread_mutex_lock(&gossip_mutex) != 0) {
        return 0;
    }
    for (i = 0; i < gossip_count && total < max; i++) {
        if (gossip_members[i].state == GOSSIP_ALIVE) {
            out[total++] = gossip_members[i].node;
        }
    }
    pthread_mutex_unlock(&gossip_mutex);
    return total;
}

/* Estado do membro na membership, ou -1 se nao esta nela */
int gossip_member_state(const uint8_t id[NODE_ID_LEN])
{
    int estado = -1;
    int indice;

    if (id == NULL || pthread_mutex_lock(&gossip_mutex) != 0) {
        return -1;
    }
    indice = gossip_find(id);
    if (indice >= 0) {
        estado = gossip_members[indice].state;
    }
    pthread_mutex_unlock(&gossip_mutex);
    return estado;
}

/* ==========================================================================
 * CAMADA ELEICAO BULLY (integrado de bully.c)
 * ========================================================================== */

/* Maior quantidade de membros consultados por vez */
#define BULLY_MAX 64

/* Estado da eleicao e o mutex que o protege */
static ChordNode bully_self;
static ChordNode bully_coord;
static int bully_has_coord = 0;
static BullyState bully_state = BULLY_FOLLOWER;
static int bully_requested = 0;
static time_t bully_deadline = 0;
static pthread_mutex_t bully_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Copia do estado lida pela thread periodica */
typedef struct {
    BullyState state;
    int has_coord;
    ChordNode coord;
    int requested;
    time_t deadline;
} BullySnapshot;

/* Abre conexao, envia este no no payload, guarda o tipo da resposta e fecha */
static int bully_request(const ChordNode *destino, uint16_t tipo, uint16_t *tipo_resposta)
{
    uint8_t payload[CHORD_NODE_MAX];
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
    pedido.header.tipo_mensagem = tipo;
    memcpy(pedido.header.no_origem, bully_self.id, NODE_ID_LEN);
    memcpy(pedido.header.no_destino, destino->id, NODE_ID_LEN);
    pedido.header.timestamp = (uint64_t)time(NULL);
    pedido.header.tamanho_payload = chord_node_write(&bully_self, payload);
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

/* Copia o estado com o mutex travado */
static int bully_snapshot(BullySnapshot *copia)
{
    if (pthread_mutex_lock(&bully_mutex) != 0) {
        return -1;
    }
    copia->state = bully_state;
    copia->has_coord = bully_has_coord;
    copia->coord = bully_coord;
    copia->requested = bully_requested;
    copia->deadline = bully_deadline;
    pthread_mutex_unlock(&bully_mutex);
    return 0;
}

/* Muda o estado; com novo coordenador, grava (coord NULL apaga) */
static void bully_set(BullyState state, const ChordNode *coord, time_t deadline)
{
    if (pthread_mutex_lock(&bully_mutex) != 0) {
        return;
    }
    bully_state = state;
    bully_deadline = deadline;
    bully_requested = 0;
    if (coord != NULL) {
        bully_coord = *coord;
        bully_has_coord = 1;
    } else {
        bully_has_coord = 0;
    }
    pthread_mutex_unlock(&bully_mutex);
}

/* Manda COORDINATOR a todos os membros ALIVE */
static void bully_announce(void)
{
    ChordNode membros[BULLY_MAX];
    uint16_t tipo;
    int total = gossip_alive_members(membros, BULLY_MAX);
    int i;

    for (i = 0; i < total; i++) {
        bully_request(&membros[i], MSG_COORDINATOR, &tipo);
    }
}

/* Ninguem maior respondeu: assume a coordenacao e avisa todos */
static void bully_become_coordinator(void)
{
    bully_set(BULLY_COORDINATOR, &bully_self, 0);
    printf("highest NodeID wins: %s\n", bully_self.name);
    printf("COORDINATOR %s\n", bully_self.name);
    bully_announce();
    printf("Coordenador: %s (este no)\n", bully_self.name);
    printf("resume operations (proximo LOG #%llu)\n",
           (unsigned long long)(smr_last_applied() + 1));
}

/* Eleicao: ELECTION a cada membro maior; sem OK, este no vence */
static void bully_election(void)
{
    ChordNode membros[BULLY_MAX];
    uint16_t tipo;
    int total;
    int recebeu_ok = 0;
    int i;

    bully_set(BULLY_CANDIDATE, NULL, 0);
    printf("start election\n");

    total = gossip_alive_members(membros, BULLY_MAX);
    for (i = 0; i < total; i++) {
        if (node_id_compare(membros[i].id, bully_self.id) <= 0) {
            continue;
        }
        printf("ELECTION -> %s\n", membros[i].name);
        if (bully_request(&membros[i], MSG_ELECTION, &tipo) == 0 && tipo == MSG_OK) {
            printf("OK <- %s\n", membros[i].name);
            recebeu_ok = 1;
        }
    }

    if (!recebeu_ok) {
        bully_become_coordinator();
        return;
    }

    /* espera o COORDINATOR, a menos que ele ja tenha chegado */
    if (pthread_mutex_lock(&bully_mutex) != 0) {
        return;
    }
    if (bully_state == BULLY_CANDIDATE) {
        bully_deadline = time(NULL) + BULLY_WAIT_SEC;
    }
    pthread_mutex_unlock(&bully_mutex);
}

/* Diz se o coordenador ainda esta ALIVE na membership */
static int bully_coord_alive(const ChordNode *coord)
{
    return gossip_member_state(coord->id) == GOSSIP_ALIVE;
}

/* Uma rodada da thread periodica */
static void bully_tick(time_t *ultimo_anuncio)
{
    BullySnapshot copia;
    time_t agora = time(NULL);

    if (bully_snapshot(&copia) != 0) {
        return;
    }

    if (copia.requested && copia.state != BULLY_CANDIDATE) {
        bully_election();
    } else if (copia.state == BULLY_CANDIDATE) {
        if (copia.deadline != 0 && agora >= copia.deadline) {
            printf("sem COORDINATOR, nova eleicao\n");
            bully_election();
        }
    } else if (copia.state == BULLY_COORDINATOR) {
        if (agora - *ultimo_anuncio >= BULLY_ANNOUNCE_SEC) {
            bully_announce();
            *ultimo_anuncio = agora;
        }
    } else if (!copia.has_coord) {
        bully_election();
    } else if (!chord_same(&copia.coord, &bully_self) && !bully_coord_alive(&copia.coord)) {
        printf("detect failure: %s (coordenador)\n", copia.coord.name);
        bully_set(BULLY_FOLLOWER, NULL, 0);
        bully_election();
    } else if (!chord_same(&copia.coord, &bully_self) &&
               agora - *ultimo_anuncio >= BULLY_ANNOUNCE_SEC) {
        smr_sync_with_coordinator(&copia.coord);
        *ultimo_anuncio = agora;
    }
}

/* Rotina periodica: espera a membership e confere a cada BULLY_PERIOD_SEC */
static void *bully_loop(void *arg)
{
    time_t ultimo_anuncio = time(NULL);

    (void)arg;
    sleep(BULLY_START_SEC);
    bully_election();
    for (;;) {
        sleep(BULLY_PERIOD_SEC);
        bully_tick(&ultimo_anuncio);
    }
    return NULL;
}

/* Grava este no e cria a thread da eleicao */
int bully_start(const Node *no, const char *nome)
{
    pthread_t thread;

    memset(&bully_self, 0, sizeof(bully_self));
    memcpy(bully_self.id, no->node_id, NODE_ID_LEN);
    snprintf(bully_self.ip, sizeof(bully_self.ip), "%s", no->ip);
    bully_self.port = no->port;
    snprintf(bully_self.name, sizeof(bully_self.name), "%s", nome);

    printf("Bully: eleicao em %d s, espera de COORDINATOR de %d s\n", BULLY_START_SEC,
           BULLY_WAIT_SEC);
    if (pthread_create(&thread, NULL, bully_loop, NULL) != 0) {
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

/* ELECTION: no menor recebe OK e este no faz a propria eleicao */
int bully_handle_election(const uint8_t *payload, uint32_t tamanho)
{
    ChordNode no;
    uint32_t pos = 0;

    if (chord_node_read(payload, tamanho, &pos, &no) != 0 || pos != tamanho) {
        return -1;
    }
    if (node_id_compare(no.id, bully_self.id) >= 0) {
        return 0;
    }
    if (pthread_mutex_lock(&bully_mutex) != 0) {
        return -1;
    }
    if (bully_state != BULLY_CANDIDATE) {
        bully_requested = 1;
    }
    pthread_mutex_unlock(&bully_mutex);
    return 1;
}

/* COORDINATOR: grava o coordenador e vira FOLLOWER */
int bully_handle_coordinator(const uint8_t *payload, uint32_t tamanho)
{
    ChordNode no;
    uint32_t pos = 0;
    int mudou;

    if (chord_node_read(payload, tamanho, &pos, &no) != 0 || pos != tamanho) {
        return -1;
    }
    if (pthread_mutex_lock(&bully_mutex) != 0) {
        return -1;
    }
    mudou = !bully_has_coord || !chord_same(&bully_coord, &no) || bully_state != BULLY_FOLLOWER;
    bully_coord = no;
    bully_has_coord = 1;
    bully_state = BULLY_FOLLOWER;
    bully_deadline = 0;
    bully_requested = 0;
    pthread_mutex_unlock(&bully_mutex);

    if (mudou) {
        printf("COORDINATOR %s\n", no.name);
        printf("Coordenador: %s\n", no.name);
        smr_sync_with_coordinator(&no);
    }
    return 0;
}

/* Copia o coordenador atual; se ainda nao houve eleicao e o no esta isolado, usa si mesmo */
int bully_get_coordinator(ChordNode *out)
{
    int tem;

    if (out == NULL || pthread_mutex_lock(&bully_mutex) != 0) {
        return 0;
    }
    tem = bully_has_coord;
    if (tem) {
        *out = bully_coord;
    } else if (bully_state == BULLY_FOLLOWER) {
        ChordNode membros[1];
        if (gossip_alive_members(membros, 1) == 0) {
            *out = bully_self;
            tem = 1;
        }
    }
    pthread_mutex_unlock(&bully_mutex);
    return tem;
}

/* ==========================================================================
 * CAMADA SMR, 2PC E IST (integrado de smr.c)
 * ========================================================================== */

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

    {
        FileMetadata antigo;
        if (metadata_table_find_by_name(meta->filename, &antigo) == METADATA_OK) {
            cache_invalidate(antigo.object_id);
            metadata_free(&antigo);
        }
    }
    smr_print(e, meta->filename);
    if (metadata_table_insert(meta) != METADATA_OK) {
        printf("SMR: falha ao aplicar LOG #%llu\n", (unsigned long long)e->log_index);
        free(copia_raw);
        return -1;
    }
    metadata_add_provider(meta->object_id, provider);
    cache_invalidate(meta->object_id);
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

/* ==========================================================================
 * CAMADA CACHE LFU (integrado de cache.c)
 * ========================================================================== */

/* Entrada da cache LFU */
typedef struct {
    int used;
    FileMetadata meta;
    uint64_t frequency;
    uint64_t last_access;
} CacheEntry;

/* Estado da cache LFU, relogio logico de acesso, metricas e mutex */
static CacheEntry cache_entries[CACHE_CAPACITY];
static uint64_t cache_clock = 0;
static uint64_t cache_hits = 0;
static uint64_t cache_misses = 0;
static uint64_t cache_evictions = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Procura uma entrada pelo nome ou basename; usar com cache_mutex travado */
static int cache_find_name(const char *filename)
{
    int i;

    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (!cache_entries[i].used) {
            continue;
        }
        if (strcmp(cache_entries[i].meta.filename, filename) == 0) {
            return i;
        }
        const char *slash = strrchr(cache_entries[i].meta.filename, '/');
        if (slash != NULL && strcmp(slash + 1, filename) == 0) {
            return i;
        }
    }
    return -1;
}

/* Procura uma entrada pelo ObjectID; usar com cache_mutex travado */
static int cache_find_id(const uint8_t object_id[METADATA_ID_LEN])
{
    int i;

    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (cache_entries[i].used &&
            memcmp(cache_entries[i].meta.object_id, object_id, METADATA_ID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* Escolhe uma posicao livre ou a vitima LFU (menor frequency, desempate menor last_access) */
static int cache_select_slot(int *foi_evict)
{
    int vitima = 0;
    int i;

    *foi_evict = 0;
    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (!cache_entries[i].used) {
            return i;
        }
    }

    *foi_evict = 1;
    for (i = 1; i < CACHE_CAPACITY; i++) {
        if (cache_entries[i].frequency < cache_entries[vitima].frequency ||
            (cache_entries[i].frequency == cache_entries[vitima].frequency &&
             cache_entries[i].last_access < cache_entries[vitima].last_access)) {
            vitima = i;
        }
    }
    return vitima;
}

/* Busca pelo nome na cache LFU */
int cache_get_by_name(const char *filename, FileMetadata *out)
{
    int indice;
    int resultado = -1;

    if (filename == NULL || out == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&cache_mutex) != 0) {
        return -1;
    }

    indice = cache_find_name(filename);
    if (indice >= 0) {
        if (metadata_copy(out, &cache_entries[indice].meta) == METADATA_OK) {
            cache_entries[indice].frequency++;
            cache_entries[indice].last_access = ++cache_clock;
            cache_hits++;
            printf("LFU CACHE HIT: %s (frequency=%llu, hits=%llu, misses=%llu)\n",
                   filename,
                   (unsigned long long)cache_entries[indice].frequency,
                   (unsigned long long)cache_hits,
                   (unsigned long long)cache_misses);
            resultado = 0;
        }
    } else {
        cache_misses++;
        printf("LFU CACHE MISS: %s (hits=%llu, misses=%llu)\n",
               filename,
               (unsigned long long)cache_hits,
               (unsigned long long)cache_misses);
    }

    pthread_mutex_unlock(&cache_mutex);
    return resultado;
}

/* Insere ou atualiza um metadado na cache LFU */
int cache_put(const FileMetadata *meta)
{
    FileMetadata copia;
    int indice;
    int foi_evict = 0;

    if (meta == NULL || strnlen(meta->filename, METADATA_NAME_LEN) == 0) {
        return -1;
    }
    if (metadata_copy(&copia, meta) != METADATA_OK) {
        return -1;
    }
    if (pthread_mutex_lock(&cache_mutex) != 0) {
        metadata_free(&copia);
        return -1;
    }

    indice = cache_find_id(meta->object_id);
    if (indice < 0) {
        indice = cache_find_name(meta->filename);
    }

    if (indice >= 0) {
        metadata_free(&cache_entries[indice].meta);
        cache_entries[indice].meta = copia;
        cache_entries[indice].frequency++;
        cache_entries[indice].last_access = ++cache_clock;
        pthread_mutex_unlock(&cache_mutex);
        return 0;
    }

    indice = cache_select_slot(&foi_evict);
    if (foi_evict) {
        cache_evictions++;
        printf("LFU CACHE EVICT: %s (frequency=%llu, last_access=%llu, evictions=%llu)\n",
               cache_entries[indice].meta.filename,
               (unsigned long long)cache_entries[indice].frequency,
               (unsigned long long)cache_entries[indice].last_access,
               (unsigned long long)cache_evictions);
        metadata_free(&cache_entries[indice].meta);
    }

    cache_entries[indice].used = 1;
    cache_entries[indice].meta = copia;
    cache_entries[indice].frequency = 1;
    cache_entries[indice].last_access = ++cache_clock;
    printf("LFU CACHE INSERT: %s (frequency=1, slot=%d/%d)\n",
           copia.filename, indice, CACHE_CAPACITY);

    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

/* Remove uma entrada da cache pelo ObjectID */
void cache_invalidate(const uint8_t object_id[METADATA_ID_LEN])
{
    int indice;

    if (object_id == NULL || pthread_mutex_lock(&cache_mutex) != 0) {
        return;
    }
    indice = cache_find_id(object_id);
    if (indice >= 0) {
        printf("LFU CACHE INVALIDATE: %s\n", cache_entries[indice].meta.filename);
        metadata_free(&cache_entries[indice].meta);
        cache_entries[indice].used = 0;
        cache_entries[indice].frequency = 0;
        cache_entries[indice].last_access = 0;
    }
    pthread_mutex_unlock(&cache_mutex);
}

/* Imprime as metricas acumuladas da cache */
void cache_print_stats(void)
{
    uint64_t total;
    double taxa = 0.0;

    if (pthread_mutex_lock(&cache_mutex) != 0) {
        return;
    }
    total = cache_hits + cache_misses;
    if (total > 0) {
        taxa = (100.0 * (double)cache_hits) / (double)total;
    }
    printf("LFU Cache stats: hits=%llu misses=%llu evictions=%llu hit_rate=%.1f%%\n",
           (unsigned long long)cache_hits,
           (unsigned long long)cache_misses,
           (unsigned long long)cache_evictions,
           taxa);
    pthread_mutex_unlock(&cache_mutex);
}
