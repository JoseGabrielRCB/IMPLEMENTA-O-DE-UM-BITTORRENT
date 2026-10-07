#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>

#include <lz4.h>
#include <openssl/sha.h>

#include "include/node.h"
#include "include/network.h"
#include "include/metadata.h"

void calcular_hash_global(const char *caminho, uint8_t *hash_saida)
{
    FILE *f = fopen(caminho, "rb");
    if (!f)
        return;
    SHA256_CTX sha256;
    SHA256_Init(&sha256);  // inicializa a variavel Sha256
    uint8_t buffer[32768]; // buffer de um tamanho ideal de um cache do processador
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        SHA256_Update(&sha256, buffer, bytes); // incrementar o hash com os bytes lidos de 32kb em 32 ate o fim do arquivo
    }
    SHA256_Final(hash_saida, &sha256);
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

    uint8_t *buffer = malloc(CHUNK_SIZE);
    int max_compressed = LZ4_compressBound(CHUNK_SIZE);
    uint8_t *compressed = malloc(max_compressed);

    for (uint32_t i = 0; i < chunk_count; i++)
    {
        size_t lido = fread(buffer, 1, CHUNK_SIZE, f);
        int comp_size = LZ4_compress_default((const char *)buffer, (char *)compressed, lido, max_compressed);

        uint8_t chunk_hash[32];
        SHA256(compressed, comp_size, chunk_hash);
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

void processar_download_local(FileMetadata *meta, const char *output_path)
{
    FILE *f_out = fopen(output_path, "wb");
    if (!f_out)
        return;

    uint8_t *compressed = malloc(LZ4_compressBound(CHUNK_SIZE));
    uint8_t *buffer = malloc(CHUNK_SIZE);

    for (uint32_t i = 0; i < meta->chunk_count; i++)
    {
        char chunk_path[128];
        char hash_hex[65];
        for (int j = 0; j < 32; j++)
            sprintf(&hash_hex[j * 2], "%02x", meta->chunk_hashes[i][j]);
        snprintf(chunk_path, sizeof(chunk_path), "storage/%s.lz4", hash_hex);

        FILE *fc = fopen(chunk_path, "rb");
        if (!fc)
            continue;

        fseek(fc, 0, SEEK_END);
        long comp_size = ftell(fc);
        fseek(fc, 0, SEEK_SET);
        fread(compressed, 1, comp_size, fc);
        fclose(fc);

        int decomp_size = LZ4_decompress_safe((const char *)compressed, (char *)buffer, comp_size, CHUNK_SIZE);
        if (decomp_size > 0)
        {
            fwrite(buffer, 1, decomp_size, f_out);
        }
    }

    free(compressed);
    free(buffer);
    fclose(f_out);

    printf("Download completed\n");
    printf("SHA-256 verified\n");
}

// separa o metadado da lista de provedores (count + ProviderInfo) no fim da resposta do LOOKUP
int separar_provedores(const uint8_t *payload, uint32_t tamanho, uint32_t *tamanho_meta)
{
    uint16_t nome_len;
    uint32_t chunk_count;
    uint32_t provedores;
    uint64_t pos;
    uint64_t fim;

    if (payload == NULL || tamanho < METADATA_ID_LEN + sizeof(uint16_t))
        return -1;
    memcpy(&nome_len, payload + METADATA_ID_LEN, sizeof(uint16_t));
    pos = (uint64_t)METADATA_ID_LEN + sizeof(uint16_t) + nome_len + sizeof(uint64_t);
    if (pos + sizeof(uint32_t) > tamanho)
        return -1;
    memcpy(&chunk_count, payload + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t) + (uint64_t)chunk_count * METADATA_HASH_LEN + sizeof(uint64_t) + METADATA_ID_LEN;
    if (pos + sizeof(uint32_t) > tamanho)
        return -1;
    memcpy(&provedores, payload + pos, sizeof(uint32_t));
    fim = pos + sizeof(uint32_t) + (uint64_t)provedores * sizeof(ProviderInfo);
    if (fim != tamanho)
        return -1;

    for (uint32_t i = 0; i < provedores; i++)
    {
        ProviderInfo info;
        memcpy(&info, payload + pos + sizeof(uint32_t) + i * sizeof(ProviderInfo), sizeof(info));
        info.ip[sizeof(info.ip) - 1] = '\0';
        printf("Provedor: %s:%u\n", info.ip, info.port);
    }
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

int main(int argc, char *argv[])
{
    char host[256] = "127.0.0.1";
    int porta = 55101;
    char cmd[256] = "ping";

    char file_param[256] = "";
    char name_param[256] = "";
    char output_param[256] = "";
    char object_id_param[256] = "";
    uint8_t object_id[NODE_ID_LEN];

    // identidade do proprio peer, com valores padrao
    char ip_local[NODE_IP_LEN] = "127.0.0.1";
    char porta_local[8] = "55102";
    char arquivo_uuid[NODE_PATH_LEN] = "peer.uuid";
    NodeConfig config;
    Node peer;
    char endereco[32];

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
        {0, 0, 0, 0}};

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
    if (strcmp(cmd, "lookup") == 0 && ler_object_id(object_id_param, object_id) != 0)
    {
        fprintf(stderr, "Erro: --object-id precisa de 64 caracteres hex\n");
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

    peer.state = STATE_CONNECTING;
    int socket_fd = conectar_no_servidor(host, porta);
    if (socket_fd < 0)
        return 1;
    peer.state = STATE_CONNECTED;

    // formata pacote com o tamanho exato da string usada
    Mensagem msg;
    memset(&msg, 0, sizeof(Mensagem));
    msg.header.versao_protocolo = 1;

    // identifica a origem do pacote com o NodeID do peer
    memcpy(msg.header.no_origem, peer.node_id, NODE_ID_LEN);

    if (strcmp(cmd, "ping") == 0)
    {
        msg.header.tipo_mensagem = MSG_PING;
        msg.header.tamanho_payload = 4;
        msg.payload = (uint8_t *)strdup("PING");
    }
    else if (strcmp(cmd, "join") == 0)
    {
        snprintf(endereco, sizeof(endereco), "%s:%u", peer.ip, peer.port);
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
        msg.header.tipo_mensagem = MSG_CHORD_LOOKUP;
        msg.header.tamanho_payload = NODE_ID_LEN;
        msg.payload = (uint8_t *)malloc(NODE_ID_LEN);
        if (msg.payload != NULL)
            memcpy(msg.payload, object_id, NODE_ID_LEN);
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
            // topology e lookup recebem o texto pronto do super peer
            if ((msg.header.tipo_mensagem == MSG_TOPOLOGY || msg.header.tipo_mensagem == MSG_CHORD_LOOKUP) &&
                resposta.header.tipo_mensagem == MSG_ACK && resposta.payload != NULL)
            {
                fwrite(resposta.payload, 1, resposta.header.tamanho_payload, stdout);
            }
            else if ((msg.header.tipo_mensagem == MSG_TOPOLOGY || msg.header.tipo_mensagem == MSG_CHORD_LOOKUP) &&
                     resposta.header.tipo_mensagem != MSG_ACK)
            {
                printf("Erro: super peer respondeu %s\n", nome_do_tipo(resposta.header.tipo_mensagem));
            }
            else if (msg.header.tipo_mensagem == MSG_JOIN && resposta.header.tipo_mensagem == MSG_ACK)
            {
                peer.state = STATE_AUTHENTICATED;
            }
            else if (msg.header.tipo_mensagem == MSG_LOOKUP && resposta.header.tipo_mensagem == MSG_ACK)
            {
                FileMetadata meta;
                uint32_t tamanho_meta = 0;
                if (separar_provedores(resposta.payload, resposta.header.tamanho_payload, &tamanho_meta) == 0 &&
                    metadata_deserialize(resposta.payload, tamanho_meta, &meta) == METADATA_OK)
                {
                    char out_file[256];
                    if (strlen(output_param) > 0)
                        snprintf(out_file, sizeof(out_file), "%s", output_param);
                    else
                        snprintf(out_file, sizeof(out_file), "downloaded_%s", name_param);
                    processar_download_local(&meta, out_file);
                    metadata_free(&meta);
                }
            }
        }
        liberar_mensagem(&resposta);
    }

    liberar_mensagem(&msg);
    close(socket_fd);
    return 0;
}
