#ifndef NETWORK_H
#define NETWORK_H

#include "protocol.h"

/* Abre um socket TCP escutando na porta, em todas as interfaces.
 * Retorna o descritor ou -1; quem chamou fecha com close. */
int criar_servidor(int porta);

/* Espera e aceita a proxima conexao no socket do servidor.
 * Retorna o descritor do cliente ou -1; quem chamou fecha com close. */
int aceitar_cliente(int servidor_fd);

/* Conecta em ip:porta (IPv4 em texto).
 * Retorna o descritor ou -1; quem chamou fecha com close. */
int conectar_no_servidor(const char *ip, int porta);

/* Empacota a mensagem e envia todos os bytes pelo socket.
 * Retorna 0 ou -1. Nao libera o payload de msg. */
int enviar_mensagem(int socket, const Mensagem *msg);

/* Le uma mensagem inteira do socket e confere tamanho e checksum.
 * Retorna 0, -1 (erro de leitura ou memoria) ou -2 (checksum errado).
 * Em caso de sucesso, liberar o payload com liberar_mensagem. */
int receber_mensagem(int socket, Mensagem *msg);

#endif
