# Para compilar todos os executaveis do projeto:
    "make clean"
    "make"

# Para rodar os testes enviador do checkpoint 1:
    "bash tests/c1/script_testes.sh"

# Para testar a comunicacao manualmente:
    Servidor (superpeer), Inicia o noh que ficara escutando as requisicoes na porta 55101:
        "./bin/superpeer superpeer 127.0.0.1 55101 sp.uuid"

    Cliente (peer) Envia um comando para o superpeer como ping, join ou leave:
        "./bin/peer --cmd X --host 127.0.0.1 --port 55101 --ip 127.0.0.2 --myport 55102 --uuid peerY.uuid"

    X = operacao(join, ping ou leave)
    Y = numero da maquina conectando