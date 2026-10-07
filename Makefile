# compilador e parametros
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LDLIBS = -lcrypto -llz4

# objetos de cada executavel
SUPERPEER_OBJS = build/superpeer.o build/node.o build/protocol.o build/network.o \
                 build/metadata.o build/chord.o build/gossip.o
PEER_OBJS = build/peer.o build/node.o build/protocol.o build/network.o build/metadata.o

# regras principais
all: bin/superpeer bin/peer bin/node bin/client

# cria diretorios de saida
bin build:
	mkdir -p $@

# compila cada .c em um .o
build/%.o: %.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

# headers de que cada objeto depende
build/protocol.o: include/protocol.h
build/network.o: include/network.h include/protocol.h
build/node.o: include/node.h
build/metadata.o: include/metadata.h
build/chord.o: include/chord.h include/node.h include/network.h include/protocol.h
build/gossip.o: include/gossip.h include/chord.h include/node.h include/network.h include/protocol.h
build/superpeer.o: include/node.h include/network.h include/protocol.h include/metadata.h \
                   include/chord.h include/gossip.h
build/peer.o: include/node.h include/network.h include/protocol.h include/metadata.h

# liga o super peer (servidor)
bin/superpeer: $(SUPERPEER_OBJS) | bin
	$(CC) $(CFLAGS) -o $@ $(SUPERPEER_OBJS) $(LDLIBS)

# liga o peer (antigo client.c)
bin/peer: $(PEER_OBJS) | bin
	$(CC) $(CFLAGS) -o $@ $(PEER_OBJS) $(LDLIBS)

# aliases para o teste do professor continuar funcionando sem quebrar
bin/node: bin/superpeer
	cp bin/superpeer bin/node

bin/client: bin/peer
	cp bin/peer bin/client

# limpa binarios e objetos gerados
clean:
	rm -rf bin build

.PHONY: all clean
