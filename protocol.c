#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// lista de todos os tipos de comandos do sistema P2P
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
    MSG_PONG = 20
} TipoMensagem;

// converte id do comando para texto legivel nos logs
const char* nome_do_tipo(uint16_t tipo) {
    switch(tipo) {
        case MSG_JOIN: return "JOIN";
        case MSG_LEAVE: return "LEAVE";
        case MSG_PING: return "PING";
        case MSG_PONG: return "PONG";
        case MSG_ACK: return "ACK";
        case MSG_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// molde do cabecalho com tamanho exato sem espacos vazios
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

// junta a etiqueta Header com a carga payload
typedef struct {
    Header header;
    uint8_t *payload;
} Mensagem;

// faz a conta do CRC32 para verificar a integridade do pacote
uint32_t calcular_checksum(const uint8_t *dados, size_t tamanho) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < tamanho; i++) {
        crc ^= dados[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    // inverte os bits no final conforme regra matematica do CRC32
    return ~crc;
}

// converte struct e payload em um vetor continuo de bytes
int empacotar_mensagem(const Mensagem *msg, uint8_t **buffer, uint32_t *tamanho_total) {
    if (msg == NULL || buffer == NULL || tamanho_total == NULL) return -1;
    
    // calcula tamanho total e aloca a memoria
    *tamanho_total = sizeof(Header) + msg->header.tamanho_payload;
    *buffer = (uint8_t*)malloc(*tamanho_total);
    
    if (*buffer == NULL) return -1;
    
    // copia o Header e zera o checksum para o calculo
    Header header_copia = msg->header;
    header_copia.checksum = 0;
    
    // copia o Header para o inicio do buffer
    memcpy(*buffer, &header_copia, sizeof(Header));
    
    // anexa o payload apos o Header
    if (msg->header.tamanho_payload > 0 && msg->payload != NULL) {
        memcpy(*buffer + sizeof(Header), msg->payload, msg->header.tamanho_payload);
    }
    
    // calcula e insere o checksum no buffer
    uint32_t check = calcular_checksum(*buffer, *tamanho_total);
    memcpy(*buffer + offsetof(Header, checksum), &check, sizeof(uint32_t));
    
    return 0;
}

// converte vetor de bytes em struct validando integridade
int desempacotar_mensagem(const uint8_t *buffer, uint32_t tamanho_total, Mensagem *msg) {
    if (buffer == NULL || msg == NULL || tamanho_total < sizeof(Header)) return -1;
    
    // copia os bytes iniciais para a struct do Header
    memcpy(&msg->header, buffer, sizeof(Header));
    
    // verifica se o tamanho total corresponde ao informado
    if (tamanho_total != sizeof(Header) + msg->header.tamanho_payload) {
        return -1;
    }
    
    // armazena o checksum recebido na mensagem
    uint32_t checksum_recebido = msg->header.checksum;
    
    // cria copia temporaria e zera o campo para recalcular o checksum
    uint8_t *buffer_temp = (uint8_t*)malloc(tamanho_total);
    if (buffer_temp == NULL) return -1;
    memcpy(buffer_temp, buffer, tamanho_total);
    
    uint32_t zero = 0;
    memcpy(buffer_temp + offsetof(Header, checksum), &zero, sizeof(uint32_t));
    
    uint32_t checksum_calculado = calcular_checksum(buffer_temp, tamanho_total);
    free(buffer_temp);
    
    // valida o calculo contra o recebido e rejeita falhas
    if (checksum_calculado != checksum_recebido) {
        return -2;
    }
    
    // aloca memoria e extrai os bytes do payload
    if (msg->header.tamanho_payload > 0) {
        msg->payload = (uint8_t*)malloc(msg->header.tamanho_payload);
        if (msg->payload == NULL) return -1;
        memcpy(msg->payload, buffer + sizeof(Header), msg->header.tamanho_payload);
    } else {
        msg->payload = NULL;
    }
    
    return 0;
}

// libera a memoria dinamica para evitar vazamentos
void liberar_mensagem(Mensagem *msg) {
    if (msg != NULL && msg->payload != NULL) {
        free(msg->payload);
        msg->payload = NULL;
    }
}