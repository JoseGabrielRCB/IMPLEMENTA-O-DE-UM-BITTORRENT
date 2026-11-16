#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <openssl/sha.h>

#include "include/node.h"
#include "include/metadata.h"

/* Le 16 bytes aleatorios de /dev/urandom */
static int node_generate_uuid(uint8_t uuid[NODE_UUID_LEN])
{
    FILE *file = fopen("/dev/urandom", "rb");
    size_t lidos;

    if (file == NULL) {
        perror("fopen /dev/urandom");
        return -1;
    }

    lidos = fread(uuid, 1, NODE_UUID_LEN, file);
    fclose(file);

    if (lidos != NODE_UUID_LEN) {
        fprintf(stderr, "Erro: nao foi possivel ler %d bytes de /dev/urandom\n", NODE_UUID_LEN);
        return -1;
    }
    return 0;
}

/* Le o UUID do arquivo ou gera um novo e salva */
int node_load_uuid(const char *path, uint8_t uuid[NODE_UUID_LEN])
{
    FILE *file;
    size_t bytes;

    if (path == NULL || uuid == NULL) {
        return -1;
    }

    file = fopen(path, "rb");
    if (file != NULL) {
        bytes = fread(uuid, 1, NODE_UUID_LEN, file);
        fclose(file);
        if (bytes != NODE_UUID_LEN) {
            fprintf(stderr, "Erro: arquivo de UUID invalido: %s\n", path);
            return -1;
        }
        return 0;
    }

    if (node_generate_uuid(uuid) != 0) {
        return -1;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Erro: nao foi possivel criar %s: %s\n", path, strerror(errno));
        return -1;
    }

    bytes = fwrite(uuid, 1, NODE_UUID_LEN, file);
    if (fclose(file) != 0 || bytes != NODE_UUID_LEN) {
        fprintf(stderr, "Erro: nao foi possivel salvar o UUID em %s\n", path);
        return -1;
    }
    return 0;
}

/* NodeID = SHA256(IP || Porta || UUID) */
void node_compute_id(const char *ip, uint16_t port,
                     const uint8_t uuid[NODE_UUID_LEN],
                     uint8_t node_id[NODE_ID_LEN])
{
    uint8_t buffer[NODE_IP_LEN + 2 + NODE_UUID_LEN];
    size_t ip_len;

    if (ip == NULL || uuid == NULL || node_id == NULL) {
        return;
    }

    ip_len = strlen(ip);
    if (ip_len > NODE_IP_LEN) {
        ip_len = NODE_IP_LEN;
    }

    memcpy(buffer, ip, ip_len);
    buffer[ip_len] = (uint8_t)(port >> 8);
    buffer[ip_len + 1] = (uint8_t)(port & 0xFF);
    memcpy(buffer + ip_len + 2, uuid, NODE_UUID_LEN);

    SHA256(buffer, ip_len + 2 + NODE_UUID_LEN, node_id);
}

/* Compara dois NodeIDs byte a byte */
int node_id_compare(const uint8_t a[NODE_ID_LEN], const uint8_t b[NODE_ID_LEN])
{
    return memcmp(a, b, NODE_ID_LEN);
}

