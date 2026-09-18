#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

// inclui arquivo de protocolo 
#include "protocol.c"

// framing, mantem o laco ate enviar todos os bytes solicitados 
static int enviar(int socket, const uint8_t *buffer, size_t tamanho) {
    size_t enviados = 0;
    while (enviados < tamanho) {
        ssize_t w = send(socket, buffer + enviados, tamanho - enviados, 0);
        if (w <= 0) return -1;
        enviados += w;
    }
    return 0;
}

// framing, mantem o laco ate ler todos os bytes solicitados
static int ler(int socket, uint8_t *buffer, size_t tamanho) {
    size_t lidos = 0;
    while (lidos < tamanho) {
        ssize_t r = recv(socket, buffer + lidos, tamanho - lidos, 0);
        if (r <= 0) return -1;
        lidos += r;
    }
    return 0;
}

// inicia a escuta do servidor na porta especificada
int criar_servidor(int porta) {
    int servidor_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor_fd < 0) return -1;

    // evita o bloqueio da porta pelo sistema operacional
    int opt = 1;
    setsockopt(servidor_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in endereco;
    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = INADDR_ANY;
    endereco.sin_port = htons(porta);

    // vincula o socket a porta
    if (bind(servidor_fd, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {
        close(servidor_fd);
        return -1;
    }

    // define o limite da fila de clientes
    if (listen(servidor_fd, 10) < 0) {
        close(servidor_fd);
        return -1;
    }

    return servidor_fd;
}

// aguarda e aceita a conexao de um novo cliente
int aceitar_cliente(int servidor_fd) {
    struct sockaddr_in cliente_end;
    socklen_t tamanho = sizeof(cliente_end);
    int cliente_fd = accept(servidor_fd, (struct sockaddr *)&cliente_end, &tamanho);
    return cliente_fd;
}

// conecta o socket do cliente no ip e porta do servidor
int conectar_no_servidor(const char *ip, int porta) {
    int cliente_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cliente_fd < 0) return -1;

    struct sockaddr_in serv_end;
    memset(&serv_end, 0, sizeof(serv_end));
    serv_end.sin_family = AF_INET;
    serv_end.sin_port = htons(porta);
    
    if (inet_pton(AF_INET, ip, &serv_end.sin_addr) <= 0) {
        close(cliente_fd);
        return -1;
    }

    if (connect(cliente_fd, (struct sockaddr *)&serv_end, sizeof(serv_end)) < 0) {
        close(cliente_fd);
        return -1;
    }

    return cliente_fd;
}

// serializa a struct e faz o envio completo pela rede
int enviar_mensagem(int socket, const Mensagem *msg) {
    uint8_t *buffer = NULL;
    uint32_t tamanho = 0;
    
    if (empacotar_mensagem(msg, &buffer, &tamanho) < 0) {
        return -1;
    }
    
    int resultado = enviar(socket, buffer, tamanho);
    free(buffer);
    return resultado;
}

// recebe os bytes e remonta a struct da mensagem
int receber_mensagem(int socket, Mensagem *msg) {
    // le tamanho rigido e previsivel do cabecalho
    uint8_t buffer_header[sizeof(Header)];
    if (ler(socket, buffer_header, sizeof(Header)) < 0) {
        return -1;
    }
    
    // verifica o tamanho do payload para alocar a memoria
    Header header_temporario;
    memcpy(&header_temporario, buffer_header, sizeof(Header));
    
    uint32_t tamanho_total = sizeof(Header) + header_temporario.tamanho_payload;
    uint8_t *buffer_completo = (uint8_t*)malloc(tamanho_total);
    if (buffer_completo == NULL) return -1;
    
    // guarda os dados iniciais lidos
    memcpy(buffer_completo, buffer_header, sizeof(Header));
    
    // usa tamanho extraido para ler o limite exato do pacote
    if (header_temporario.tamanho_payload > 0) {
        if (ler(socket, buffer_completo + sizeof(Header), header_temporario.tamanho_payload) < 0) {
            free(buffer_completo);
            return -1;
        }
    }
    
    // valida integridade e constroi a struct
    int resultado = desempacotar_mensagem(buffer_completo, tamanho_total, msg);
    free(buffer_completo);
    
    return resultado;
}
