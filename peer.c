#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

#include <lz4.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include "include/node.h"
#include "include/network.h"
#include "include/metadata.h"

void calcular_hash_global(const char *caminho, uint8_t *hash_saida)
{
    FILE *f = fopen(caminho, "rb");
    if (!f)
        return;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        fclose(f);
        return;
    }
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    uint8_t buffer[32768];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        EVP_DigestUpdate(ctx, buffer, bytes);
    }
    unsigned int out_len = 0;
    EVP_DigestFinal_ex(ctx, hash_saida, &out_len);
    EVP_MD_CTX_free(ctx);
    fclose(f);
}
/*
arquivo
   ↓
SHA-256
   ↓
fragmentação
   ↓
LZ4
   ↓
checksum
   ↓
transferência
*/
int processar_upload(const char *caminho, FileMetadata *meta)
{
    uint8_t object_id[32];
    calcular_hash_global(caminho, object_id);

    FILE *f = fopen(caminho, "rb");
    if (!f)
    {
        printf("Arquivo %s nao encontrado.\n", caminho);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t chunk_count = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (size == 0)
        chunk_count = 0;

    metadata_create(meta, chunk_count);
    memcpy(meta->object_id, object_id, 32);
    snprintf(meta->filename, sizeof(meta->filename), "%s", caminho);
    meta->size = size;
    meta->compression = METADATA_COMPRESSION_LZ4;
    meta->upload_date = (uint64_t)time(NULL);
    meta->status = METADATA_STATUS_ACTIVE;

    uint8_t *buffer = malloc(CHUNK_SIZE);
    int max_compressed = LZ4_compressBound(CHUNK_SIZE);
    uint8_t *compressed = malloc(max_compressed);

    mkdir("storage", 0755);

    for (uint32_t i = 0; i < chunk_count; i++)
    {
        size_t lido = fread(buffer, 1, CHUNK_SIZE, f);
        int comp_size = LZ4_compress_default((const char *)buffer, (char *)compressed, lido, max_compressed);

        /* SHA-256 do chunk sobre o conteudo original, antes do LZ4 (Secoes 10.4 e 11.3) */
        uint8_t chunk_hash[32];
        SHA256(buffer, lido, chunk_hash);
        memcpy(meta->chunk_hashes[i], chunk_hash, 32);

        char chunk_path[128];
        char hash_hex[65];
        for (int j = 0; j < 32; j++)
            sprintf(&hash_hex[j * 2], "%02x", chunk_hash[j]);
        snprintf(chunk_path, sizeof(chunk_path), "storage/%s.lz4", hash_hex);

        FILE *fc = fopen(chunk_path, "wb");
        if (fc)
        {
            fwrite(compressed, 1, comp_size, fc);
            fclose(fc);
        }
    }

    free(buffer);
    free(compressed);
    fclose(f);

    metadata_print(meta);
    printf("\nCompression: LZ4\n");
    return 0;
}

typedef struct
{
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint8_t chunk_hash[32];
    uint8_t self_id[NODE_ID_LEN];
    char sp_host[256];
    int sp_port;
    const ProviderInfo *providers;
    uint32_t provider_count;
    uint8_t *decomp_buf;
    int decomp_size;
    int status;
} ChunkTask;

static int requisitar_chunk_rede(const char *host, int port, const uint8_t self_id[NODE_ID_LEN],
                                 const uint8_t chunk_hash[32], uint32_t chunk_index,
                                 uint8_t *comp_out, int max_comp)
{
    uint8_t req_payload[32 + sizeof(uint32_t)];
    Mensagem pedido;
    Mensagem resposta;
    int fd;
    int comp_size = -1;

    fd = conectar_no_servidor(host, port);
    if (fd < 0)
        return -1;

    memcpy(req_payload, chunk_hash, 32);
    memcpy(req_payload + 32, &chunk_index, sizeof(uint32_t));

    memset(&pedido, 0, sizeof(pedido));
    memset(&resposta, 0, sizeof(resposta));
    pedido.header.versao_protocolo = VERSAO_PROTOCOLO;
    pedido.header.tipo_mensagem = MSG_DOWNLOAD_REQ;
    memcpy(pedido.header.no_origem, self_id, NODE_ID_LEN);
    pedido.header.tamanho_payload = sizeof(req_payload);
    pedido.payload = req_payload;

    if (enviar_mensagem(fd, &pedido) == 0 && receber_mensagem(fd, &resposta) == 0)
    {
        if (resposta.header.tipo_mensagem == MSG_DOWNLOAD_REP &&
            resposta.payload != NULL &&
            resposta.header.tamanho_payload > 0 &&
            (int)resposta.header.tamanho_payload <= max_comp)
        {
            comp_size = (int)resposta.header.tamanho_payload;
            memcpy(comp_out, resposta.payload, (size_t)comp_size);
        }
    }
    liberar_mensagem(&resposta);
    close(fd);
    return comp_size;
}

static void *baixar_chunk_thread(void *arg)
{
    ChunkTask *t = (ChunkTask *)arg;
    int max_comp = LZ4_compressBound(CHUNK_SIZE);
    uint8_t *compressed = (uint8_t *)malloc((size_t)max_comp);
    uint8_t *buffer = (uint8_t *)malloc(CHUNK_SIZE);
    uint8_t calc_hash[32];
    int comp_size = -1;
    int via_rede = 0;
    char origem_desc[300] = "storage local";

    if (!compressed || !buffer)
    {
        free(compressed);
        free(buffer);
        t->status = -1;
        return NULL;
    }

    if (t->providers != NULL && t->provider_count > 0)
    {
        for (uint32_t attempt = 0; attempt < t->provider_count; attempt++)
        {
            uint32_t idx = (t->chunk_index + attempt) % t->provider_count;
            const ProviderInfo *rep = &t->providers[idx];
            comp_size = requisitar_chunk_rede(rep->ip, rep->port, t->self_id,
                                              t->chunk_hash, t->chunk_index,
                                              compressed, max_comp);
            if (comp_size > 0)
            {
                via_rede = 1;
                snprintf(origem_desc, sizeof(origem_desc), "DOWNLOAD_REP replica %s:%u",
                         rep->ip, rep->port);
                break;
            }
            printf("Chunk %u/%u: timeout/falha na replica %s:%u -> selecionando proxima replica\n",
                   t->chunk_index + 1, t->chunk_count, rep->ip, rep->port);
        }
    }

    if (comp_size <= 0)
    {
        comp_size = requisitar_chunk_rede(t->sp_host, t->sp_port, t->self_id,
                                          t->chunk_hash, t->chunk_index,
                                          compressed, max_comp);
        if (comp_size > 0)
        {
            via_rede = 1;
            snprintf(origem_desc, sizeof(origem_desc), "DOWNLOAD_REP %s:%d",
                     t->sp_host, t->sp_port);
        }
    }

    if (comp_size <= 0)
    {
        char chunk_path[128];
        char hash_hex[65];
        for (int j = 0; j < 32; j++)
            sprintf(&hash_hex[j * 2], "%02x", t->chunk_hash[j]);
        snprintf(chunk_path, sizeof(chunk_path), "storage/%s.lz4", hash_hex);

        FILE *fc = fopen(chunk_path, "rb");
        if (fc)
        {
            fseek(fc, 0, SEEK_END);
            long fsize = ftell(fc);
            fseek(fc, 0, SEEK_SET);
            if (fsize > 0 && fsize <= max_comp &&
                fread(compressed, 1, (size_t)fsize, fc) == (size_t)fsize)
            {
                comp_size = (int)fsize;
            }
            fclose(fc);
        }
    }

    if (comp_size <= 0)
    {
        free(compressed);
        free(buffer);
        t->status = -1;
        return NULL;
    }

    /* Compressed Chunk -> LZ4 Decode -> SHA-256 -> Validacao (Secao 10.4) */
    int decomp_size = LZ4_decompress_safe((const char *)compressed, (char *)buffer, comp_size, CHUNK_SIZE);
    free(compressed);

    if (decomp_size <= 0)
    {
        free(buffer);
        t->status = -1;
        return NULL;
    }

    SHA256(buffer, (size_t)decomp_size, calc_hash);
    if (memcmp(calc_hash, t->chunk_hash, 32) != 0)
    {
        printf("Chunk %u/%u: falha no checksum SHA-256\n", t->chunk_index + 1, t->chunk_count);
        free(buffer);
        t->status = -1;
        return NULL;
    }

    t->decomp_buf = buffer;
    t->decomp_size = decomp_size;
    t->status = 0;
    printf("Transferencia paralela: chunk %u/%u recebido (%s) e verificado (LZ4 + SHA-256)\n",
           t->chunk_index + 1, t->chunk_count, via_rede ? origem_desc : "storage local");
    return NULL;
}

int processar_download_paralelo(FileMetadata *meta, const char *output_path,
                                 const char *sp_host, int sp_port,
                                 const uint8_t self_id[NODE_ID_LEN],
                                 const ProviderInfo *providers, uint32_t provider_count)
{
    FILE *f_out = fopen(output_path, "wb");
    if (!f_out)
    {
        printf("Erro: nao foi possivel criar %s\n", output_path);
        return -1;
    }

    printf("Transfer state: CREATED -> QUEUED -> STARTED -> TRANSFERRING\n");

    uint32_t n = meta->chunk_count;
    if (n > 0)
    {
        pthread_t *threads = (pthread_t *)calloc(n, sizeof(pthread_t));
        ChunkTask *tasks = (ChunkTask *)calloc(n, sizeof(ChunkTask));

        for (uint32_t i = 0; i < n; i++)
        {
            tasks[i].chunk_index = i;
            tasks[i].chunk_count = n;
            memcpy(tasks[i].chunk_hash, meta->chunk_hashes[i], 32);
            memcpy(tasks[i].self_id, self_id, NODE_ID_LEN);
            snprintf(tasks[i].sp_host, sizeof(tasks[i].sp_host), "%s", sp_host);
            tasks[i].sp_port = sp_port;
            tasks[i].providers = providers;
            tasks[i].provider_count = provider_count;
            tasks[i].status = -1;
            pthread_create(&threads[i], NULL, baixar_chunk_thread, &tasks[i]);
        }

        for (uint32_t i = 0; i < n; i++)
        {
            pthread_join(threads[i], NULL);
            if (tasks[i].status == 0 && tasks[i].decomp_buf != NULL)
            {
                fwrite(tasks[i].decomp_buf, 1, (size_t)tasks[i].decomp_size, f_out);
                free(tasks[i].decomp_buf);
            }
        }

        free(threads);
        free(tasks);
    }

    fclose(f_out);

    printf("Transfer state: VERIFYING\n");
    uint8_t final_hash[32];
    calcular_hash_global(output_path, final_hash);
    if (memcmp(final_hash, meta->object_id, 32) == 0)
    {
        printf("Transfer state: FINISHED -> REPLICATED\n");
        printf("Download completed\n");
        printf("SHA-256 verified\n");
        return 0;
    }
    printf("Erro: SHA-256 global do arquivo reconstituido diverge do ObjectID\n");
    return -1;
}

/* Depois do STORE confirmado, envia cada chunk comprimido ao Super Peer (CHUNK_STORE),
 * que valida, grava e replica para os demais Super Peers vivos */
static int enviar_chunks(const FileMetadata *meta, const char *host, int port,
                         const uint8_t self_id[NODE_ID_LEN])
{
    int max_comp = LZ4_compressBound(CHUNK_SIZE);
    uint8_t *payload = (uint8_t *)malloc((size_t)max_comp + 33);
    int falhas = 0;

    if (payload == NULL)
        return -1;

    for (uint32_t i = 0; i < meta->chunk_count; i++)
    {
        char hash_hex[65];
        char chunk_path[128];
        long fsize = 0;
        FILE *fc;
        Mensagem pedido;
        Mensagem resposta;
        int ok = 0;

        for (int j = 0; j < 32; j++)
            sprintf(&hash_hex[j * 2], "%02x", meta->chunk_hashes[i][j]);
        snprintf(chunk_path, sizeof(chunk_path), "storage/%s.lz4", hash_hex);

        fc = fopen(chunk_path, "rb");
        if (fc != NULL)
        {
            fseek(fc, 0, SEEK_END);
            fsize = ftell(fc);
            fseek(fc, 0, SEEK_SET);
            if (fsize <= 0 || fsize > max_comp ||
                fread(payload + 33, 1, (size_t)fsize, fc) != (size_t)fsize)
                fsize = 0;
            fclose(fc);
        }
        if (fsize > 0)
        {
            memcpy(payload, meta->chunk_hashes[i], 32);
            payload[32] = 1; /* Super Peer de entrada replica para os demais */

            memset(&pedido, 0, sizeof(pedido));
            memset(&resposta, 0, sizeof(resposta));
            pedido.header.versao_protocolo = VERSAO_PROTOCOLO;
            pedido.header.tipo_mensagem = MSG_CHUNK_STORE;
            memcpy(pedido.header.no_origem, self_id, NODE_ID_LEN);
            pedido.header.tamanho_payload = (uint32_t)fsize + 33;
            pedido.payload = payload;

            int fd = conectar_no_servidor(host, port);
            if (fd >= 0)
            {
                if (enviar_mensagem(fd, &pedido) == 0 && receber_mensagem(fd, &resposta) == 0 &&
                    resposta.header.tipo_mensagem == MSG_ACK)
                    ok = 1;
                liberar_mensagem(&resposta);
                close(fd);
            }
        }
        if (ok)
            printf("Chunk %u/%u enviado ao Super Peer %s:%d e replicado\n",
                   i + 1, meta->chunk_count, host, port);
        else
        {
            printf("Chunk %u/%u: falha ao enviar ao Super Peer %s:%d\n",
                   i + 1, meta->chunk_count, host, port);
            falhas++;
        }
    }
    free(payload);
    return falhas == 0 ? 0 : -1;
}

// separa o metadado da lista de provedores (count + ProviderInfo) no fim da resposta do LOOKUP
int separar_provedores(const uint8_t *payload, uint32_t tamanho, uint32_t *tamanho_meta,
                       ProviderInfo providers_out[MAX_PROVIDERS], uint32_t *provider_count_out)
{
    uint16_t nome_len;
    uint32_t chunk_count;
    uint32_t provedores;
    uint64_t pos;
    uint64_t fim;

    if (provider_count_out != NULL)
        *provider_count_out = 0;
    if (payload == NULL || tamanho < METADATA_ID_LEN + sizeof(uint16_t))
        return -1;
    memcpy(&nome_len, payload + METADATA_ID_LEN, sizeof(uint16_t));
    pos = (uint64_t)METADATA_ID_LEN + sizeof(uint16_t) + nome_len + sizeof(uint64_t);
    if (pos + sizeof(uint32_t) > tamanho)
        return -1;
    memcpy(&chunk_count, payload + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t) + (uint64_t)chunk_count * METADATA_HASH_LEN + sizeof(uint64_t) + METADATA_ID_LEN;
    /* compression | upload_date | last_access | download_counter | status | replica_count */
    pos += METADATA_EXTRA_LEN;
    if (pos > tamanho)
        return -1;
    pos += (uint64_t)payload[pos - 1] * METADATA_ID_LEN;
    if (pos + sizeof(uint32_t) > tamanho)
        return -1;
    memcpy(&provedores, payload + pos, sizeof(uint32_t));
    fim = pos + sizeof(uint32_t) + (uint64_t)provedores * sizeof(ProviderInfo);
    if (fim != tamanho)
        return -1;

    uint32_t salvos = 0;
    for (uint32_t i = 0; i < provedores; i++)
    {
        ProviderInfo info;
        memcpy(&info, payload + pos + sizeof(uint32_t) + i * sizeof(ProviderInfo), sizeof(info));
        info.ip[sizeof(info.ip) - 1] = '\0';
        printf("Provedor: %s:%u\n", info.ip, info.port);
        if (providers_out != NULL && salvos < MAX_PROVIDERS)
        {
            providers_out[salvos++] = info;
        }
    }
    if (provider_count_out != NULL)
        *provider_count_out = salvos;
    *tamanho_meta = (uint32_t)pos;
    return 0;
}

// converte o ObjectID de 64 caracteres hex para 32 bytes
int ler_object_id(const char *hex, uint8_t object_id[NODE_ID_LEN])
{
    if (strlen(hex) != NODE_ID_LEN * 2)
        return -1;
    for (int i = 0; i < NODE_ID_LEN; i++)
    {
        unsigned int byte;
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &byte) != 1)
            return -1;
        object_id[i] = (uint8_t)byte;
    }
    return 0;
}

