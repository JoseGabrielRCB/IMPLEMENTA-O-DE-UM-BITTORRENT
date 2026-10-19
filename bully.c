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
#include "include/smr.h"
#include "include/bully.h"

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
    }
}

/* Rotina periodica: espera a membership e confere a cada BULLY_PERIOD_SEC */
static void *bully_loop(void *arg)
{
    time_t ultimo_anuncio = time(NULL);

    (void)arg;
    sleep(BULLY_START_SEC);
    for (;;) {
        bully_tick(&ultimo_anuncio);
        sleep(BULLY_PERIOD_SEC);
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
    }
    return 0;
}

/* Copia o coordenador atual, se houver */
int bully_get_coordinator(ChordNode *out)
{
    int tem;

    if (out == NULL || pthread_mutex_lock(&bully_mutex) != 0) {
        return 0;
    }
    tem = bully_has_coord;
    if (tem) {
        *out = bully_coord;
    }
    pthread_mutex_unlock(&bully_mutex);
    return tem;
}