/* Diz se o NodeID e formado so por zeros */
int node_id_is_zero(const uint8_t node_id[NODE_ID_LEN])
{
    int i;

    for (i = 0; i < NODE_ID_LEN; i++) {
        if (node_id[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Converte o NodeID para texto hexadecimal */
void node_id_to_hex(const uint8_t node_id[NODE_ID_LEN], char hex[NODE_ID_HEX_LEN])
{
    int i;

    for (i = 0; i < NODE_ID_LEN; i++) {
        snprintf(hex + (i * 2), 3, "%02x", node_id[i]);
    }
}

/* Imprime o NodeID em hexadecimal */
void node_print_id(const uint8_t node_id[NODE_ID_LEN])
{
    char hex[NODE_ID_HEX_LEN];

    node_id_to_hex(node_id, hex);
    printf("%s", hex);
}

const char *node_role_name(NodeRole role)
{
    return (role == ROLE_SUPERPEER) ? "SUPERPEER" : "PEER";
}

const char *node_state_name(NodeState state)
{
    switch (state) {
    case STATE_DISCONNECTED:
        return "DISCONNECTED";
    case STATE_CONNECTING:
        return "CONNECTING";
    case STATE_CONNECTED:
        return "CONNECTED";
    case STATE_AUTHENTICATED:
        return "AUTHENTICATED";
    }
    return "DISCONNECTED";
}

/* Valida a porta e converte para numero */
int node_parse_port(const char *texto, uint16_t *port)
{
    char *fim = NULL;
    long valor;

    errno = 0;
    valor = strtol(texto, &fim, 10);
    if (errno != 0 || fim == texto || *fim != '\0' || valor < 1 || valor > 65535) {
        return -1;
    }

    *port = (uint16_t)valor;
    return 0;
}

/* Valida o IP e copia para o destino */
int node_parse_ip(const char *texto, char *destino, size_t tamanho)
{
    struct in_addr endereco;

    if (strlen(texto) >= tamanho) {
        return -1;
    }
    if (inet_pton(AF_INET, texto, &endereco) != 1) {
        return -1;
    }

    snprintf(destino, tamanho, "%s", texto);
    return 0;
}

void node_print_usage(const char *program)
{
    fprintf(stderr, "Uso:\n");
    fprintf(stderr, "  %s superpeer <ip> <porta> <arquivo_uuid>\n", program);
    fprintf(stderr, "  %s peer <ip> <porta> <arquivo_uuid> <ip_superpeer> <porta_superpeer>\n",
            program);
}

/* Le a configuracao dos argumentos da linha de comando */
int node_parse_args(int argc, char *argv[], NodeConfig *config)
{
    if (config == NULL || argc < 2) {
        fprintf(stderr, "Erro: argumentos insuficientes\n");
        return -1;
    }

    memset(config, 0, sizeof(*config));

    if (strcmp(argv[1], "superpeer") == 0) {
        config->role = ROLE_SUPERPEER;
        if (argc != 5) {
            fprintf(stderr, "Erro: o superpeer precisa de 3 argumentos\n");
            return -1;
        }
    } else if (strcmp(argv[1], "peer") == 0) {
        config->role = ROLE_PEER;
        if (argc != 7) {
            fprintf(stderr, "Erro: o peer precisa de 5 argumentos\n");
            return -1;
        }
    } else {
        fprintf(stderr, "Erro: papel invalido: %s\n", argv[1]);
        return -1;
    }

    if (node_parse_ip(argv[2], config->ip, sizeof(config->ip)) != 0) {
        fprintf(stderr, "Erro: IP invalido: %s\n", argv[2]);
        return -1;
    }
    if (node_parse_port(argv[3], &config->port) != 0) {
        fprintf(stderr, "Erro: porta invalida: %s\n", argv[3]);
        return -1;
    }
    if (strlen(argv[4]) == 0 || strlen(argv[4]) >= sizeof(config->uuid_path)) {
        fprintf(stderr, "Erro: caminho do arquivo de UUID invalido\n");
        return -1;
    }
    snprintf(config->uuid_path, sizeof(config->uuid_path), "%s", argv[4]);

    if (config->role == ROLE_PEER) {
        if (node_parse_ip(argv[5], config->superpeer_ip, sizeof(config->superpeer_ip)) != 0) {
            fprintf(stderr, "Erro: IP do superpeer invalido: %s\n", argv[5]);
            return -1;
        }
        if (node_parse_port(argv[6], &config->superpeer_port) != 0) {
            fprintf(stderr, "Erro: porta do superpeer invalida: %s\n", argv[6]);
            return -1;
        }
    }

    return 0;
}

/* Monta a identidade do processo a partir da configuracao */
int node_init(Node *node, const NodeConfig *config)
{
    uint8_t uuid[NODE_UUID_LEN];

    if (node == NULL || config == NULL) {
        return -1;
    }

    if (node_load_uuid(config->uuid_path, uuid) != 0) {
        return -1;
    }

    memset(node, 0, sizeof(*node));
    snprintf(node->ip, sizeof(node->ip), "%s", config->ip);
    node->port = config->port;
    node->role = config->role;
    node->state = STATE_DISCONNECTED;
    node_compute_id(node->ip, node->port, uuid, node->node_id);

    return 0;
}

/* Imprime a identidade do processo */
void node_print(const Node *node)
{
    if (node == NULL) {
        return;
    }

    printf("Papel:  %s\n", node_role_name(node->role));
    printf("IP:     %s\n", node->ip);
    printf("Porta:  %u\n", node->port);
    printf("Estado: %s\n", node_state_name(node->state));
    printf("NodeID: ");
    node_print_id(node->node_id);
    printf("\n");
}

/* ==========================================================================
 * CAMADA DE METADADOS E TABELA HASH (integrado de metadata.c)
 * ========================================================================== */

/* Entrada da tabela principal, indexada pelo ObjectID */
typedef struct {
    int used;
    FileMetadata meta;
    uint8_t providers[MAX_PROVIDERS][32];
    int provider_count;
} MetadataSlot;

/* Entrada do indice por nome, leva o nome ao ObjectID */
typedef struct {
    int used;
    char filename[METADATA_NAME_LEN];
    uint8_t object_id[METADATA_ID_LEN];
} MetadataNameSlot;

/* Tabelas de metadados e o mutex que protege as duas */
static MetadataSlot metadata_slots[MAX_FILES];
static MetadataNameSlot metadata_names[MAX_FILES];
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Zera o metadado e aloca os hashes dos chunks */
int metadata_create(FileMetadata *meta, uint32_t chunk_count)
{
    uint32_t i;

    if (meta == NULL) {
        return METADATA_ERR_INVALID;
    }

    memset(meta, 0, sizeof(*meta));
    meta->version = 1;
    if (chunk_count == 0) {
        return METADATA_OK;
    }

    meta->chunk_hashes = (uint8_t **)calloc(chunk_count, sizeof(uint8_t *));
    if (meta->chunk_hashes == NULL) {
        return METADATA_ERR_MEMORY;
    }
    meta->chunk_count = chunk_count;

    for (i = 0; i < chunk_count; i++) {
        meta->chunk_hashes[i] = (uint8_t *)calloc(1, METADATA_HASH_LEN);
        if (meta->chunk_hashes[i] == NULL) {
            metadata_free(meta);
            return METADATA_ERR_MEMORY;
        }
    }
    return METADATA_OK;
}

/* Libera os hashes dos chunks e zera o metadado */
void metadata_free(FileMetadata *meta)
{
    uint32_t i;

    if (meta == NULL) {
        return;
    }

    if (meta->chunk_hashes != NULL) {
        for (i = 0; i < meta->chunk_count; i++) {
            free(meta->chunk_hashes[i]);
        }
        free(meta->chunk_hashes);
    }
    memset(meta, 0, sizeof(*meta));
}

/* Copia o metadado inteiro, inclusive os hashes */
int metadata_copy(FileMetadata *dest, const FileMetadata *src)
{
    uint32_t i;
    int resultado;

    if (dest == NULL || src == NULL) {
        return METADATA_ERR_INVALID;
    }
    if (src->chunk_count > 0 && src->chunk_hashes == NULL) {
        return METADATA_ERR_INVALID;
    }

    resultado = metadata_create(dest, src->chunk_count);
    if (resultado != METADATA_OK) {
        return resultado;
    }

    memcpy(dest->object_id, src->object_id, METADATA_ID_LEN);
    memcpy(dest->filename, src->filename, METADATA_NAME_LEN);
    dest->filename[METADATA_NAME_LEN - 1] = '\0';
    dest->size = src->size;
    dest->version = src->version;
    memcpy(dest->owner, src->owner, METADATA_ID_LEN);
    dest->compression = src->compression;
    dest->upload_date = src->upload_date;
    dest->last_access = src->last_access;
    dest->download_counter = src->download_counter;
    dest->status = src->status;
    dest->replica_count = src->replica_count > METADATA_REPLICA_MAX ? METADATA_REPLICA_MAX
                                                                    : src->replica_count;
    memcpy(dest->replica_peers, src->replica_peers, sizeof(dest->replica_peers));

    for (i = 0; i < src->chunk_count; i++) {
        if (src->chunk_hashes[i] == NULL) {
            metadata_free(dest);
            return METADATA_ERR_INVALID;
        }
        memcpy(dest->chunk_hashes[i], src->chunk_hashes[i], METADATA_HASH_LEN);
    }
    return METADATA_OK;
}

/* Imprime bytes em hexadecimal */
static void metadata_print_hex(const uint8_t *bytes, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        printf("%02x", bytes[i]);
    }
}

/* Imprime o metadado no formato do checkpoint */
void metadata_print(const FileMetadata *meta)
{
    uint32_t i;

    if (meta == NULL) {
        return;
    }

    printf("File: %s\n", meta->filename);
    printf("Size: %llu bytes\n\n", (unsigned long long)meta->size);
    printf("ObjectID:\n");
    metadata_print_hex(meta->object_id, METADATA_ID_LEN);
    printf("\n\n");
    printf("Chunks: %u\n\n", meta->chunk_count);

    for (i = 0; i < meta->chunk_count; i++) {
        printf("Chunk %u: ", i);
        if (meta->chunk_hashes != NULL && meta->chunk_hashes[i] != NULL) {
            metadata_print_hex(meta->chunk_hashes[i], METADATA_HASH_LEN);
        }
        printf("\n");
    }
}

/* Nome do status do metadado */
const char *metadata_status_name(uint8_t status)
{
    switch (status) {
    case METADATA_STATUS_ACTIVE:
        return "ACTIVE";
    case METADATA_STATUS_REPLICATING:
        return "REPLICATING";
    case METADATA_STATUS_REMOVED:
        return "REMOVED";
    default:
        return "?";
    }
}

/* Data em texto local, ou "-" se zero */
static void metadata_print_time(const char *rotulo, uint64_t segundos)
{
    char texto[32] = "-";
    time_t t = (time_t)segundos;
    struct tm quando;

    if (segundos != 0 && localtime_r(&t, &quando) != NULL) {
        strftime(texto, sizeof(texto), "%Y-%m-%d %H:%M:%S", &quando);
    }
    printf("%s: %s\n", rotulo, texto);
}

/* Imprime os demais campos da Secao 8.3 */
void metadata_print_details(const FileMetadata *meta)
{
    uint8_t i;

    if (meta == NULL) {
        return;
    }
    printf("Version: %llu\n", (unsigned long long)meta->version);
    printf("Status: %s\n", metadata_status_name(meta->status));
    printf("Compression: %s\n", meta->compression == METADATA_COMPRESSION_LZ4 ? "LZ4" : "none");
    printf("OwnerPeer: ");
    metadata_print_hex(meta->owner, 8);
    printf("...\n");
    metadata_print_time("UploadDate", meta->upload_date);
    metadata_print_time("LastAccess", meta->last_access);
    printf("DownloadCounter: %llu\n", (unsigned long long)meta->download_counter);
    printf("ReplicaPeers: %u", meta->replica_count);
    for (i = 0; i < meta->replica_count && i < METADATA_REPLICA_MAX; i++) {
        printf("%s", i == 0 ? " (" : ", ");
        metadata_print_hex(meta->replica_peers[i], 6);
    }
    printf("%s\n", meta->replica_count > 0 ? ")" : "");
}

/* Calcula o ObjectID a partir do arquivo inteiro */
int metadata_compute_object_id(const char *path, uint8_t object_id[METADATA_ID_LEN],
                               uint64_t *size)
{
    struct stat info;
    FILE *file;
    uint8_t *buffer;
    size_t tamanho;
    size_t lidos;

    if (path == NULL || object_id == NULL || size == NULL) {
        return METADATA_ERR_INVALID;
    }
    if (stat(path, &info) != 0) {
        perror("stat");
        return METADATA_ERR_FILE;
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= 0) {
        fprintf(stderr, "Erro: %s nao e um arquivo valido ou esta vazio\n", path);
        return METADATA_ERR_FILE;
    }
    if ((uint64_t)info.st_size > (uint64_t)SIZE_MAX) {
        return METADATA_ERR_FILE;
    }
    tamanho = (size_t)info.st_size;

    buffer = (uint8_t *)malloc(tamanho);
    if (buffer == NULL) {
        return METADATA_ERR_MEMORY;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        perror("fopen");
        free(buffer);
        return METADATA_ERR_FILE;
    }
    lidos = fread(buffer, 1, tamanho, file);
    fclose(file);

    if (lidos != tamanho) {
        fprintf(stderr, "Erro: leitura incompleta de %s\n", path);
        free(buffer);
        return METADATA_ERR_FILE;
    }

    SHA256(buffer, tamanho, object_id);
    free(buffer);
    *size = (uint64_t)tamanho;
    return METADATA_OK;
}

/* Confere se chunk_count e igual a ceil(size / CHUNK_SIZE) */
int metadata_check_chunks(const FileMetadata *meta)
{
    uint64_t esperado;

    if (meta == NULL || meta->size == 0) {
        return METADATA_ERR_INVALID;
    }

    esperado = (meta->size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if ((uint64_t)meta->chunk_count != esperado) {
        return METADATA_ERR_INVALID;
    }
    return METADATA_OK;
}

/* Converte o metadado em bytes para o payload */
int metadata_serialize(const FileMetadata *meta, uint8_t **buffer, uint32_t *size)
{
    uint8_t *saida;
    size_t nome_len;
    uint16_t nome_len16;
    uint64_t total;
    size_t pos = 0;
    uint32_t i;

    if (meta == NULL || buffer == NULL || size == NULL) {
        return METADATA_ERR_INVALID;
    }
    if (meta->chunk_count > 0 && meta->chunk_hashes == NULL) {
        return METADATA_ERR_INVALID;
    }

    nome_len = strnlen(meta->filename, METADATA_NAME_LEN);
    if (nome_len == 0 || nome_len > METADATA_NAME_MAX) {
        return METADATA_ERR_INVALID;
    }
    nome_len16 = (uint16_t)nome_len;

    if (meta->replica_count > METADATA_REPLICA_MAX) {
        return METADATA_ERR_INVALID;
    }
    total = METADATA_ID_LEN + sizeof(uint16_t) + nome_len + sizeof(uint64_t)
            + sizeof(uint32_t) + (uint64_t)meta->chunk_count * METADATA_HASH_LEN
            + sizeof(uint64_t) + METADATA_ID_LEN
            + METADATA_EXTRA_LEN + (uint64_t)meta->replica_count * METADATA_ID_LEN;
    if (total > UINT32_MAX) {
        return METADATA_ERR_INVALID;
    }

    saida = (uint8_t *)malloc((size_t)total);
    if (saida == NULL) {
        return METADATA_ERR_MEMORY;
    }

    memcpy(saida + pos, meta->object_id, METADATA_ID_LEN);
    pos += METADATA_ID_LEN;
    memcpy(saida + pos, &nome_len16, sizeof(uint16_t));
    pos += sizeof(uint16_t);
    memcpy(saida + pos, meta->filename, nome_len);
    pos += nome_len;
    memcpy(saida + pos, &meta->size, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    memcpy(saida + pos, &meta->chunk_count, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    for (i = 0; i < meta->chunk_count; i++) {
        if (meta->chunk_hashes[i] == NULL) {
            free(saida);
            return METADATA_ERR_INVALID;
        }
        memcpy(saida + pos, meta->chunk_hashes[i], METADATA_HASH_LEN);
        pos += METADATA_HASH_LEN;
    }

    memcpy(saida + pos, &meta->version, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    memcpy(saida + pos, meta->owner, METADATA_ID_LEN);
    pos += METADATA_ID_LEN;
    saida[pos++] = meta->compression;
    memcpy(saida + pos, &meta->upload_date, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    memcpy(saida + pos, &meta->last_access, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    memcpy(saida + pos, &meta->download_counter, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    saida[pos++] = meta->status;
    saida[pos++] = meta->replica_count;
    for (i = 0; i < meta->replica_count; i++) {
        memcpy(saida + pos, meta->replica_peers[i], METADATA_ID_LEN);
        pos += METADATA_ID_LEN;
    }

    *buffer = saida;
    *size = (uint32_t)total;
    return METADATA_OK;
}

/* Copia n bytes do buffer se couberem no que resta */
static int metadata_read(const uint8_t *buffer, uint32_t size, size_t *pos,
                         void *dest, size_t n)
{
    if (*pos > size || n > size - *pos) {
        return -1;
    }
    memcpy(dest, buffer + *pos, n);
    *pos += n;
    return 0;
}

/* Reconstroi o metadado a partir do payload */
int metadata_deserialize(const uint8_t *buffer, uint32_t size, FileMetadata *meta)
{
    uint8_t object_id[METADATA_ID_LEN];
    char filename[METADATA_NAME_LEN];
    uint16_t nome_len;
    uint64_t tamanho_arquivo;
    uint32_t chunk_count;
    uint64_t restante;
    size_t pos = 0;
    uint32_t i;

    if (buffer == NULL || meta == NULL) {
        return METADATA_ERR_INVALID;
    }

    if (metadata_read(buffer, size, &pos, object_id, METADATA_ID_LEN) != 0 ||
        metadata_read(buffer, size, &pos, &nome_len, sizeof(uint16_t)) != 0) {
        return METADATA_ERR_INVALID;
    }
    if (nome_len == 0 || nome_len > METADATA_NAME_MAX) {
        return METADATA_ERR_INVALID;
    }

    memset(filename, 0, sizeof(filename));
    if (metadata_read(buffer, size, &pos, filename, nome_len) != 0) {
        return METADATA_ERR_INVALID;
    }
    if (memchr(filename, '\0', nome_len) != NULL) {
        return METADATA_ERR_INVALID;
    }

    if (metadata_read(buffer, size, &pos, &tamanho_arquivo, sizeof(uint64_t)) != 0 ||
        metadata_read(buffer, size, &pos, &chunk_count, sizeof(uint32_t)) != 0) {
        return METADATA_ERR_INVALID;
    }

    /* o que sobra: hashes, versao, dono, campos fixos e ao menos o replica_count */
    restante = (uint64_t)size - pos;
    if (restante < (uint64_t)chunk_count * METADATA_HASH_LEN
                   + sizeof(uint64_t) + METADATA_ID_LEN + METADATA_EXTRA_LEN) {
        return METADATA_ERR_INVALID;
    }

    if (metadata_create(meta, chunk_count) != METADATA_OK) {
        return METADATA_ERR_MEMORY;
    }

    memcpy(meta->object_id, object_id, METADATA_ID_LEN);
    memcpy(meta->filename, filename, METADATA_NAME_LEN);
    meta->size = tamanho_arquivo;

    for (i = 0; i < chunk_count; i++) {
        if (metadata_read(buffer, size, &pos, meta->chunk_hashes[i], METADATA_HASH_LEN) != 0) {
            metadata_free(meta);
            return METADATA_ERR_INVALID;
        }
    }
    if (metadata_read(buffer, size, &pos, &meta->version, sizeof(uint64_t)) != 0 ||
        metadata_read(buffer, size, &pos, meta->owner, METADATA_ID_LEN) != 0 ||
        metadata_read(buffer, size, &pos, &meta->compression, 1) != 0 ||
        metadata_read(buffer, size, &pos, &meta->upload_date, sizeof(uint64_t)) != 0 ||
        metadata_read(buffer, size, &pos, &meta->last_access, sizeof(uint64_t)) != 0 ||
        metadata_read(buffer, size, &pos, &meta->download_counter, sizeof(uint64_t)) != 0 ||
        metadata_read(buffer, size, &pos, &meta->status, 1) != 0 ||
        metadata_read(buffer, size, &pos, &meta->replica_count, 1) != 0 ||
        meta->replica_count > METADATA_REPLICA_MAX ||
        (uint64_t)size - pos != (uint64_t)meta->replica_count * METADATA_ID_LEN) {
        metadata_free(meta);
        return METADATA_ERR_INVALID;
    }
    for (i = 0; i < meta->replica_count; i++) {
        memcpy(meta->replica_peers[i], buffer + pos, METADATA_ID_LEN);
        pos += METADATA_ID_LEN;
    }
    return METADATA_OK;
}

/* Indice inicial: primeiros 4 bytes do ObjectID modulo MAX_FILES */
static uint32_t metadata_hash_id(const uint8_t object_id[METADATA_ID_LEN])
{
    uint32_t valor;

    memcpy(&valor, object_id, sizeof(uint32_t));
    return valor % MAX_FILES;
}

/* Indice inicial do nome pela funcao djb2 */
static uint32_t metadata_hash_name(const char *filename)
{
    uint32_t hash = 5381;
    const unsigned char *c = (const unsigned char *)filename;

    while (*c != '\0') {
        hash = hash * 33 + *c;
        c++;
    }
    return hash % MAX_FILES;
}

/* Procura o ObjectID na tabela, usar com o mutex travado */
static int metadata_find_id_index(const uint8_t object_id[METADATA_ID_LEN])
{
    uint32_t inicio = metadata_hash_id(object_id);
    uint32_t i;

    for (i = 0; i < MAX_FILES; i++) {
        uint32_t indice = (inicio + i) % MAX_FILES;

        if (!metadata_slots[indice].used) {
            return -1;
        }
        if (memcmp(metadata_slots[indice].meta.object_id, object_id, METADATA_ID_LEN) == 0) {
            return (int)indice;
        }
    }
    return -1;
}

/* Procura o nome (ou basename apos '/') no indice, usar com o mutex travado */
static int metadata_find_name_index(const char *filename)
{
    uint32_t inicio = metadata_hash_name(filename);
    uint32_t i;

    for (i = 0; i < MAX_FILES; i++) {
        uint32_t indice = (inicio + i) % MAX_FILES;

        if (!metadata_names[indice].used) {
            break;
        }
        if (strcmp(metadata_names[indice].filename, filename) == 0) {
            return (int)indice;
        }
    }
    for (i = 0; i < MAX_FILES; i++) {
        if (!metadata_names[i].used) {
            continue;
        }
        const char *slash = strrchr(metadata_names[i].filename, '/');
        if (slash != NULL && strcmp(slash + 1, filename) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Acha a primeira posicao livre na tabela principal */
static int metadata_free_id_index(const uint8_t object_id[METADATA_ID_LEN])
{
    uint32_t inicio = metadata_hash_id(object_id);
    uint32_t i;

    for (i = 0; i < MAX_FILES; i++) {
        uint32_t indice = (inicio + i) % MAX_FILES;

        if (!metadata_slots[indice].used) {
            return (int)indice;
        }
    }
    return -1;
}

/* Acha a primeira posicao livre no indice por nome */
static int metadata_free_name_index(const char *filename)
{
    uint32_t inicio = metadata_hash_name(filename);
    uint32_t i;

    for (i = 0; i < MAX_FILES; i++) {
        uint32_t indice = (inicio + i) % MAX_FILES;

        if (!metadata_names[indice].used) {
            return (int)indice;
        }
    }
    return -1;
}

/* Insere uma copia do metadado nas duas tabelas */
int metadata_table_insert(const FileMetadata *meta)
{
    int resultado = METADATA_OK;
    int nome;
    int livre_id;
    int livre_nome;

    if (meta == NULL || strnlen(meta->filename, METADATA_NAME_LEN) == 0 ||
        strnlen(meta->filename, METADATA_NAME_LEN) > METADATA_NAME_MAX) {
        return METADATA_ERR_INVALID;
    }
    if (pthread_mutex_lock(&metadata_mutex) != 0) {
        return METADATA_ERR_INVALID;
    }

    livre_id = metadata_find_id_index(meta->object_id);
    if (livre_id >= 0) {
        FileMetadata *atual = &metadata_slots[livre_id].meta;
        if (atual->status == meta->status) {
            printf("STORE: ObjectID ja registrado, tabela inalterada\n");
        } else {
            /* remocao logica (Secao 8.4) ou reativacao: muda o status e faz Version++ */
            atual->status = meta->status;
            atual->version++;
            atual->replica_count = meta->replica_count;
            memcpy(atual->replica_peers, meta->replica_peers, sizeof(atual->replica_peers));
            printf("STORE: arquivo %s %s (version=%llu)\n", atual->filename,
                   atual->status == METADATA_STATUS_REMOVED ? "marcado como REMOVED (remocao logica)"
                                                            : "reativado",
                   (unsigned long long)atual->version);
        }
        pthread_mutex_unlock(&metadata_mutex);
        return METADATA_OK;
    }

    nome = metadata_find_name_index(meta->filename);
    if (nome >= 0) {
        int slot_antigo = metadata_find_id_index(metadata_names[nome].object_id);
        if (slot_antigo >= 0) {
            uint64_t nova_versao = metadata_slots[slot_antigo].meta.version + 1;
            metadata_free(&metadata_slots[slot_antigo].meta);
            resultado = metadata_copy(&metadata_slots[slot_antigo].meta, meta);
            if (resultado == METADATA_OK) {
                metadata_slots[slot_antigo].meta.version = nova_versao;
                metadata_slots[slot_antigo].provider_count = 0;
                memcpy(metadata_names[nome].object_id, meta->object_id, METADATA_ID_LEN);
                printf("STORE: arquivo %s atualizado para version=%llu\n",
                       meta->filename, (unsigned long long)nova_versao);
                metadata_print(&metadata_slots[slot_antigo].meta);
            }
            pthread_mutex_unlock(&metadata_mutex);
            return resultado;
        }
    }

    livre_id = metadata_free_id_index(meta->object_id);
    livre_nome = metadata_free_name_index(meta->filename);

    if (livre_id < 0 || livre_nome < 0) {
        resultado = METADATA_ERR_FULL;
    } else {
        resultado = metadata_copy(&metadata_slots[livre_id].meta, meta);
    }

    if (resultado == METADATA_OK) {
        metadata_slots[livre_id].used = 1;
        metadata_slots[livre_id].provider_count = 0;
        metadata_names[livre_nome].used = 1;
        snprintf(metadata_names[livre_nome].filename, METADATA_NAME_LEN, "%s",
                 metadata_slots[livre_id].meta.filename);
        memcpy(metadata_names[livre_nome].object_id, meta->object_id, METADATA_ID_LEN);
        metadata_print(&metadata_slots[livre_id].meta);
    }

    pthread_mutex_unlock(&metadata_mutex);
    return resultado;
}

/* Busca pelo ObjectID e devolve uma copia em out */
int metadata_table_find_by_id(const uint8_t object_id[METADATA_ID_LEN], FileMetadata *out)
{
    int indice;
    int resultado;

    if (object_id == NULL || out == NULL) {
        return METADATA_ERR_INVALID;
    }
    if (pthread_mutex_lock(&metadata_mutex) != 0) {
        return METADATA_ERR_INVALID;
    }

    indice = metadata_find_id_index(object_id);
    if (indice < 0 || metadata_slots[indice].meta.status == METADATA_STATUS_REMOVED) {
        resultado = METADATA_ERR_NOT_FOUND;
    } else {
        resultado = metadata_copy(out, &metadata_slots[indice].meta);
    }

    pthread_mutex_unlock(&metadata_mutex);
    return resultado;
}

/* Busca pelo nome e devolve uma copia em out */
int metadata_table_find_by_name(const char *filename, FileMetadata *out)
{
    int nome;
    int indice = -1;
    int resultado;

    if (filename == NULL || out == NULL) {
        return METADATA_ERR_INVALID;
    }
    if (pthread_mutex_lock(&metadata_mutex) != 0) {
        return METADATA_ERR_INVALID;
    }

    nome = metadata_find_name_index(filename);
    if (nome >= 0) {
        indice = metadata_find_id_index(metadata_names[nome].object_id);
    }

    if (indice < 0 || metadata_slots[indice].meta.status == METADATA_STATUS_REMOVED) {
        resultado = METADATA_ERR_NOT_FOUND;
    } else {
        resultado = metadata_copy(out, &metadata_slots[indice].meta);
    }

    pthread_mutex_unlock(&metadata_mutex);
    return resultado;
}

/* Estatistica local de acesso: LastAccess = agora e DownloadCounter++ */
int metadata_table_touch(const uint8_t object_id[METADATA_ID_LEN], uint64_t *last_access,
                         uint64_t *downloads)
{
    int indice;

    if (object_id == NULL || pthread_mutex_lock(&metadata_mutex) != 0) {
        return METADATA_ERR_INVALID;
    }
    indice = metadata_find_id_index(object_id);
    if (indice < 0) {
        pthread_mutex_unlock(&metadata_mutex);
        return METADATA_ERR_NOT_FOUND;
    }
    metadata_slots[indice].meta.last_access = (uint64_t)time(NULL);
    metadata_slots[indice].meta.download_counter++;
    if (last_access != NULL) {
        *last_access = metadata_slots[indice].meta.last_access;
    }
    if (downloads != NULL) {
        *downloads = metadata_slots[indice].meta.download_counter;
    }
    pthread_mutex_unlock(&metadata_mutex);
    return METADATA_OK;
}

/* Imprime todos os metadados registrados */
void metadata_table_print(void)
{
    int i;
    int total = 0;

    if (pthread_mutex_lock(&metadata_mutex) != 0) {
        return;
    }

    for (i = 0; i < MAX_FILES; i++) {
        if (metadata_slots[i].used) {
            total++;
        }
    }
    printf("Tabela de metadados (%d/%d)\n", total, MAX_FILES);
    for (i = 0; i < MAX_FILES; i++) {
        if (metadata_slots[i].used) {
            printf("[%d]\n", i);
            metadata_print(&metadata_slots[i].meta);
        }
    }

    pthread_mutex_unlock(&metadata_mutex);
}

/* Adiciona um NodeID (provedor) para o arquivo associado ao object_id */
int metadata_add_provider(const uint8_t object_id[METADATA_ID_LEN], const uint8_t node_id[32])
{
    int indice;
    int i;
    
    if (object_id == NULL || node_id == NULL) {
        return METADATA_ERR_INVALID;
    }
    
    if (pthread_mutex_lock(&metadata_mutex) != 0) {
        return METADATA_ERR_INVALID;
    }
    
    indice = metadata_find_id_index(object_id);
    if (indice < 0) {
        pthread_mutex_unlock(&metadata_mutex);
        return METADATA_ERR_NOT_FOUND;
    }
    
    /* Verifica se ja existe na lista */
    for (i = 0; i < metadata_slots[indice].provider_count; i++) {
        if (memcmp(metadata_slots[indice].providers[i], node_id, 32) == 0) {
            pthread_mutex_unlock(&metadata_mutex);
            return METADATA_OK;
        }
    }
    
    /* Adiciona se houver espaco */
    if (metadata_slots[indice].provider_count < MAX_PROVIDERS) {
        memcpy(metadata_slots[indice].providers[metadata_slots[indice].provider_count], node_id, 32);
        metadata_slots[indice].provider_count++;
    } else {
        /* Lista cheia */
        pthread_mutex_unlock(&metadata_mutex);
        return METADATA_ERR_FULL;
    }
    
    pthread_mutex_unlock(&metadata_mutex);
    return METADATA_OK;
}

/* Recupera a lista de provedores de um arquivo */
int metadata_get_providers(const uint8_t object_id[METADATA_ID_LEN], uint8_t out_providers[][32], int *count)
{
    int indice;
    int i;
    
    if (object_id == NULL || out_providers == NULL || count == NULL) {
        return METADATA_ERR_INVALID;
    }
    
    if (pthread_mutex_lock(&metadata_mutex) != 0) {
        return METADATA_ERR_INVALID;
    }
    
    indice = metadata_find_id_index(object_id);
    if (indice < 0) {
        pthread_mutex_unlock(&metadata_mutex);
        return METADATA_ERR_NOT_FOUND;
    }
    
    *count = metadata_slots[indice].provider_count;
    for (i = 0; i < metadata_slots[indice].provider_count; i++) {
        memcpy(out_providers[i], metadata_slots[indice].providers[i], 32);
    }
    
    pthread_mutex_unlock(&metadata_mutex);
    return METADATA_OK;
}
