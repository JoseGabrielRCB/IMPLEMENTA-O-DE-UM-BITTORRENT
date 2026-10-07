#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>
#include <stddef.h>

/* Tamanhos dos campos do metadado */
#define METADATA_ID_LEN 32
#define METADATA_HASH_LEN 32
#define METADATA_NAME_LEN 256
#define METADATA_NAME_MAX 255

/* Capacidade da tabela, tamanho do chunk (4 MiB) e provedores por arquivo */
#define MAX_FILES 256
#define CHUNK_SIZE (4 * 1024 * 1024)
#define MAX_PROVIDERS 10

/* Retornos das operacoes de metadados */
#define METADATA_OK 0
#define METADATA_ERR_INVALID (-1)
#define METADATA_ERR_MEMORY (-2)
#define METADATA_ERR_FULL (-3)
#define METADATA_ERR_NAME_TAKEN (-4)
#define METADATA_ERR_NOT_FOUND (-5)
#define METADATA_ERR_FILE (-6)

/* Metadado de um arquivo registrado; chunk_hashes tem chunk_count hashes */
typedef struct {
    uint8_t   object_id[32];
    char      filename[256];
    uint64_t  size;
    uint32_t  chunk_count;
    uint8_t **chunk_hashes;
    uint64_t  version;
    uint8_t   owner[32];
} FileMetadata;

/* Zera o metadado, poe version = 1 e aloca chunk_count hashes zerados.
 * Retorna METADATA_OK, METADATA_ERR_INVALID ou METADATA_ERR_MEMORY.
 * Liberar com metadata_free. */
int metadata_create(FileMetadata *meta, uint32_t chunk_count);

/* Libera os hashes e zera o metadado; aceita meta NULL. */
void metadata_free(FileMetadata *meta);

/* Copia src para dest, inclusive os hashes (dest e sobrescrito sem liberar).
 * Retorna METADATA_OK, METADATA_ERR_INVALID ou METADATA_ERR_MEMORY.
 * Liberar dest com metadata_free. */
int metadata_copy(FileMetadata *dest, const FileMetadata *src);

/* Imprime nome, tamanho, ObjectID e o hash de cada chunk. */
void metadata_print(const FileMetadata *meta);

/* Calcula o ObjectID (SHA-256 do arquivo inteiro) e o tamanho do arquivo.
 * Retorna METADATA_OK, METADATA_ERR_INVALID, METADATA_ERR_FILE (arquivo
 * ausente, vazio ou ilegivel) ou METADATA_ERR_MEMORY. */
int metadata_compute_object_id(const char *path, uint8_t object_id[METADATA_ID_LEN],
                               uint64_t *size);

/* Confere se chunk_count e igual a ceil(size / CHUNK_SIZE).
 * Retorna METADATA_OK ou METADATA_ERR_INVALID. */
int metadata_check_chunks(const FileMetadata *meta);

/* Serializa no formato: object_id | nome_len (uint16) | nome | size (uint64) |
 * chunk_count (uint32) | hashes | version (uint64) | owner.
 * Retorna METADATA_OK, METADATA_ERR_INVALID ou METADATA_ERR_MEMORY.
 * Em caso de sucesso, quem chamou libera *buffer com free. */
int metadata_serialize(const FileMetadata *meta, uint8_t **buffer, uint32_t *size);

/* Le o formato de metadata_serialize; o tamanho tem de bater exatamente.
 * Retorna METADATA_OK, METADATA_ERR_INVALID ou METADATA_ERR_MEMORY.
 * Liberar meta com metadata_free. */
int metadata_deserialize(const uint8_t *buffer, uint32_t size, FileMetadata *meta);

/* Insere uma copia do metadado na tabela e imprime o metadado novo.
 * ObjectID ja registrado nao muda nada e retorna METADATA_OK.
 * Retorna tambem METADATA_ERR_NAME_TAKEN, METADATA_ERR_FULL,
 * METADATA_ERR_INVALID ou METADATA_ERR_MEMORY. Trava o mutex da tabela. */
int metadata_table_insert(const FileMetadata *meta);

/* Busca pelo ObjectID e copia em out (liberar com metadata_free).
 * Retorna METADATA_OK, METADATA_ERR_NOT_FOUND ou METADATA_ERR_INVALID.
 * Trava o mutex da tabela. */
int metadata_table_find_by_id(const uint8_t object_id[METADATA_ID_LEN], FileMetadata *out);

/* Busca pelo nome e copia em out (liberar com metadata_free).
 * Retorna METADATA_OK, METADATA_ERR_NOT_FOUND ou METADATA_ERR_INVALID.
 * Trava o mutex da tabela. */
int metadata_table_find_by_name(const char *filename, FileMetadata *out);

/* Imprime todos os metadados registrados. Trava o mutex da tabela. */
void metadata_table_print(void);

/* Registra node_id como provedor do arquivo (sem repetir).
 * Retorna METADATA_OK, METADATA_ERR_NOT_FOUND, METADATA_ERR_FULL (ja tem
 * MAX_PROVIDERS) ou METADATA_ERR_INVALID. Trava o mutex da tabela. */
int metadata_add_provider(const uint8_t object_id[METADATA_ID_LEN], const uint8_t node_id[32]);

/* Copia os provedores do arquivo em out_providers (espaco para MAX_PROVIDERS)
 * e a quantidade em *count. Retorna METADATA_OK, METADATA_ERR_NOT_FOUND ou
 * METADATA_ERR_INVALID. Trava o mutex da tabela. */
int metadata_get_providers(const uint8_t object_id[METADATA_ID_LEN], uint8_t out_providers[][32], int *count);

#endif
