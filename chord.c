#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define CHORD_FINGERS 256
#define CHORD_PERIOD_SEC 1
#define CHORD_MAX_HOPS 32
#define CHORD_NAME_LEN 64
#define CHORD_IP_MIN 7
#define CHORD_TEXT_LEN 32768

/* Maior no serializado: id + porta + ip_len + ip + name_len + nome */
#define CHORD_NODE_MAX (NODE_ID_LEN + 2 + 1 + (NODE_IP_LEN - 1) + 1 + (CHORD_NAME_LEN - 1))

/* No do anel */
typedef struct {
    uint8_t id[NODE_ID_LEN];
    char ip[NODE_IP_LEN];
    uint16_t port;
    char name[CHORD_NAME_LEN];
} ChordNode;

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
static int chord_same(const ChordNode *a, const ChordNode *b)
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
static uint32_t chord_node_write(const ChordNode *no, uint8_t *buffer)
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
static int chord_has_bytes(uint32_t size, uint32_t pos, uint32_t n)
{
    return pos <= size && size - pos >= n;
}

/* Le um no do buffer a partir de pos, validando cada campo */
static int chord_node_read(const uint8_t *buffer, uint32_t size, uint32_t *pos, ChordNode *no)
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

    /* Successor remoto: pergunta pela rede */
    if (!chord_same(&successor, &chord_self) &&
        chord_ask_predecessor(&successor, &x, &has_x) != 0) {
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
