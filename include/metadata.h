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

/* Compression e Status do metadado (Secao 8.3); Status 0 = ativo */
#define METADATA_COMPRESSION_NONE 0
#define METADATA_COMPRESSION_LZ4 1
#define METADATA_STATUS_ACTIVE 0
#define METADATA_STATUS_REPLICATING 1
#define METADATA_STATUS_REMOVED 2

/* Maximo de Super Peers registrados em ReplicaPeers */
#define METADATA_REPLICA_MAX 16

/* Bytes fixos depois do dono na serializacao: compression | upload_date |
 * last_access | download_counter | status | replica_count */
#define METADATA_EXTRA_LEN (1 + 8 + 8 + 8 + 1 + 1)

/* Retornos das operacoes de metadados */
#define METADATA_OK 0
#define METADATA_ERR_INVALID (-1)
#define METADATA_ERR_MEMORY (-2)
#define METADATA_ERR_FULL (-3)
#define METADATA_ERR_NAME_TAKEN (-4)
#define METADATA_ERR_NOT_FOUND (-5)
#define METADATA_ERR_FILE (-6)

/* Metadado de um arquivo registrado (os 13 campos da Secao 8.3);
 * chunk_hashes tem chunk_count hashes */
typedef struct {
    uint8_t   object_id[32];      /* ObjectID = SHA-256 do arquivo */
    char      filename[256];      /* Filename */
    uint64_t  size;               /* Size */
    uint32_t  chunk_count;        /* ChunkCount */
    uint8_t **chunk_hashes;       /* ChunkHash[] */
    uint64_t  version;            /* Version */
    uint8_t   owner[32];          /* OwnerPeer: NodeID de quem publicou */
    uint8_t   compression;        /* Compression: METADATA_COMPRESSION_* */
    uint64_t  upload_date;        /* UploadDate: segundos desde 1970 */
    uint64_t  last_access;        /* LastAccess: ultimo LOOKUP neste Super Peer (local) */
    uint64_t  download_counter;   /* DownloadCounter: LOOKUPs neste Super Peer (local) */
    uint8_t   status;             /* Status: METADATA_STATUS_* */
    uint8_t   replica_count;      /* ReplicaPeers: Super Peers que aplicaram a entrada */
    uint8_t   replica_peers[METADATA_REPLICA_MAX][32];
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

/* Imprime os demais campos da Secao 8.3 (versao, status, dono, datas, contadores, replicas). */
void metadata_print_details(const FileMetadata *meta);

/* Nome do status (ACTIVE, REPLICATING, REMOVED). */
const char *metadata_status_name(uint8_t status);

/* Calcula o ObjectID (SHA-256 do arquivo inteiro) e o tamanho do arquivo.
 * Retorna METADATA_OK, METADATA_ERR_INVALID, METADATA_ERR_FILE (arquivo
 * ausente, vazio ou ilegivel) ou METADATA_ERR_MEMORY. */
int metadata_compute_object_id(const char *path, uint8_t object_id[METADATA_ID_LEN],
                               uint64_t *size);

/* Confere se chunk_count e igual a ceil(size / CHUNK_SIZE).
 * Retorna METADATA_OK ou METADATA_ERR_INVALID. */
int metadata_check_chunks(const FileMetadata *meta);

/* Serializa no formato: object_id | nome_len (uint16) | nome | size (uint64) |
 * chunk_count (uint32) | hashes | version (uint64) | owner | compression (uint8) |
 * upload_date (uint64) | last_access (uint64) | download_counter (uint64) |
 * status (uint8) | replica_count (uint8) | replica_peers (32 bytes cada).
 * Retorna METADATA_OK, METADATA_ERR_INVALID ou METADATA_ERR_MEMORY.
 * Em caso de sucesso, quem chamou libera *buffer com free. */
int metadata_serialize(const FileMetadata *meta, uint8_t **buffer, uint32_t *size);

/* Le o formato de metadata_serialize; o tamanho tem de bater exatamente.
 * Retorna METADATA_OK, METADATA_ERR_INVALID ou METADATA_ERR_MEMORY.
 * Liberar meta com metadata_free. */
int metadata_deserialize(const uint8_t *buffer, uint32_t size, FileMetadata *meta);

/* Insere uma copia do metadado na tabela e imprime o metadado novo.
 * ObjectID ja registrado: se o status mudou (remocao logica ou reativacao), atualiza
 * status e ReplicaPeers e faz version++; senao nao muda nada. Retorna METADATA_OK.
 * Retorna tambem METADATA_ERR_NAME_TAKEN, METADATA_ERR_FULL,
 * METADATA_ERR_INVALID ou METADATA_ERR_MEMORY. Trava o mutex da tabela. */
int metadata_table_insert(const FileMetadata *meta);

/* Busca pelo ObjectID e copia em out (liberar com metadata_free).
 * Registros com status REMOVED contam como nao encontrados.
 * Retorna METADATA_OK, METADATA_ERR_NOT_FOUND ou METADATA_ERR_INVALID.
 * Trava o mutex da tabela. */
int metadata_table_find_by_id(const uint8_t object_id[METADATA_ID_LEN], FileMetadata *out);

/* Busca pelo nome e copia em out (liberar com metadata_free).
 * Registros com status REMOVED contam como nao encontrados.
 * Retorna METADATA_OK, METADATA_ERR_NOT_FOUND ou METADATA_ERR_INVALID.
 * Trava o mutex da tabela. */
int metadata_table_find_by_name(const char *filename, FileMetadata *out);

/* Estatistica local de acesso (RF-024): LastAccess = agora e DownloadCounter++.
 * Copia os valores atualizados em *last_access e *downloads (podem ser NULL).
 * Retorna METADATA_OK ou METADATA_ERR_NOT_FOUND. Trava o mutex da tabela. */
int metadata_table_touch(const uint8_t object_id[METADATA_ID_LEN], uint64_t *last_access,
                         uint64_t *downloads);

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
