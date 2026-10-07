# CHECKPOINT 1
### Para compilar todos os executaveis do projeto:
    "make clean"
    "make"

### Para rodar os testes enviador do checkpoint 1:
    "bash tests/c1/script_testes.sh"

### Para testar a comunicacao manualmente:
    Servidor (superpeer), Inicia o noh que ficara escutando as requisicoes na porta 55101:
        "./bin/superpeer superpeer 127.0.0.1 55101 sp.uuid"

    Cliente (peer) Envia um comando para o superpeer como ping, join ou leave:
        "./bin/peer --cmd X --host 127.0.0.1 --port 55101 --ip 127.0.0.2 --myport 55102 --uuid peerY.uuid"

    X = operacao(join, ping ou leave)
    Y = numero da maquina conectando

# CHECKPOINT 2

### Para LZ4 neste Checkpoint, a máquina precisa ter as bibliotecas instaladas: 
    sudo apt-get install build-essential libssl-dev liblz4-dev

### para realizar o upload primeiro o join para estabelecer a conexao
    ./bin/peer upload arquivo.pdf 
    ./bin/peer download arquivo.pdf 

### o arquivo baixado e salvo como downloaded_<nome>, ou no caminho passado em --output
    ./bin/peer download arquivo.pdf --output saida.pdf
