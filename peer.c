#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "node.c"
#include "network.c"

int main(int argc, char *argv[]) {
    char host[256] = "127.0.0.1";
    int porta = 55101;
    char cmd[256] = "ping";

    // identidade do proprio peer, com valores padrao
    char ip_local[NODE_IP_LEN] = "127.0.0.1";
    char porta_local[8] = "55102";
    char arquivo_uuid[NODE_PATH_LEN] = "peer.uuid";
    NodeConfig config;
    Node peer;
    char endereco[32];

    struct option long_options[] = {
        {"cmd", required_argument, 0, 'c'},
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"ip", required_argument, 0, 'i'},
        {"myport", required_argument, 0, 'm'},
        {"uuid", required_argument, 0, 'u'},
        {0, 0, 0, 0}
    };
    
    // leitura dos parametros de disparo (adaptado para ser o peer/cliente)
    int opt;
    while ((opt = getopt_long(argc, argv, "c:h:p:i:m:u:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': snprintf(cmd, sizeof(cmd), "%s", optarg); break;
            case 'h': snprintf(host, sizeof(host), "%s", optarg); break;
            case 'p': porta = atoi(optarg); break;
            case 'i': snprintf(ip_local, sizeof(ip_local), "%s", optarg); break;
            case 'm': snprintf(porta_local, sizeof(porta_local), "%s", optarg); break;
            case 'u': snprintf(arquivo_uuid, sizeof(arquivo_uuid), "%s", optarg); break;
        }
    }

    // monta e valida a configuracao antes de gerar o NodeID
    memset(&config, 0, sizeof(config));
    config.role = ROLE_PEER;
    if (node_parse_ip(ip_local, config.ip, sizeof(config.ip)) != 0) {
        fprintf(stderr, "Erro: IP invalido: %s\n", ip_local);
        return 1;
    }
    if (node_parse_port(porta_local, &config.port) != 0) {
        fprintf(stderr, "Erro: porta invalida: %s\n", porta_local);
        return 1;
    }
    if (porta < 1 || porta > 65535) {
        fprintf(stderr, "Erro: porta do superpeer invalida: %d\n", porta);
        return 1;
    }
    if (node_parse_ip(host, config.superpeer_ip, sizeof(config.superpeer_ip)) != 0) {
        fprintf(stderr, "Erro: IP do superpeer invalido: %s\n", host);
        return 1;
    }
    config.superpeer_port = (uint16_t)porta;
    snprintf(config.uuid_path, sizeof(config.uuid_path), "%s", arquivo_uuid);

    if (node_init(&peer, &config) != 0) {
        return 1;
    }
    node_print(&peer);
    
    peer.state = STATE_CONNECTING;
    int socket_fd = conectar_no_servidor(host, porta);
    if (socket_fd < 0) return 1;
    peer.state = STATE_CONNECTED;
    
    // formata pacote com o tamanho exato da string usada
    Mensagem msg;
    memset(&msg, 0, sizeof(Mensagem));
    msg.header.versao_protocolo = 1;

    // identifica a origem do pacote com o NodeID do peer
    memcpy(msg.header.no_origem, peer.node_id, NODE_ID_LEN);
    
    if (strcmp(cmd, "ping") == 0) {
        msg.header.tipo_mensagem = MSG_PING;
        msg.header.tamanho_payload = 4;
        msg.payload = (uint8_t*)strdup("PING");
    } else if (strcmp(cmd, "join") == 0) {
        // o JOIN leva o endereco do peer para a tabela do super peer
        snprintf(endereco, sizeof(endereco), "%s:%u", peer.ip, peer.port);
        msg.header.tipo_mensagem = MSG_JOIN;
        msg.header.tamanho_payload = (uint32_t)strlen(endereco);
        msg.payload = (uint8_t*)strdup(endereco);
    } else if (strcmp(cmd, "leave") == 0) {
        msg.header.tipo_mensagem = MSG_LEAVE;
        msg.header.tamanho_payload = 5;
        msg.payload = (uint8_t*)strdup("LEAVE");
    } else {
        msg.header.tipo_mensagem = MSG_PING;
        msg.header.tamanho_payload = 4;
        msg.payload = (uint8_t*)strdup("PING");
    }

    // sem payload nao ha o que enviar
    if (msg.payload == NULL) {
        close(socket_fd);
        return 1;
    }
    
    // envia instrucao e aguarda imediato retorno para log
    if (enviar_mensagem(socket_fd, &msg) == 0) {
        Mensagem resposta;
        memset(&resposta, 0, sizeof(Mensagem));
        if (receber_mensagem(socket_fd, &resposta) == 0) {
            printf("RX %s\n", nome_do_tipo(resposta.header.tipo_mensagem));

            // o ACK do JOIN encerra o handshake
            if (msg.header.tipo_mensagem == MSG_JOIN && resposta.header.tipo_mensagem == MSG_ACK) {
                peer.state = STATE_AUTHENTICATED;
                printf("Estado: %s\n", node_state_name(peer.state));
            }
        }
        liberar_mensagem(&resposta);
    }
    
    liberar_mensagem(&msg);
    close(socket_fd);
    return 0;
}