/* Executa um comando contra o Super Peer host:porta e devolve 0 em caso de sucesso.
 * Os campos que o comando nao usa podem vir vazios. */
static int executar_comando(Node *peer, const char *host, int porta, const char *cmd,
                            const char *file_param, const char *name_param,
                            const char *output_param, const char *object_id_param)
{
    uint8_t object_id[NODE_ID_LEN];
    char endereco[32];
    int rc = 1;

    if (strcmp(cmd, "lookup") == 0 && strlen(name_param) == 0 &&
        ler_object_id(object_id_param, object_id) != 0)
    {
        fprintf(stderr, "Erro: --object-id precisa de 64 caracteres hex\n");
        return 1;
    }

    peer->state = STATE_CONNECTING;
    int socket_fd = conectar_no_servidor(host, porta);
    if (socket_fd < 0 && porta == 55101)
    {
        porta = 5101;
        socket_fd = conectar_no_servidor(host, porta);
    }
    if (socket_fd < 0)
    {
        printf("Erro: nao foi possivel conectar ao Super Peer %s:%d\n", host, porta);
        return 1;
    }
    peer->state = STATE_CONNECTED;

    // formata pacote com o tamanho exato da string usada
    Mensagem msg;
    memset(&msg, 0, sizeof(Mensagem));
    msg.header.versao_protocolo = 1;

    // identifica a origem do pacote com o NodeID do peer
    memcpy(msg.header.no_origem, peer->node_id, NODE_ID_LEN);

    if (strcmp(cmd, "ping") == 0)
    {
        msg.header.tipo_mensagem = MSG_PING;
        msg.header.tamanho_payload = 4;
        msg.payload = (uint8_t *)strdup("PING");
    }
    else if (strcmp(cmd, "join") == 0)
    {
        snprintf(endereco, sizeof(endereco), "%s:%u", peer->ip, peer->port);
        msg.header.tipo_mensagem = MSG_JOIN;
        msg.header.tamanho_payload = (uint32_t)strlen(endereco);
        msg.payload = (uint8_t *)strdup(endereco);
    }
    else if (strcmp(cmd, "upload") == 0)
    {
        FileMetadata meta;
        if (processar_upload(file_param, &meta) == 0)
        {
            uint8_t *payload = NULL;
            uint32_t tam = 0;
            if (metadata_serialize(&meta, &payload, &tam) == METADATA_OK)
            {
                msg.header.tipo_mensagem = MSG_STORE;
                msg.header.tamanho_payload = tam;
                msg.payload = payload;
            }
            metadata_free(&meta);
        }
    }
    else if (strcmp(cmd, "download") == 0)
    {
        msg.header.tipo_mensagem = MSG_LOOKUP;
        msg.header.tamanho_payload = (uint32_t)strlen(name_param);
        msg.payload = (uint8_t *)strdup(name_param);
    }
    else if (strcmp(cmd, "topology") == 0)
    {
        msg.header.tipo_mensagem = MSG_TOPOLOGY;
        msg.header.tamanho_payload = 0;
    }
    else if (strcmp(cmd, "lookup") == 0)
    {
        if (strlen(name_param) > 0)
        {
            msg.header.tipo_mensagem = MSG_LOOKUP;
            msg.header.tamanho_payload = (uint32_t)strlen(name_param);
            msg.payload = (uint8_t *)strdup(name_param);
        }
        else
        {
            msg.header.tipo_mensagem = MSG_CHORD_LOOKUP;
            msg.header.tamanho_payload = NODE_ID_LEN;
            msg.payload = (uint8_t *)malloc(NODE_ID_LEN);
            if (msg.payload != NULL)
                memcpy(msg.payload, object_id, NODE_ID_LEN);
        }
    }
    else if (strcmp(cmd, "leave") == 0)
    {
        msg.header.tipo_mensagem = MSG_LEAVE;
        msg.header.tamanho_payload = 5;
        msg.payload = (uint8_t *)strdup("LEAVE");
    }
    else
    {
        msg.header.tipo_mensagem = MSG_PING;
        msg.header.tamanho_payload = 4;
        msg.payload = (uint8_t *)strdup("PING");
    }

    // sem payload nao ha o que enviar
    if (msg.payload == NULL && msg.header.tipo_mensagem != MSG_TOPOLOGY)
    {
        close(socket_fd);
        return 1;
    }

    // envia instrucao e aguarda imediato retorno para log
    if (enviar_mensagem(socket_fd, &msg) == 0)
    {
        Mensagem resposta;
        memset(&resposta, 0, sizeof(Mensagem));
        if (receber_mensagem(socket_fd, &resposta) == 0)
        {
            int ack = resposta.header.tipo_mensagem == MSG_ACK;

            // topology e lookup recebem o texto pronto do super peer
            if ((msg.header.tipo_mensagem == MSG_TOPOLOGY || msg.header.tipo_mensagem == MSG_CHORD_LOOKUP) &&
                ack && resposta.payload != NULL)
            {
                fwrite(resposta.payload, 1, resposta.header.tamanho_payload, stdout);
                rc = 0;
            }
            else if (msg.header.tipo_mensagem == MSG_TOPOLOGY || msg.header.tipo_mensagem == MSG_CHORD_LOOKUP)
            {
                printf("Erro: super peer respondeu %s\n", nome_do_tipo(resposta.header.tipo_mensagem));
            }
            else if (msg.header.tipo_mensagem == MSG_STORE && ack)
            {
                FileMetadata enviado;
                if (metadata_deserialize(msg.payload, msg.header.tamanho_payload, &enviado) == METADATA_OK)
                {
                    rc = enviar_chunks(&enviado, host, porta, peer->node_id) == 0 ? 0 : 1;
                    metadata_free(&enviado);
                }
            }
            else if (msg.header.tipo_mensagem == MSG_STORE)
            {
                printf("Erro: super peer respondeu %s ao STORE\n", nome_do_tipo(resposta.header.tipo_mensagem));
            }
            else if (msg.header.tipo_mensagem == MSG_JOIN)
            {
                if (ack)
                {
                    peer->state = STATE_AUTHENTICATED;
                    rc = 0;
                }
                else
                {
                    printf("Erro: super peer respondeu %s ao JOIN\n", nome_do_tipo(resposta.header.tipo_mensagem));
                }
            }
            else if (msg.header.tipo_mensagem == MSG_LOOKUP && ack)
            {
                FileMetadata meta;
                uint32_t tamanho_meta = 0;
                ProviderInfo providers[MAX_PROVIDERS];
                uint32_t provider_count = 0;
                if (separar_provedores(resposta.payload, resposta.header.tamanho_payload, &tamanho_meta,
                                       providers, &provider_count) == 0 &&
                    metadata_deserialize(resposta.payload, tamanho_meta, &meta) == METADATA_OK)
                {
                    if (strcmp(cmd, "download") == 0)
                    {
                        char out_file[256];
                        if (strlen(output_param) > 0)
                            snprintf(out_file, sizeof(out_file), "%s", output_param);
                        else
                            snprintf(out_file, sizeof(out_file), "downloaded_%s", name_param);
                        rc = processar_download_paralelo(&meta, out_file, host, porta, peer->node_id,
                                                         providers, provider_count) == 0 ? 0 : 1;
                    }
                    else
                    {
                        metadata_print(&meta);
                        metadata_print_details(&meta);
                        rc = 0;
                    }
                    metadata_free(&meta);
                }
                else
                {
                    printf("Erro: resposta invalida ao LOOKUP\n");
                }
            }
            else if (msg.header.tipo_mensagem == MSG_LOOKUP)
            {
                printf("Erro: %s nao encontrado (super peer respondeu %s)\n", name_param,
                       nome_do_tipo(resposta.header.tipo_mensagem));
            }
            else
            {
                // ping e leave: qualquer resposta conta como sucesso
                printf("Resposta: %s\n", nome_do_tipo(resposta.header.tipo_mensagem));
                rc = 0;
            }
        }
        else
        {
            printf("Erro: sem resposta do Super Peer %s:%d\n", host, porta);
        }
        liberar_mensagem(&resposta);
    }
    else
    {
        printf("Erro: falha ao enviar ao Super Peer %s:%d\n", host, porta);
    }

    liberar_mensagem(&msg);
    close(socket_fd);
    return rc;
}

