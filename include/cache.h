#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>

#include "metadata.h"

/* Capacidade da cache LFU de metadados */
#define CACHE_CAPACITY 8

/* Busca o metadado pelo nome na cache LFU.
 * Em caso de CACHE HIT, incrementa frequency, atualiza last_access, copia o
 * metadado para *out (liberar com metadata_free), imprime a linha de HIT e
 * retorna 0. Em caso de CACHE MISS, imprime a linha de MISS e retorna -1. */
int cache_get_by_name(const char *filename, FileMetadata *out);

/* Insere ou atualiza o metadado na cache LFU.
 * Se a cache estiver cheia, remove a entrada de menor frequency (desempate
 * pelo menor last_access) e imprime a linha de EVICT. Retorna 0 ou -1. */
int cache_put(const FileMetadata *meta);

/* Invalida uma entrada da cache pelo ObjectID. */
void cache_invalidate(const uint8_t object_id[METADATA_ID_LEN]);

/* Imprime as metricas acumuladas da cache (hits, misses, evictions, hit rate). */
void cache_print_stats(void);

#endif
