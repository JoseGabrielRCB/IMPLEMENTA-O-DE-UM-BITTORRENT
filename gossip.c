#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "include/network.h"
#include "include/node.h"
#include "include/chord.h"
#include "include/gossip.h"

#define GOSSIP_MAX 64
#define GOSSIP_HEARTBEAT_SEC 5
#define GOSSIP_TIMEOUT_SEC 15

/* Maior entrada serializada: no + estado + contador de heartbeat */
#define GOSSIP_ENTRY_MAX (CHORD_NODE_MAX + 1 + sizeof(uint64_t))

/* Estados de um Super Peer na membership */
typedef enum {
    GOSSIP_ALIVE = 0,
    GOSSIP_SUSPECT = 1,
    GOSSIP_FAILED = 2,
    GOSSIP_REMOVED = 3
} GossipState;

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

/* HEARTBEAT: no + contador; renova o prazo de quem enviou */
int gossip_handle_heartbeat(const uint8_t *payload, uint32_t tamanho)
{
    ChordNode no;
    uint64_t heartbeat;
    uint32_t pos = 0;

    if (chord_node_read(payload, tamanho, &pos, &no) != 0 || tamanho - pos != sizeof(uint64_t)) {
        return -1;
    }
    memcpy(&heartbeat, payload + pos, sizeof(uint64_t));

    if (pthread_mutex_lock(&gossip_mutex) != 0) {
        return -1;
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
