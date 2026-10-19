## Arquitetura: Camada de Rede e Protocolo

a comunicação do P2P foi dividida em duas camadas principais, separando a infraestrutura TCP das regras de empacotamento.

### 1. network.c (a camada de transporte)

Este arquivo é o responsavel exclusivo por lidar com a comunicação do sistema operacional e a internet. Ele não se importa com o conteúdo da mensagem que esta sendo enviada, a sua única função é garantir a estabilidade do túnel TCP.

**Principais responsabilidades:**

* **`criar_servidor()`**: interage com o Sistema Operacional para reservar uma porta TCP, fazer o Bind e colocar o socket no modo Listen (escuta).
* **`aceitar_cliente()`**: aguarda e processa o handshake TCP de um novo par que esta tentando se conectar à rede.
* **`conectar_no_servidor()`**: cria um socket e tenta estabelecer uma conexão com um IP/Porta distante.

### 2. protocol.c (A Camada de Aplicação / Framing)

o  `protocol.c` é o responsavel por decidir como os pacotes (cargas) devem ser empacotados , etiquetados e validados.

**Principais responsabilidades:**

* **Empacotamento:** Transforma os dados em uma `struct` padrão contendo um Cabeçalho (com Versão, Tipo da Mensagem, Tamanho do Payload, e NodeID do remetente) e um Payload .
* **Validação de Corrupção (CRC32):** aplica algumas contas padronizadas sobre o conteudo do pacote enviado. Quando os bytes chegam, o receptor refaz o cálculo para garantir a integridade dos dados.

### 3. node.c (camada de identidade)

Arquivo responsavel por definir o que e um no antes de qualquer byte,montanado o processo e validando seu inicio

**Principais Responsabilidades**

* **node_load_uiid()**: Para ler o UUId dos 16 bytes do arquivos passado por parametro, senão ouver um gera um novo.
* **node_compute_id()**:calcular o NodeId de 32 bytes , utilizando a SHA256, para endereço logico da rede
* **node_parse_args/node_parse_ip/node_parse_port()**: fazem a validação da linha de comando
* **node_int()** [MAQUINA DE ESTADOS] : faz a montagem da struc Node e controlo o estados no enum, dependendo so seu status (DISCONNECTED → CONNECTING → CONNECTED → AUTHENTICATED), e também define o papel dizendo se é peer ou superpeer.

### 4. superpeer.c (camada de membros - para serviço)

O superpeer.c mantem a registrado quem entrou saiu e como varias conexões simultaneas são atendidas, sem perca de informação no processo.

**Principais Responsabilidades**

* **Tabela de membros** : Um vetor estático de MAX_MEMBERS , que gaurda NodeId,IP,porta,estadp, last_hertbeat e version.Com capacidade de adcionar,remover, mostrar dados sobre os membros e com retornos proprios. **Nota : Tranformar isso em  dinamico, por enquanto aceita quantidades limitadas de usuario**
* **Exclusao muta[PTHREAD_MUTEX]** : Foi utilizado um sistema de threas com mutex, para que as os joins simultaneos não travassem ou corrompessem os dados em conexão simultanea
* **Concorrencia** :Existe uma thread por cliente então se um peer demora para responder não interfere na conexão
* **TRatamento das mensagens**: *tratar_join()* valida a versão do protocolo, fazendo o parse do payload e registra o no  repondendo com um ACK para sucesso ou ERROR. *tratar_leave()* remove o no respondendo um ACK e PING responde com PONG
* **STORE e LOOKUP (checkpoint 2)**: *tratar_store()* valida a versão e o no_origem, desserializa o metadado, confere os chunks e insere na hash table, respondendo ACK ou ERROR. *tratar_lookup()* busca pelo nome do arquivo e responde ACK com o metadado serializado no payload, ou ERROR se não achar. Qualquer outro tipo continua recebendo ERROR
* **Inicialização do servico** : Nota o servidor pode ser inicializado tanto posicional quanto por opções longas (como --port/--name...), isso de deve ao fato deque o aluno 2 so recebeu o arquivo de teste depois do terminao da sua parte, então na hora de juntarmnos os sistemas preferimos por manter ambas as funcionalidades para não perder nenhum trabalho.

### 5. metadata.c (camada de metadados - checkpoint 2)

O metadata.c guarda as informações de cada arquivo registrado na rede, é usado pelo superpeer.c e pelo peer.c através do include/metadata.h.

**Principais Responsabilidades**

