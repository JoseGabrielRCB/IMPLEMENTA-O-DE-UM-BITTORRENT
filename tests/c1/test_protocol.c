#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../protocol.c"

/* Monta uma mensagem de teste com o texto como payload */
static int montar(Mensagem *msg, uint16_t tipo, const char *texto)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.versao_protocolo = 1;
    msg->header.tipo_mensagem = tipo;
    memset(msg->header.no_origem, 0xA1, sizeof(msg->header.no_origem));

    if (texto == NULL) {
        return 0;
    }

    msg->header.tamanho_payload = (uint32_t)strlen(texto);
    msg->payload = (uint8_t *)malloc(msg->header.tamanho_payload);
    if (msg->payload == NULL) {
        return -1;
    }
    memcpy(msg->payload, texto, msg->header.tamanho_payload);
    return 0;
}

/* Empacotar e desempacotar devolve a mensagem igual */
static int teste_ida_e_volta(void)
{
    Mensagem original;
    Mensagem copia;
    uint8_t *buffer = NULL;
    uint32_t tamanho = 0;
    int ok = 1;

    if (montar(&original, MSG_JOIN, "127.0.0.1:9100") != 0) {
        return 0;
    }
    if (empacotar_mensagem(&original, &buffer, &tamanho) != 0) {
        liberar_mensagem(&original);
        return 0;
    }

    memset(&copia, 0, sizeof(copia));
    if (desempacotar_mensagem(buffer, tamanho, &copia) != 0) {
        ok = 0;
    }
    if (ok && copia.header.tipo_mensagem != original.header.tipo_mensagem) {
        ok = 0;
    }
    if (ok && memcmp(copia.header.no_origem, original.header.no_origem,
                     sizeof(copia.header.no_origem)) != 0) {
        ok = 0;
    }
    if (ok && copia.header.tamanho_payload != original.header.tamanho_payload) {
        ok = 0;
    }
    if (ok && memcmp(copia.payload, original.payload, original.header.tamanho_payload) != 0) {
        ok = 0;
    }

    liberar_mensagem(&copia);
    liberar_mensagem(&original);
    free(buffer);
    return ok;
}

/* Um byte trocado no payload e pego pelo checksum */
static int teste_crc_detecta_corrupcao(void)
{
    Mensagem original;
    Mensagem copia;
    uint8_t *buffer = NULL;
    uint32_t tamanho = 0;
    int ok;

    if (montar(&original, MSG_JOIN, "127.0.0.1:9100") != 0) {
        return 0;
    }
    if (empacotar_mensagem(&original, &buffer, &tamanho) != 0) {
        liberar_mensagem(&original);
        return 0;
    }

    buffer[sizeof(Header)] = (uint8_t)(buffer[sizeof(Header)] ^ 0xFF);

    memset(&copia, 0, sizeof(copia));
    ok = (desempacotar_mensagem(buffer, tamanho, &copia) == -2);

    liberar_mensagem(&copia);
    liberar_mensagem(&original);
    free(buffer);
    return ok;
}

/* Buffer menor que o cabecalho e recusado */
static int teste_buffer_curto(void)
{
    uint8_t buffer[8];
    Mensagem msg;

    memset(buffer, 0, sizeof(buffer));
    memset(&msg, 0, sizeof(msg));

    return desempacotar_mensagem(buffer, (uint32_t)sizeof(buffer), &msg) == -1;
}

/* Cabecalho que promete mais payload do que existe e recusado */
static int teste_tamanho_inconsistente(void)
{
    uint8_t buffer[sizeof(Header) + 2];
    Header header;
    Mensagem msg;

    memset(&header, 0, sizeof(header));
    header.versao_protocolo = 1;
    header.tipo_mensagem = MSG_JOIN;
    header.tamanho_payload = 10;

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, &header, sizeof(header));
    memset(&msg, 0, sizeof(msg));

    return desempacotar_mensagem(buffer, (uint32_t)sizeof(buffer), &msg) == -1;
}

/* Mensagem sem payload volta com o ponteiro nulo */
static int teste_payload_vazio(void)
{
    Mensagem original;
    Mensagem copia;
    uint8_t *buffer = NULL;
    uint32_t tamanho = 0;
    int ok;

    montar(&original, MSG_ACK, NULL);
    if (empacotar_mensagem(&original, &buffer, &tamanho) != 0) {
        return 0;
    }

    memset(&copia, 0, sizeof(copia));
    ok = (desempacotar_mensagem(buffer, tamanho, &copia) == 0 && copia.payload == NULL);

    liberar_mensagem(&copia);
    free(buffer);
    return ok;
}

/* Imprime o resultado e conta as falhas */
static int verificar(const char *nome, int passou)
{
    printf("%-26s %s\n", nome, passou ? "ok" : "FALHOU");
    return passou ? 0 : 1;
}

int main(void)
{
    int falhas = 0;

    falhas += verificar("ida e volta", teste_ida_e_volta());
    falhas += verificar("crc detecta corrupcao", teste_crc_detecta_corrupcao());
    falhas += verificar("buffer curto", teste_buffer_curto());
    falhas += verificar("tamanho inconsistente", teste_tamanho_inconsistente());
    falhas += verificar("payload vazio", teste_payload_vazio());

    if (falhas > 0) {
        printf("%d teste(s) falharam\n", falhas);
        return 1;
    }

    printf("todos os testes do protocolo passaram\n");
    return 0;
}
