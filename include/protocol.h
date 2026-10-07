#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* Versao de protocolo aceita no cabecalho de todas as mensagens */
#define VERSAO_PROTOCOLO 1

/* Codigos de todos os tipos de mensagem do sistema P2P */
typedef enum {
    MSG_JOIN = 1,
    MSG_LEAVE = 2,
    MSG_LOOKUP = 3,
    MSG_STORE = 4,
    MSG_DOWNLOAD_REQ = 5,
    MSG_DOWNLOAD_REP = 6,
    MSG_PREPARE = 7,
    MSG_COMMIT = 8,
    MSG_ABORT = 9,
    MSG_HEARTBEAT = 10,
    MSG_GOSSIP = 11,
    MSG_ELECTION = 12,
    MSG_OK = 13,
    MSG_COORDINATOR = 14,
    MSG_SNAPSHOT = 15,
    MSG_STATE_TRANSFER = 16,
    MSG_ACK = 17,
    MSG_ERROR = 18,
    MSG_PING = 19,
    MSG_PONG = 20,
    MSG_FIND_SUCCESSOR = 21,
    MSG_GET_PREDECESSOR = 22,
    MSG_NOTIFY = 23,
    MSG_CHORD_LOOKUP = 24,
    MSG_TOPOLOGY = 25
} TipoMensagem;

/* IP e porta de um peer provedor, anexados na resposta do LOOKUP */
typedef struct __attribute__((packed)) {
    char ip[16];
    uint16_t port;
} ProviderInfo;

/* Cabecalho fixo de toda mensagem, sem padding */
typedef struct __attribute__((packed)) {
    uint8_t versao_protocolo;
    uint16_t tipo_mensagem;
    uint8_t no_origem[32];
    uint8_t no_destino[32];
    uint8_t id_transacao[16];
    uint64_t timestamp;
    uint32_t tamanho_payload;
    uint32_t checksum;
} Header;

/* Mensagem completa: cabecalho e payload de tamanho_payload bytes */
typedef struct {
    Header header;
    uint8_t *payload;
} Mensagem;

/* Devolve o nome do tipo de mensagem para o log ("UNKNOWN" se nao conhecido).
 * A string e constante e nao deve ser liberada. */
const char *nome_do_tipo(uint16_t tipo);

/* Calcula o CRC32 de tamanho bytes de dados. */
uint32_t calcular_checksum(const uint8_t *dados, size_t tamanho);

/* Serializa cabecalho e payload num buffer alocado, com o checksum preenchido.
 * Retorna 0 ou -1. Em caso de sucesso, quem chamou libera *buffer com free. */
int empacotar_mensagem(const Mensagem *msg, uint8_t **buffer, uint32_t *tamanho_total);

/* Reconstroi a mensagem a partir do buffer e confere tamanho e checksum.
 * Retorna 0, -1 (buffer invalido ou sem memoria) ou -2 (checksum errado).
 * Em caso de sucesso, o payload e alocado; liberar com liberar_mensagem. */
int desempacotar_mensagem(const uint8_t *buffer, uint32_t tamanho_total, Mensagem *msg);

/* Libera o payload da mensagem e zera o ponteiro; aceita payload NULL. */
void liberar_mensagem(Mensagem *msg);

#endif