/* Remocao logica (Secao 8.4): busca o metadado pelo nome, marca Status = REMOVED e
 * envia um STORE. O coordenador registra DELETE no log via 2PC (Secao 12.3), e cada
 * Super Peer marca o registro como REMOVED, com Version++. */
static int executar_delete(Node *peer, const char *host, int porta, const char *nome)
{
    Mensagem msg;
    Mensagem resposta;
    FileMetadata meta;
    ProviderInfo providers[MAX_PROVIDERS];
    uint32_t tamanho_meta = 0;
    uint32_t n_providers = 0;
    uint8_t *payload = NULL;
    uint32_t tam = 0;
    int achou = 0;
    int rc = 1;
    int fd;

    if (nome == NULL || nome[0] == '\0')
    {
        printf("uso: delete <nome>\n");
        return 2;
    }

    /* 1) LOOKUP: o metadado atual do arquivo */
    fd = conectar_no_servidor(host, porta);
    if (fd < 0)
    {
        printf("Erro: nao foi possivel conectar ao Super Peer %s:%d\n", host, porta);
        return 1;
    }
    memset(&msg, 0, sizeof(msg));
    memset(&resposta, 0, sizeof(resposta));
    msg.header.versao_protocolo = 1;
    msg.header.tipo_mensagem = MSG_LOOKUP;
    memcpy(msg.header.no_origem, peer->node_id, NODE_ID_LEN);
    msg.header.tamanho_payload = (uint32_t)strlen(nome);
    msg.payload = (uint8_t *)strdup(nome);
    if (msg.payload != NULL && enviar_mensagem(fd, &msg) == 0 && receber_mensagem(fd, &resposta) == 0 &&
        resposta.header.tipo_mensagem == MSG_ACK &&
        separar_provedores(resposta.payload, resposta.header.tamanho_payload, &tamanho_meta,
                           providers, &n_providers) == 0 &&
        metadata_deserialize(resposta.payload, tamanho_meta, &meta) == METADATA_OK)
    {
        achou = 1;
    }
    liberar_mensagem(&resposta);
    liberar_mensagem(&msg);
    close(fd);
    if (!achou)
    {
        printf("Erro: %s nao encontrado\n", nome);
        return 1;
    }

    /* 2) STORE com Status = REMOVED */
    meta.status = METADATA_STATUS_REMOVED;
    if (metadata_serialize(&meta, &payload, &tam) != METADATA_OK)
    {
        metadata_free(&meta);
        return 1;
    }
    fd = conectar_no_servidor(host, porta);
    if (fd >= 0)
    {
        memset(&msg, 0, sizeof(msg));
        memset(&resposta, 0, sizeof(resposta));
        msg.header.versao_protocolo = 1;
        msg.header.tipo_mensagem = MSG_STORE;
        memcpy(msg.header.no_origem, peer->node_id, NODE_ID_LEN);
        msg.header.tamanho_payload = tam;
        msg.payload = payload;
        payload = NULL;
        if (enviar_mensagem(fd, &msg) == 0 && receber_mensagem(fd, &resposta) == 0 &&
            resposta.header.tipo_mensagem == MSG_ACK)
        {
            printf("Documento %s marcado como REMOVED (remocao logica)\n", meta.filename);
            rc = 0;
        }
        else
        {
            printf("Erro: super peer respondeu %s ao STORE de remocao\n",
                   nome_do_tipo(resposta.header.tipo_mensagem));
        }
        liberar_mensagem(&resposta);
        liberar_mensagem(&msg);
        close(fd);
    }
    else
    {
        printf("Erro: nao foi possivel conectar ao Super Peer %s:%d\n", host, porta);
    }
    free(payload);
    metadata_free(&meta);
    return rc;
}

