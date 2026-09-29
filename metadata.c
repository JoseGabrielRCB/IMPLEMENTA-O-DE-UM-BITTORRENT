#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <openssl/sha.h>

#define METADATA_ID_LEN 32
#define METADATA_HASH_LEN 32
#define METADATA_NAME_LEN 256
#define METADATA_NAME_MAX 255

#define MAX_FILES 256
#define CHUNK_SIZE (4 * 1024 * 1024)

/* Retornos das operacoes de metadados */
#define METADATA_OK 0
#define METADATA_ERR_INVALID (-1)
#define METADATA_ERR_MEMORY (-2)
#define METADATA_ERR_FULL (-3)
#define METADATA_ERR_NAME_TAKEN (-4)
#define METADATA_ERR_NOT_FOUND (-5)
#define METADATA_ERR_FILE (-6)

/* Metadado de um arquivo registrado */
typedef struct {
    uint8_t   object_id[32];
    char      filename[256];
    uint64_t  size;
    uint32_t  chunk_count;
    uint8_t **chunk_hashes;
    uint64_t  version;
    uint8_t   owner[32];
} FileMetadata;

/* Entrada da tabela principal, indexada pelo ObjectID */
typedef struct {
    int used;
    FileMetadata meta;
} MetadataSlot;

/* Entrada do indice por nome, leva o nome ao ObjectID */
typedef struct {
    int used;
    char filename[METADATA_NAME_LEN];
    uint8_t object_id[METADATA_ID_LEN];
} MetadataNameSlot;

int metadata_create(FileMetadata *meta, uint32_t chunk_count);
void metadata_free(FileMetadata *meta);
int metadata_copy(FileMetadata *dest, const FileMetadata *src);
void metadata_print(const FileMetadata *meta);

int metadata_compute_object_id(const char *path, uint8_t object_id[METADATA_ID_LEN],
                               uint64_t *size);
int metadata_check_chunks(const FileMetadata *meta);

int metadata_serialize(const FileMetadata *meta, uint8_t **buffer, uint32_t *size);
int metadata_deserialize(const uint8_t *buffer, uint32_t size, FileMetadata *meta);

int metadata_table_insert(const FileMetadata *meta);
int metadata_table_find_by_id(const uint8_t object_id[METADATA_ID_LEN], FileMetadata *out);
int metadata_table_find_by_name(const char *filename, FileMetadata *out);
void metadata_table_print(void);

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

    total = METADATA_ID_LEN + sizeof(uint16_t) + nome_len + sizeof(uint64_t)
            + sizeof(uint32_t) + (uint64_t)meta->chunk_count * METADATA_HASH_LEN
            + sizeof(uint64_t) + METADATA_ID_LEN;
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

    /* o que sobra tem de ser exatamente os hashes, a versao e o dono */
    restante = (uint64_t)size - pos;
    if (restante != (uint64_t)chunk_count * METADATA_HASH_LEN
                    + sizeof(uint64_t) + METADATA_ID_LEN) {
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
        metadata_read(buffer, size, &pos, meta->owner, METADATA_ID_LEN) != 0) {
        metadata_free(meta);
        return METADATA_ERR_INVALID;
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

/* Procura o nome no indice, usar com o mutex travado */
static int metadata_find_name_index(const char *filename)
{
    uint32_t inicio = metadata_hash_name(filename);
    uint32_t i;

    for (i = 0; i < MAX_FILES; i++) {
        uint32_t indice = (inicio + i) % MAX_FILES;

        if (!metadata_names[indice].used) {
            return -1;
        }
        if (strcmp(metadata_names[indice].filename, filename) == 0) {
            return (int)indice;
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

    if (metadata_find_id_index(meta->object_id) >= 0) {
        printf("STORE: ObjectID ja registrado, tabela inalterada\n");
        pthread_mutex_unlock(&metadata_mutex);
        return METADATA_OK;
    }

    nome = metadata_find_name_index(meta->filename);
    livre_id = metadata_free_id_index(meta->object_id);
    livre_nome = metadata_free_name_index(meta->filename);

    if (nome >= 0) {
        resultado = METADATA_ERR_NAME_TAKEN;
    } else if (livre_id < 0 || livre_nome < 0) {
        resultado = METADATA_ERR_FULL;
    } else {
        resultado = metadata_copy(&metadata_slots[livre_id].meta, meta);
    }

    if (resultado == METADATA_OK) {
        metadata_slots[livre_id].used = 1;
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
    if (indice < 0) {
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

    if (indice < 0) {
        resultado = METADATA_ERR_NOT_FOUND;
    } else {
        resultado = metadata_copy(out, &metadata_slots[indice].meta);
    }

    pthread_mutex_unlock(&metadata_mutex);
    return resultado;
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
