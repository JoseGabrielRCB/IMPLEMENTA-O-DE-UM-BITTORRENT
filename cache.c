#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "include/metadata.h"
#include "include/cache.h"

/* Entrada da cache LFU */
typedef struct {
    int used;
    FileMetadata meta;
    uint64_t frequency;
    uint64_t last_access;
} CacheEntry;

/* Estado da cache LFU, relogio logico de acesso, metricas e mutex */
static CacheEntry cache_entries[CACHE_CAPACITY];
static uint64_t cache_clock = 0;
static uint64_t cache_hits = 0;
static uint64_t cache_misses = 0;
static uint64_t cache_evictions = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Procura uma entrada pelo nome ou basename; usar com cache_mutex travado */
static int cache_find_name(const char *filename)
{
    int i;

    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (!cache_entries[i].used) {
            continue;
        }
        if (strcmp(cache_entries[i].meta.filename, filename) == 0) {
            return i;
        }
        const char *slash = strrchr(cache_entries[i].meta.filename, '/');
        if (slash != NULL && strcmp(slash + 1, filename) == 0) {
            return i;
        }
    }
    return -1;
}

/* Procura uma entrada pelo ObjectID; usar com cache_mutex travado */
static int cache_find_id(const uint8_t object_id[METADATA_ID_LEN])
{
    int i;

    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (cache_entries[i].used &&
            memcmp(cache_entries[i].meta.object_id, object_id, METADATA_ID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* Escolhe uma posicao livre ou a vitima LFU (menor frequency, desempate menor last_access) */
static int cache_select_slot(int *foi_evict)
{
    int vitima = 0;
    int i;

    *foi_evict = 0;
    for (i = 0; i < CACHE_CAPACITY; i++) {
        if (!cache_entries[i].used) {
            return i;
        }
    }

    *foi_evict = 1;
    for (i = 1; i < CACHE_CAPACITY; i++) {
        if (cache_entries[i].frequency < cache_entries[vitima].frequency ||
            (cache_entries[i].frequency == cache_entries[vitima].frequency &&
             cache_entries[i].last_access < cache_entries[vitima].last_access)) {
            vitima = i;
        }
    }
    return vitima;
}

/* Busca pelo nome na cache LFU */
int cache_get_by_name(const char *filename, FileMetadata *out)
{
    int indice;
    int resultado = -1;

    if (filename == NULL || out == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&cache_mutex) != 0) {
        return -1;
    }

    indice = cache_find_name(filename);
    if (indice >= 0) {
        if (metadata_copy(out, &cache_entries[indice].meta) == METADATA_OK) {
            cache_entries[indice].frequency++;
            cache_entries[indice].last_access = ++cache_clock;
            cache_hits++;
            printf("LFU CACHE HIT: %s (frequency=%llu, hits=%llu, misses=%llu)\n",
                   filename,
                   (unsigned long long)cache_entries[indice].frequency,
                   (unsigned long long)cache_hits,
                   (unsigned long long)cache_misses);
            resultado = 0;
        }
    } else {
        cache_misses++;
        printf("LFU CACHE MISS: %s (hits=%llu, misses=%llu)\n",
               filename,
               (unsigned long long)cache_hits,
               (unsigned long long)cache_misses);
    }

    pthread_mutex_unlock(&cache_mutex);
    return resultado;
}

/* Insere ou atualiza um metadado na cache LFU */
int cache_put(const FileMetadata *meta)
{
    FileMetadata copia;
    int indice;
    int foi_evict = 0;

    if (meta == NULL || strnlen(meta->filename, METADATA_NAME_LEN) == 0) {
        return -1;
    }
    if (metadata_copy(&copia, meta) != METADATA_OK) {
        return -1;
    }
    if (pthread_mutex_lock(&cache_mutex) != 0) {
        metadata_free(&copia);
        return -1;
    }

    indice = cache_find_id(meta->object_id);
    if (indice < 0) {
        indice = cache_find_name(meta->filename);
    }

    if (indice >= 0) {
        metadata_free(&cache_entries[indice].meta);
        cache_entries[indice].meta = copia;
        cache_entries[indice].frequency++;
        cache_entries[indice].last_access = ++cache_clock;
        pthread_mutex_unlock(&cache_mutex);
        return 0;
    }

    indice = cache_select_slot(&foi_evict);
    if (foi_evict) {
        cache_evictions++;
        printf("LFU CACHE EVICT: %s (frequency=%llu, last_access=%llu, evictions=%llu)\n",
               cache_entries[indice].meta.filename,
               (unsigned long long)cache_entries[indice].frequency,
               (unsigned long long)cache_entries[indice].last_access,
               (unsigned long long)cache_evictions);
        metadata_free(&cache_entries[indice].meta);
    }

    cache_entries[indice].used = 1;
    cache_entries[indice].meta = copia;
    cache_entries[indice].frequency = 1;
    cache_entries[indice].last_access = ++cache_clock;
    printf("LFU CACHE INSERT: %s (frequency=1, slot=%d/%d)\n",
           copia.filename, indice, CACHE_CAPACITY);

    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

/* Remove uma entrada da cache pelo ObjectID */
void cache_invalidate(const uint8_t object_id[METADATA_ID_LEN])
{
    int indice;

    if (object_id == NULL || pthread_mutex_lock(&cache_mutex) != 0) {
        return;
    }
    indice = cache_find_id(object_id);
    if (indice >= 0) {
        printf("LFU CACHE INVALIDATE: %s\n", cache_entries[indice].meta.filename);
        metadata_free(&cache_entries[indice].meta);
        cache_entries[indice].used = 0;
        cache_entries[indice].frequency = 0;
        cache_entries[indice].last_access = 0;
    }
    pthread_mutex_unlock(&cache_mutex);
}

/* Imprime as metricas acumuladas da cache */
void cache_print_stats(void)
{
    uint64_t total;
    double taxa = 0.0;

    if (pthread_mutex_lock(&cache_mutex) != 0) {
        return;
    }
    total = cache_hits + cache_misses;
    if (total > 0) {
        taxa = (100.0 * (double)cache_hits) / (double)total;
    }
    printf("LFU Cache stats: hits=%llu misses=%llu evictions=%llu hit_rate=%.1f%%\n",
           (unsigned long long)cache_hits,
           (unsigned long long)cache_misses,
           (unsigned long long)cache_evictions,
           taxa);
    pthread_mutex_unlock(&cache_mutex);
}
