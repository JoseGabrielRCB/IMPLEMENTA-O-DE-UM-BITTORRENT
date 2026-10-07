## Arquitetura: Camada de Rede e Protocolo

a comunicação do P2P foi dividida em duas camadas principais, separando a infraestrutura TCP das regras de empacotamento.

### 1. network.c (a camada de transporte)

Este arquivo é o responsável exclusivo por lidar com a comunicação do sistema operacional e a internet. Ele não se importa com o conteúdo da mensagem que está sendo enviada, a sua única função é garantir a estabilidade do túnel TCP.

**Principais responsabilidades:**

* **`criar_servidor()`**: interage com o Sistema Operacional para reservar uma porta TCP, fazer o Bind e colocar o socket no modo Listen (escuta).
* **`aceitar_cliente()`**: aguarda e processa o handshake TCP de um novo par que está tentando se conectar à rede.
* **`conectar_no_servidor()`**: cria um socket e tenta estabelecer uma conexão com um IP/Porta distante.

### 2. protocol.c (A Camada de Aplicação / Framing)

o  `protocol.c` é o responsável por decidir como os pacotes (cargas) devem ser empacotados , etiquetados e validados.

**Principais responsabilidades:**

* **Empacotamento:** Transforma os dados em uma `struct` padrão contendo um Cabeçalho (com Versão, Tipo da Mensagem, Tamanho do Payload, e NodeID do remetente) e um Payload .
* **Validação de Corrupção (CRC32):** aplica algumas contas padronizadas sobre o conteudo do pacote enviado. Quando os bytes chegam, o receptor refaz o cálculo para garantir a integridade dos dados.

### 3. protocol.c (camada de identidade)

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
* **TRatamento das mensagens**: *tratar_join()* valida a versão do protocolo, fazendo o parse do payload e registra o nó  repondendo com um ACK para sucesso ou ERROR. *tratar_leave()* remove o nó respondendo um ACK e PING responde com PONG
* **STORE e LOOKUP (checkpoint 2)**: *tratar_store()* valida a versão e o no_origem, desserializa o metadado, confere os chunks e insere na hash table, respondendo ACK ou ERROR. *tratar_lookup()* busca pelo nome do arquivo e responde ACK com o metadado serializado no payload, ou ERROR se não achar. Qualquer outro tipo continua recebendo ERROR
* **Inicialização do servico** : Nota o servidor pode ser inicializado tanto posicional quanto por opções longas (como --port/--name...), isso de deve ao fato deque o aluno 2 so recebeu o arquivo de teste depois do terminao da sua parte, então na hora de juntarmnos os sistemas preferimos por manter ambas as funcionalidades para não perder nenhum trabalho.

### 5. metadata.c (camada de metadados - checkpoint 2)

O metadata.c guarda as informações de cada arquivo registrado na rede, é incluido pelo superpeer.c e pelo peer.c.

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