* **FileMetadata** : struct com object_id, filename, size, chunk_count, chunk_hashes, version (começa em 1) e owner (NodeId de quem registrou). *metadata_create()* aloca os hashes dos chunks, *metadata_free()* libera tudo, *metadata_copy()* faz uma copia completa e *metadata_print()* mostra File, Size, ObjectID, Chunks e o hash de cada chunk.
* **ObjectID** : *metadata_compute_object_id()* le o arquivo inteiro e calcula o SHA256 dele, antes de fragmentar e comprimir. Arquivo vazio ou inexistente retorna erro.
* **Validação dos chunks** : *metadata_check_chunks()* confere se chunk_count é igual a ceil(size / CHUNK_SIZE), com CHUNK_SIZE de 4 MiB. Um arquivo de 12458291 bytes da 3 chunks.
* **Hash table** : vetor fixo de MAX_FILES (256) com sondagem linear. O indice são os 4 primeiros bytes do ObjectID modulo MAX_FILES. A tabela guarda copias dos metadados.
* **Indice por nome** : uma segunda tabela fixa, tambem com sondagem linear, que leva o filename ao ObjectID usando a função djb2. É ele que permite o download pelo nome do arquivo.
* **Regras da inserção** : ObjectID repetido não duplica e responde sucesso (igual ao JOIN). Mesmo nome com ObjectID diferente retorna erro, e tabela cheia tambem.
* **Exclusao muta[PTHREAD_MUTEX]** : um mutex protege as duas tabelas em toda leitura e escrita.
* **Serialização** : *metadata_serialize()* e *metadata_deserialize()* transformam o metadado no payload, na ordem abaixo e sem padding. A desserialização confere os tamanhos antes de cada leitura, para não ler nada fora do buffer.

```
object_id[32] | filename_len (uint16) | filename (sem '\0') | size (uint64)
| chunk_count (uint32) | chunk_count * hash[32] | version (uint64) | owner[32]
```

### 6. peer.c (upload e download - checkpoint 2)

* **upload** : calcula o ObjectID, divide o arquivo em chunks de 4 MiB, comprime com LZ4, calcula o hash de cada chunk e salva em storage/. Depois monta o metadado e manda um STORE para o superpeer.
* **download** : manda um LOOKUP com o nome do arquivo, recebe o metadado de volta e remonta o arquivo a partir dos chunks do storage/.

### 7. chord.c (anel Chord entre os Super Peers - checkpoint 3)

O chord.c organiza os Super Peers num anel ordenado pelo NodeID, para que qualquer um deles consiga descobrir quem é o dono de um ObjectID.

**Principais Responsabilidades**

* **ChordNode** : struct com id[32], ip, porta e nome do no (o nome é o que aparece no log, ex.: SP5101). O estado global guarda self, successor, predecessor (com indicador de "sem predecessor") e a finger table de CHORD_FINGERS (256) entradas.
* **Exclusao muta[PTHREAD_MUTEX]** : um mutex protege o estado do anel, mas nunca fica travado durante chamada de rede. A ideia é copiar o que precisa, soltar o mutex, conversar com o outro no e travar de novo so para gravar o resultado.
* **Aritmetica do anel** : os IDs são comparados em big-endian com *node_id_compare()* (memcmp). *chord_add_pow2()* calcula self.id + 2^i com vai-um sobre os 32 bytes, e *chord_in_open()* / *chord_in_half()* testam os intervalos (a, b) e (a, b] com a volta do anel. Quando a == b o intervalo é o anel inteiro.
* **find_successor (iterativo)** : se o id esta em (self, successor] a resposta é o successor, senão pergunta ao *closest_preceding_node()*, que responde com o successor (achou) ou com o proximo no a perguntar. Quem iniciou guarda cada no visitado, que vira o "Lookup path". Tem limite de 32 saltos para não entrar em laço.
* **Join** : se não tem bootstrap ou o bootstrap é o proprio no, cria o anel (successor = self). Senão pede ao bootstrap o find_successor do proprio id, tentando de novo a cada 1 s até ele responder, e imprime JOIN <nome>.
* **Stabilize, Notify e Fix_fingers** : uma thread so roda a cada CHORD_PERIOD_SEC (1 s). O stabilize pergunta ao successor quem é o predecessor dele e manda NOTIFY, o notify atualiza o predecessor e o fix_fingers recalcula as 256 entradas. Successor, Predecessor e Finger table so são impressos quando mudam.
* **No que nao responde** : imprime "Chord: <nome> nao respondeu" e mantém o estado como esta.
* **Serialização do no** : *chord_node_write()* e *chord_node_read()*, sem padding, conferindo o tamanho antes de cada leitura (ip_len de 7 a 15 e name_len de 1 a 63).

