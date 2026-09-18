# compilador e parametros
CC = gcc
CFLAGS = -Wall -Wextra -O2

# regra principal
all: test_protocol

# compila os testes do protocolo
test_protocol: test_protocol.c ../../protocol.c
	$(CC) $(CFLAGS) -o test_protocol test_protocol.c

# limpa o binario gerado
clean:
	rm -f test_protocol
