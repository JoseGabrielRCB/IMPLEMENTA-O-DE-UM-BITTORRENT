# compilador e parametros
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LDLIBS = -lcrypto

# regras principais
all: bin/superpeer bin/peer bin/node bin/client

# cria diretorio de saida
bin:
	mkdir -p bin

# compila o super peer (servidor)
bin/superpeer: superpeer.c node.c network.c protocol.c | bin
	$(CC) $(CFLAGS) -o bin/superpeer superpeer.c $(LDLIBS)

# compila o peer (antigo client.c)
bin/peer: peer.c node.c network.c protocol.c | bin
	$(CC) $(CFLAGS) -o bin/peer peer.c $(LDLIBS)

# aliases para o teste do professor continuar funcionando sem quebrar
bin/node: bin/superpeer
	cp bin/superpeer bin/node

bin/client: bin/peer
	cp bin/peer bin/client

# limpa binarios gerados
clean:
	rm -rf bin