#define SESSAO_LINHA_MAX 4096
#define SESSAO_ARGS_MAX 8

// separa a linha em palavras; aspas duplas agrupam uma palavra com espacos
static int separar_palavras(char *linha, char *args[], int max)
{
    int n = 0;
    char *p = linha;

    while (*p != '\0' && n < max)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        if (*p == '"')
        {
            p++;
            args[n++] = p;
            while (*p != '\0' && *p != '"')
                p++;
        }
        else
        {
            args[n++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p != '\0')
            *p++ = '\0';
    }
    return n;
}

static void sessao_ajuda(void)
{
    printf("Comandos:\n"
           "  upload <arquivo>            download <nome> [saida]\n"
           "  delete <nome>               (remocao logica: Status = REMOVED)\n"
           "  lookup <nome|object-id>     state [host porta]\n"
           "  ping|join|leave [host porta]\n"
           "  sleep <segundos>            help | quit\n");
}

/* Sessao de varios comandos (--interactive ou --batch) com um NodeID so.
 * Cada comando termina com "[done N rc=R]"; a sessao abre com [ready] e fecha com [bye]. */
static int executar_sessao(Node *peer, const char *host, int porta, FILE *entrada,
                           int para_no_erro)
{
    char linha[SESSAO_LINHA_MAX];
    int numero = 0;
    int rc_final = 0;

    printf("[ready]\n");
    fflush(stdout);

    while (fgets(linha, sizeof(linha), entrada) != NULL)
    {
        char *args[SESSAO_ARGS_MAX];
        char *inicio = linha;
        int argc_cmd;
        int rc = 0;

        linha[strcspn(linha, "\r\n")] = '\0';
        while (*inicio == ' ' || *inicio == '\t')
            inicio++;
        if (*inicio == '\0' || *inicio == '#')
            continue;

        argc_cmd = separar_palavras(inicio, args, SESSAO_ARGS_MAX);
        if (argc_cmd == 0)
            continue;
        if (strcmp(args[0], "quit") == 0 || strcmp(args[0], "exit") == 0)
            break;

        numero++;
        const char *sp_host = host;
        int sp_porta = porta;

        if (strcmp(args[0], "help") == 0)
        {
            sessao_ajuda();
        }
        else if (strcmp(args[0], "sleep") == 0)
        {
            if (argc_cmd < 2 || atoi(args[1]) < 0)
            {
                printf("uso: sleep <segundos>\n");
                rc = 2;
            }
            else
            {
                sleep((unsigned int)atoi(args[1]));
            }
        }
        else if (strcmp(args[0], "upload") == 0)
        {
            if (argc_cmd < 2)
            {
                printf("uso: upload <arquivo>\n");
                rc = 2;
            }
            else
                rc = executar_comando(peer, host, porta, "upload", args[1], "", "", "");
        }
        else if (strcmp(args[0], "download") == 0)
        {
            if (argc_cmd < 2)
            {
                printf("uso: download <nome> [saida]\n");
                rc = 2;
            }
            else
                rc = executar_comando(peer, host, porta, "download", "", args[1],
                                      argc_cmd > 2 ? args[2] : "", "");
        }
        else if (strcmp(args[0], "delete") == 0)
        {
            rc = executar_delete(peer, host, porta, argc_cmd > 1 ? args[1] : "");
        }
        else if (strcmp(args[0], "lookup") == 0)
        {
            uint8_t oid[NODE_ID_LEN];
            if (argc_cmd < 2)
            {
                printf("uso: lookup <nome|object-id>\n");
                rc = 2;
            }
            else if (ler_object_id(args[1], oid) == 0)
                rc = executar_comando(peer, host, porta, "lookup", "", "", "", args[1]);
            else
                rc = executar_comando(peer, host, porta, "lookup", "", args[1], "", "");
        }
        else if (strcmp(args[0], "state") == 0 || strcmp(args[0], "topology") == 0 ||
                 strcmp(args[0], "ping") == 0 || strcmp(args[0], "join") == 0 ||
                 strcmp(args[0], "leave") == 0)
        {
            const char *cmd = strcmp(args[0], "state") == 0 ? "topology" : args[0];
            if (argc_cmd >= 3)
            {
                sp_host = args[1];
                sp_porta = atoi(args[2]);
            }
            if (sp_porta < 1 || sp_porta > 65535)
            {
                printf("Erro: porta invalida\n");
                rc = 2;
            }
            else
                rc = executar_comando(peer, sp_host, sp_porta, cmd, "", "", "", "");
        }
        else
        {
            printf("Erro: comando desconhecido: %s (help lista os comandos)\n", args[0]);
            rc = 2;
        }

        printf("[done %d rc=%d]\n", numero, rc);
        fflush(stdout);
        if (rc != 0)
        {
            rc_final = rc;
            if (para_no_erro)
                break;
        }
    }

    printf("[bye]\n");
    fflush(stdout);
    return rc_final;
}

