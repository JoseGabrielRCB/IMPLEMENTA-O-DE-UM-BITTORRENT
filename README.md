# CHECKPOINT 1
### Para compilar todos os executaveis do projeto:
    "make clean"
    "make"

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

# CHECKPOINT 3

### Para subir os Super Peers no anel Chord, primeiro crie o arquivo de configuracao:
    echo "bootstrap=127.0.0.1:5101" > c3.conf

### Depois suba um Super Peer por terminal (portas 5101 a 5105):
    ./bin/superpeer --config c3.conf --port 5101
    ./bin/superpeer --config c3.conf --port 5102
    ...

### o nome do no e SP<porta> e o UUID fica salvo em superpeer_<porta>.uuid

### Para ver o estado de um Super Peer (successor, predecessor e finger table):
    ./bin/peer --cmd topology --host 127.0.0.1 --port 5101

### Para descobrir o dono de um ObjectID (mostra o Lookup path e o Owner):
    ./bin/peer --cmd lookup --object-id <64 caracteres hex> --host 127.0.0.1 --port 5101

# CHECKPOINT 4 — Eleição Bully e Replicação SMR

### Executar bateria automatizada do Checkpoint 4:
    bash tests/c4/run.sh

# CHECKPOINT 5 — Cache LFU, Transferência Paralela, 2PC e IST

### Executar bateria automatizada do Checkpoint 5:
    bash tests/c5/run.sh

### Consultar metadados ativando a Cache LFU:
    ./bin/peer --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101

# CHECKPOINT 6 — Integração e Teste Final (Aluno 1 + Aluno 2)

### Executar bateria completa de integração final (29 verificações, com derrubada de nós, IST, Bully e 2PC com falha):
    bash tests/c6/run.sh

### Upload, Lookup com Cache LFU e Download Paralelo Multi-Réplica:
    ./bin/peer --cmd upload --file tests/data/documento_c.pdf --host 127.0.0.1 --port 5101
    ./bin/peer --cmd lookup --name documento_c.pdf --host 127.0.0.1 --port 5102
    ./bin/peer --cmd download --name documento_c.pdf --output /tmp/saida.pdf --host 127.0.0.1 --port 5103

### Documentação do checkpoint:
    docs/cp6/CP6_Integracao_e_Teste_Final.md

### Cada Super Peer guarda as suas réplicas dos chunks em storage/<porta>/
