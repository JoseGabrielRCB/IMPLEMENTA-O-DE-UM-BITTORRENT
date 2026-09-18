#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include "node.c"
#include "network.c"

/* Versao de protocolo aceita neste checkpoint */
#define VERSAO_PROTOCOLO 1

/* Valores usados quando o no e iniciado pelas opcoes longas */
#define IP_PADRAO "127.0.0.1"
#define PORTA_PADRAO "55101"
#define UUID_PADRAO "superpeer.uuid"
#define NOME_PADRAO "superpeer"
#define NOME_MAX 64

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

/* Monta e envia uma resposta sem payload */
static int responder(const Conexao *conexao, uint16_t tipo, const uint8_t destino[NODE_ID_LEN])
{
    Mensagem resposta;

    memset(&resposta, 0, sizeof(resposta));
    resposta.header.versao_protocolo = VERSAO_PROTOCOLO;
    resposta.header.tipo_mensagem = tipo;
    memcpy(resposta.header.no_origem, conexao->superpeer->node_id, NODE_ID_LEN);
    memcpy(resposta.header.no_destino, destino, NODE_ID_LEN);
    resposta.header.timestamp = (uint64_t)time(NULL);

    return enviar_mensagem(conexao->socket, &resposta);
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
        default:
            printf("Tipo de mensagem fora do checkpoint 1\n");
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

/* Le a configuracao no formato de opcoes longas */
static int ler_opcoes(int argc, char *argv[], NodeConfig *config, char *nome, size_t tamanho_nome)
{
    char porta[8];
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
    snprintf(nome, tamanho_nome, "%s", NOME_PADRAO);
    snprintf(config->uuid_path, sizeof(config->uuid_path), "%s", UUID_PADRAO);

    while ((opt = getopt_long(argc, argv, "c:p:n:", longas, NULL)) != -1) {
        switch (opt) {
        case 'c':
            /* o arquivo de configuracao nao e usado neste checkpoint */
            break;
        case 'p':
            snprintf(porta, sizeof(porta), "%s", optarg);
            break;
        case 'n':
            snprintf(nome, tamanho_nome, "%s", optarg);
            break;
        default:
            return -1;
        }
    }

    if (node_parse_ip(IP_PADRAO, config->ip, sizeof(config->ip)) != 0) {
        return -1;
    }
    if (node_parse_port(porta, &config->port) != 0) {
        fprintf(stderr, "Erro: porta invalida: %s\n", porta);
        return -1;
    }
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