int main(int argc, char *argv[])
{
    char host[256] = "127.0.0.1";
    int porta = 55101;
    char cmd[256] = "ping";

    char file_param[256] = "";
    char name_param[256] = "";
    char output_param[256] = "";
    char object_id_param[256] = "";

    // identidade do proprio peer, com valores padrao
    char ip_local[NODE_IP_LEN] = "127.0.0.1";
    char porta_local[8] = "55102";
    char arquivo_uuid[NODE_PATH_LEN] = "peer.uuid";
    NodeConfig config;
    Node peer;

    // modo sessao: --interactive le a stdin, --batch le um arquivo ("-" = stdin)
    int interativo = 0;
    const char *arquivo_lote = NULL;
    int continuar_no_erro = 0;

    struct option long_options[] = {
        {"cmd", required_argument, 0, 'c'},
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"ip", required_argument, 0, 'i'},
        {"myport", required_argument, 0, 'm'},
        {"uuid", required_argument, 0, 'u'},
        {"file", required_argument, 0, 'f'},
        {"name", required_argument, 0, 'n'},
        {"output", required_argument, 0, 'o'},
        {"object-id", required_argument, 0, 'x'},
        {"interactive", no_argument, 0, 'I'},
        {"batch", required_argument, 0, 'B'},
        {"keep-going", no_argument, 0, 'K'},
        {0, 0, 0, 0}};

    // saida linha a linha: o harness acompanha o log enquanto a sessao roda
    setvbuf(stdout, NULL, _IOLBF, 0);

    // leitura dos parametros de disparo (adaptado para ser o peer/cliente)
    int opt;
    while ((opt = getopt_long(argc, argv, "c:h:p:i:m:u:f:n:o:x:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'c':
            snprintf(cmd, sizeof(cmd), "%s", optarg);
            break;
        case 'h':
            snprintf(host, sizeof(host), "%s", optarg);
            break;
        case 'p':
            porta = atoi(optarg);
            break;
        case 'i':
            snprintf(ip_local, sizeof(ip_local), "%s", optarg);
            break;
        case 'm':
            snprintf(porta_local, sizeof(porta_local), "%s", optarg);
            break;
        case 'u':
            snprintf(arquivo_uuid, sizeof(arquivo_uuid), "%s", optarg);
            break;
        case 'f':
            snprintf(file_param, sizeof(file_param), "%s", optarg);
            break;
        case 'n':
            snprintf(name_param, sizeof(name_param), "%s", optarg);
            break;
        case 'o':
            snprintf(output_param, sizeof(output_param), "%s", optarg);
            break;
        case 'x':
            snprintf(object_id_param, sizeof(object_id_param), "%s", optarg);
            break;
        case 'I':
            interativo = 1;
            break;
        case 'B':
            arquivo_lote = optarg;
            break;
        case 'K':
            continuar_no_erro = 1;
            break;
        }
    }

    if (optind < argc)
    {
        if (strcmp(argv[optind], "upload") == 0 && optind + 1 < argc)
        {
            snprintf(cmd, sizeof(cmd), "upload");
            snprintf(file_param, sizeof(file_param), "%s", argv[optind + 1]);
        }
        else if (strcmp(argv[optind], "download") == 0 && optind + 1 < argc)
        {
            snprintf(cmd, sizeof(cmd), "download");
            snprintf(name_param, sizeof(name_param), "%s", argv[optind + 1]);
        }
        else if (strcmp(argv[optind], "delete") == 0 && optind + 1 < argc)
        {
            snprintf(cmd, sizeof(cmd), "delete");
            snprintf(name_param, sizeof(name_param), "%s", argv[optind + 1]);
        }
        else if (strcmp(argv[optind], "join") == 0)
        {
            snprintf(cmd, sizeof(cmd), "join");
        }
        else if (strcmp(argv[optind], "leave") == 0)
        {
            snprintf(cmd, sizeof(cmd), "leave");
        }
        else if (strcmp(argv[optind], "ping") == 0)
        {
            snprintf(cmd, sizeof(cmd), "ping");
        }
    }

    // Os comandos upload e download serao executados apos a conexao com o superpeer

    // monta e valida a configuracao antes de gerar o NodeID
    memset(&config, 0, sizeof(config));
    config.role = ROLE_PEER;
    if (node_parse_ip(ip_local, config.ip, sizeof(config.ip)) != 0)
    {
        fprintf(stderr, "Erro: IP invalido: %s\n", ip_local);
        return 1;
    }
    if (node_parse_port(porta_local, &config.port) != 0)
    {
        fprintf(stderr, "Erro: porta invalida: %s\n", porta_local);
        return 1;
    }
    if (porta < 1 || porta > 65535)
    {
        fprintf(stderr, "Erro: porta do superpeer invalida: %d\n", porta);
        return 1;
    }
    if (node_parse_ip(host, config.superpeer_ip, sizeof(config.superpeer_ip)) != 0)
    {
        fprintf(stderr, "Erro: IP do superpeer invalido: %s\n", host);
        return 1;
    }
    config.superpeer_port = (uint16_t)porta;
    snprintf(config.uuid_path, sizeof(config.uuid_path), "%s", arquivo_uuid);

    if (node_init(&peer, &config) != 0)
    {
        return 1;
    }
    // node_print removido para limpar o terminal

    if (interativo || arquivo_lote != NULL)
    {
        FILE *entrada = stdin;
        int rc;

        if (arquivo_lote != NULL && strcmp(arquivo_lote, "-") != 0)
        {
            entrada = fopen(arquivo_lote, "r");
            if (entrada == NULL)
            {
                fprintf(stderr, "Erro: nao foi possivel abrir %s\n", arquivo_lote);
                return 1;
            }
        }
        // a sessao interativa nunca para no erro; o lote para, salvo --keep-going
        rc = executar_sessao(&peer, host, porta, entrada, !interativo && !continuar_no_erro);
        if (entrada != stdin)
            fclose(entrada);
        return interativo ? 0 : rc;
    }

    if (strcmp(cmd, "delete") == 0)
        return executar_delete(&peer, host, porta, name_param);
    return executar_comando(&peer, host, porta, cmd, file_param, name_param,
                            output_param, object_id_param);
}