```
id[32] | port (uint16) | ip_len (uint8) | ip (sem '\0') | name_len (uint8) | name (sem '\0')
```

### 8. gossip.c (membership dos Super Peers - checkpoint 3)

O gossip.c mantem a lista de quais Super Peers estão vivos, usando heartbeat e troca de tabelas (gossip), e detecta quando um deles cai.

**Principais Responsabilidades**

* **GossipMember** : guarda o ChordNode, o estado, o contador de heartbeat e o last_seen. A membership é um vetor fixo de GOSSIP_MAX (64) protegido por mutex, que também não fica travado durante a rede.
* **Heartbeat** : a cada GOSSIP_HEARTBEAT_SEC (5 s) o no incrementa o seu contador e manda HEARTBEAT para os membros ALIVE e SUSPECT.
* **Gossip** : na mesma rodada sorteia um membro vivo e troca a tabela inteira com ele (ou com o bootstrap enquanto ainda não conhece ninguem). *gossip_handle_gossip()* valida a tabela recebida, mescla com a local e responde com a sua.
* **Detecção de falha** : a cada 1 s *gossip_check_timeouts()* avança os estados ALIVE → SUSPECT (15 s sem heartbeat) → FAILED (30 s) → REMOVED (45 s). Se um no volta a mandar heartbeat ele retorna para ALIVE.

### 9. superpeer.c (atualizações do checkpoint 3)

* **Leitura do --config** : arquivo no formato chave=valor, com # para comentario. Aceita port, name e bootstrap, e chave desconhecida é ignorada. O --port e o --name da linha de comando valem mais que o arquivo. O tests/config/c3.conf tem so bootstrap=127.0.0.1:5101.
* **Nome e UUID** : o nome é o --name, senão o name do arquivo, senão SP<porta>. Com opções longas o UUID fica em superpeer_<porta>.uuid, para os 5 processos do teste não disputarem o mesmo arquivo.
* **Inicialização** : depois do *node_init()* chama *chord_start()* e *gossip_start()*, cada um com a sua thread periodica.
* **Mensagens novas** : *tratar_chord()* atende FIND_SUCCESSOR (21), GET_PREDECESSOR (22), NOTIFY (23), CHORD_LOOKUP (24) e TOPOLOGY (25), e *tratar_gossip()* atende HEARTBEAT e GOSSIP. As duas validam a versão do protocolo e o no_origem, e payload invalido recebe ERROR. Cada pedido entre Super Peers abre uma conexão, manda, recebe o ACK e fecha.
* **Provedores** : o STORE registra quem enviou como provedor do arquivo (*metadata_add_provider()*, até MAX_PROVIDERS), e a resposta do LOOKUP passa a levar, depois do metadado, um count + ProviderInfo (ip e porta) de cada provedor.

### 10. peer.c (comandos do checkpoint 3)

* **--cmd topology** : manda TOPOLOGY sem payload e imprime o texto que o Super Peer devolve (no, successor, predecessor e finger table).
* **--cmd lookup --object-id <hex>** : converte os 64 caracteres hex em 32 bytes, manda CHORD_LOOKUP e imprime o "Lookup path" e o "Owner".
* **download** : separa a lista de provedores do fim da resposta do LOOKUP antes de desserializar o metadado.

### 11. Divisão dos .h (pasta include/)

Antes cada binario era uma unidade de compilação so, com #include dos .c ,agora cada .c tem o seu .h com as structs, defines e as funções publicas, e o Makefile compila cada .c em um .o separado (build/) e depois liga os binarios.

* **protocol.h** : enum TipoMensagem (1 a 25), Header, Mensagem, ProviderInfo e as funções de empacotar e checksum.
* **network.h** : as funções de socket (criar_servidor, aceitar_cliente, conectar_no_servidor, enviar_mensagem, receber_mensagem).
* **node.h** : NodeConfig, Node, os enums de papel e estado e as funções de NodeID.
* **metadata.h** : FileMetadata, os codigos de retorno, a hash table e os provedores.
* **chord.h** : ChordNode, a serialização do no, *chord_start()* e os *chord_handle_*()* de cada mensagem.
* **gossip.h** : *gossip_start()* e os tratadores de HEARTBEAT e GOSSIP.

O superpeer usa todos (superpeer, node, protocol, network, metadata, chord e gossip) e o peer so node, protocol, network e metadata.
