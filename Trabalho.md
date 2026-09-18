# Trabalho: Implementação de um Sistema P2P Híbrido

**Universidade Estadual de Mato Grosso do Sul — UEMS**
**Curso de Ciência da Computação**
**Disciplina de Programação Distribuída**

**Professor:** Prof. Dr. Rubens Barbosa Filho
**Título oficial do trabalho:** *"TRABALHO: IMPLEMENTAÇÃO DE UM BITTORRENT"* `[C-13]`
**Local e data:** Dourados (MS), 09/09/2026
**Data final de entrega:** 06/11/2026

> **Sobre este documento.** Esta é uma transcrição integral e higienizada de `trabalho_2026_SD.pdf` (38 páginas). Todo o conteúdo técnico do original foi preservado. Foram removidos 18 blocos de texto oculto que não fazem parte da especificação (detalhes no **Apêndice A**), e as contradições internas do original foram resolvidas e marcadas com `[C-xx]`, rastreáveis no **Apêndice B**. Pontos que dependem de confirmação do professor estão no **Apêndice C**.

---

# ESPECIFICAÇÃO DE ARQUITETURA DE SOFTWARE DISTRIBUÍDO

## Sistema Distribuído P2P Híbrido para Compartilhamento Inteligente de Documentos

### Baseado em

- DHT-Chord
- Gossip Protocol
- SHA-256
- Hash Table Distribuída
- LFU Cache
- Compressão LZ4
- Bully Algorithm
- Two Phase Commit (2PC)
- State Machine Replication
- Incremental State Transfer

---

# 1. INTRODUÇÃO

## 1.1 Objetivo do Projeto

O presente documento especifica a arquitetura de um sistema distribuído Peer-to-Peer híbrido destinado ao compartilhamento eficiente de documentos digitais utilizando técnicas modernas de computação distribuída, algoritmos de consenso, replicação determinística e descoberta dinâmica de recursos.

Diferentemente das arquiteturas P2P tradicionais, que frequentemente sofrem com problemas de escalabilidade, disponibilidade e consistência, o sistema proposto combina uma rede estruturada baseada em DHT-Chord com uma camada hierárquica de Super Peers, proporcionando alta eficiência na localização de recursos e tolerância a falhas.

O documento descreve todos os componentes arquiteturais, protocolos de comunicação, estruturas de dados e mecanismos necessários para permitir que o sistema seja implementado de forma consistente, modular e escalável.

## 1.2 Motivação

O crescimento exponencial da produção de documentos digitais torna cada vez mais relevante a construção de sistemas distribuídos capazes de armazenar, localizar e compartilhar informações de maneira descentralizada.

Arquiteturas centralizadas apresentam limitações relacionadas à disponibilidade, escalabilidade horizontal e resiliência, uma vez que dependem da operação contínua de servidores específicos.

Sistemas P2P eliminam parte dessas limitações, porém introduzem novos desafios, como:

- descoberta eficiente de recursos;
- sincronização distribuída;
- manutenção de consistência;
- eleição de coordenadores;
- tolerância a falhas;
- gerenciamento de metadados;
- balanceamento de carga;
- recuperação após falhas.

Este projeto busca integrar soluções consolidadas para esses desafios em uma arquitetura única e coerente.

## 1.3 Escopo

O sistema será responsável por:

- cadastrar nós distribuídos;
- permitir entrada e saída dinâmica de participantes;
- localizar documentos através de uma DHT;
- transferir documentos comprimidos;
- replicar metadados;
- detectar falhas automaticamente;
- eleger novos coordenadores;
- manter consistência distribuída.

**Não faz parte do escopo:**

- interface gráfica;
- autenticação federada;
- armazenamento em nuvem pública;
- criptografia ponta a ponta.

## 1.4 Objetivos

### 1.4.1 Objetivo Geral

Projetar uma arquitetura distribuída capaz de fornecer um ambiente robusto para compartilhamento de documentos PDF utilizando tecnologias modernas de computação distribuída.

### 1.4.2 Objetivos Específicos

- desenvolver uma arquitetura híbrida P2P;
- reduzir tráfego de pesquisa;
- eliminar pontos únicos de falha;
- garantir integridade dos documentos;
- minimizar tempo de recuperação;
- maximizar disponibilidade;
- facilitar expansão horizontal.

## 1.5 Premissas

A arquitetura assume:

- comunicação TCP/IP confiável;
- relógios locais independentes;
- nós potencialmente heterogêneos;
- falhas por parada (*crash failures*);
- perda eventual de mensagens;
- possibilidade de particionamento temporário da rede.

## 1.6 Restrições

O projeto deverá:

- operar em ambiente Linux;
- utilizar linguagem C;
- empregar POSIX Sockets;
- manter independência de banco de dados relacional;
- suportar a arquitetura x86-64. `[C-09]`

## 1.7 Visão Geral da Arquitetura

A arquitetura é organizada em nove subsistemas principais: `[C-10]`

1. Cliente P2P
2. Super Peer
3. Overlay DHT-Chord
4. Gossip Manager
5. Metadata Manager
6. Consensus Manager
7. Replication Manager
8. Cache Manager
9. Compression Manager

Cada subsistema possui responsabilidades claramente definidas, reduzindo acoplamento e aumentando a modularidade.

---

# 2. REQUISITOS DO SISTEMA

## 2.1 Introdução

Esta seção especifica todos os requisitos funcionais, não funcionais, restrições arquiteturais e atributos de qualidade do sistema distribuído.

A arquitetura foi concebida para atender aos seguintes princípios:

- Alta disponibilidade;
- Escalabilidade horizontal;
- Consistência dos metadados;
- Tolerância a falhas;
- Balanceamento de carga;
- Modularidade;
- Baixo acoplamento;
- Alta coesão.

## 2.2 Atores do Sistema

O sistema possui quatro atores principais.

### Ator 1: Usuário

Responsável por:

- compartilhar documentos;
- pesquisar documentos;
- realizar download;
- remover documentos.

### Ator 2: Peer

Máquina participante da rede. Responsabilidades:

- armazenar documentos;
- responder pesquisas;
- enviar blocos;
- receber blocos;
- atualizar metadados.

### Ator 3: Super Peer

Nó responsável pelo gerenciamento de um domínio. Funções:

- manter índice local;
- gerenciar cache;
- executar Gossip;
- participar da DHT;
- executar replicação;
- executar consenso;
- participar da eleição.

### Ator 4: Coordenador

Coordenador lógico eleito dinamicamente pelo algoritmo Bully. Funções:

- coordenar transações 2PC;
- supervisionar replicação;
- registrar Super Peers;
- detectar inconsistências;
- iniciar recuperação.

## 2.3 Casos de Uso

### UC-01 — Registrar Peer

**Descrição:** Permite que um novo Peer entre na rede.

**Fluxo principal:**

1. Peer envia `JOIN`.
2. Gossip localiza Super Peer.
3. Super Peer valida o nó.
4. Nó recebe NodeID.
5. Finger Table é atualizada.
6. Metadados são sincronizados.

### UC-02 — Publicar Documento

**Fluxo:** `Upload → SHA-256 → Fragmentação → Compressão LZ4 → Hash Table → Replicação → Confirmação` `[C-06]`

### UC-03 — Pesquisar Documento

**Fluxo:** `Cliente → Super Peer → Hash Table → Chord Lookup → Localização → Resposta`

### UC-04 — Download

**Fluxo:** `Lookup → Lista de Peers → Transferência Paralela → Descompressão → Validação SHA-256 → Documento`

### UC-05 — Entrada de Novo Nó

**Etapas:**

- `JOIN`
- Gossip
- Finger Table
- Incremental State Transfer
- Atualização da DHT

### UC-06 — Saída de Nó

**Etapas:**

- `LEAVE`
- Redistribuição
- Atualização da Finger Table
- Atualização da Hash Table

### UC-07 — Falha

**Etapas:** `Heartbeat → Timeout → Gossip → Election → Recovery → Replicação`

## 2.4 Requisitos Funcionais

| ID | Descrição |
|---|---|
| RF-001 | Cadastrar novos Peers. |
| RF-002 | Cadastrar novos Super Peers. |
| RF-003 | Remover nós. |
| RF-004 | Compartilhar documentos PDF. |
| RF-005 | Gerar SHA-256 para cada documento. |
| RF-006 | Armazenar metadados em Hash Table. |
| RF-007 | Executar Lookup via DHT-Chord. |
| RF-008 | Executar compressão LZ4. |
| RF-009 | Executar descompressão automática. |
| RF-010 | Fragmentar documentos. |
| RF-011 | Transferência paralela. |
| RF-012 | Executar Gossip Protocol. |
| RF-013 | Atualizar Finger Tables. |
| RF-014 | Replicar metadados. |
| RF-015 | Executar State Machine Replication. |
| RF-016 | Executar Incremental State Transfer. |
| RF-017 | Executar Bully Algorithm. |
| RF-018 | Executar Two Phase Commit. |
| RF-019 | Executar Cache LFU. |
| RF-020 | Detectar falhas automaticamente. |
| RF-021 | Realizar balanceamento de carga. |
| RF-022 | Executar Heartbeats. |
| RF-023 | Executar recuperação automática. |
| RF-024 | Atualizar estatísticas. |
| RF-025 | Registrar logs distribuídos. |

## 2.5 Requisitos Não Funcionais

| ID | Atributo | Especificação |
|---|---|---|
| RNF-001 | Disponibilidade | Disponibilidade mínima: 99,5% |
| RNF-002 | Escalabilidade | Suportar crescimento sem necessidade de reconfiguração manual. |
| RNF-003 | Consistência | Garantir consistência forte para metadados. `[C-07]` |
| RNF-004 | Segurança | Validação SHA-256. |
| RNF-005 | Portabilidade | Linux x86-64. |
| RNF-006 | Modularidade | Cada componente será implementado como módulo independente. |
| RNF-007 | Desempenho | Lookup médio: O(log N) |
| RNF-008 | Replicação | Tempo máximo de sincronização incremental: < 2 segundos. |
| RNF-009 | Compressão | LZ4 com throughput superior a 500 MB/s. |
| RNF-010 | Recuperação | Eleição inferior a 5 segundos. `[C-16]` |

> **`[C-07]` Nota sobre RNF-003.** O original exige "consistência forte" e, ao mesmo tempo, emprega Gossip (eventualmente consistente por definição) e Version Vector (mecanismo para reconciliar escritas concorrentes, típico de modelos eventuais). Interpretação adotada: **consistência forte no caminho de escrita**, garantida por SMR + 2PC, cuja ordem total já é dada pelo `LogIndex`; o **Gossip é usado exclusivamente para *membership* e detecção de falha**, nunca para propagar metadados. Registre essa interpretação na documentação do grupo.

> **`[C-16]` Nota sobre RNF-010.** Detectar a falha do coordenador exige o Timeout de 15 s mais a confirmação via Gossip, o que leva a recuperação ponta a ponta a mais de 20 s. O requisito só é satisfeito se "eleição" for medida **a partir do disparo da mensagem `ELECTION`**, e não a partir do instante da falha. Deixe isso explícito na documentação.

## 2.6 Restrições Arquiteturais

O sistema deverá utilizar obrigatoriamente:

- linguagem C;
- POSIX Threads;
- POSIX Sockets;
- SHA-256;
- LZ4;
- Hash Tables;
- Chord;
- Gossip;
- Bully;
- 2PC;
- Incremental State Transfer;
- State Machine Replication.

## 2.7 Critérios de Aceitação

O sistema será considerado operacional quando:

- ✓ realizar upload;
- ✓ localizar documentos;
- ✓ realizar downloads;
- ✓ recuperar falhas;
- ✓ eleger novo líder;
- ✓ manter consistência;
- ✓ sincronizar novos nós.

---

# 3. ARQUITETURA GERAL DO SISTEMA

## 3.1 Visão Geral

O sistema adota uma arquitetura híbrida composta por:

- Overlay DHT-Chord;
- Super Peers;
- Gossip Protocol;
- Coordenador dinâmico;
- Replicação determinística.

Cada componente possui responsabilidades específicas.

## 3.2 Arquitetura Física

```
                    CLIENTES
                       │
        ┌──────────────┴──────────────┐
        │                             │
    Cliente A                     Cliente B
        │                             │
        └──────────────┬──────────────┘
                       │
                   Super Peer
                       │
          ┌────────────┴────────────┐
          │                         │
    Chord Overlay              Gossip Layer
          │                         │
          ├────────────┬────────────┤
          │            │            │
      Metadata    Replication   Consensus
          │
     Compression
          │
       Storage
```

## 3.3 Arquitetura em Camadas

```
Application Layer
      ↓
Service Layer
      ↓
Metadata Layer
      ↓
Replication Layer
      ↓
Consensus Layer
      ↓
Compression Layer
      ↓
Network Layer
      ↓
TCP/IP
```

Cada camada abstrai os detalhes da inferior, promovendo modularidade e facilitando manutenção e evolução.

## 3.4 Componentes Arquiteturais

> **`[C-10]`** A Seção 1.7 fala em **nove subsistemas**; a lista abaixo tem **dez entradas** porque inclui o **Coordenador**. Não há contradição real: o Coordenador é um **papel eleito dinamicamente** que um Super Peer assume, não um décimo subsistema com código próprio. A contagem oficial permanece nove.

### Cliente

Responsabilidades:

- upload;
- download;
- fragmentação;
- recomposição;
- pesquisa;
- validação SHA-256;
- comunicação com Super Peer.

### Super Peer

Responsabilidades:

- Finger Table;
- Lookup;
- Hash Table;
- Gossip;
- Cache LFU;
- Replicação;
- Consenso.

### Coordenador

Responsabilidades:

- iniciar 2PC;
- supervisionar State Machine Replication;
- controlar replicação;
- coordenar eleição;
- controlar/sincronizar logs.

### DHT Manager

Responsável por:

- Join;
- Leave;
- Lookup;
- Stabilize;
- Notify.

### Gossip Manager

Responsável por:

- Membership;
- Heartbeats;
- Disseminação;
- Failure Detection.

### Metadata Manager

Gerencia:

- índices;
- Hash Table;
- estatísticas;
- versões.

### Replication Manager

Executa:

- SMR;
- Snapshot;
- Incremental Transfer.

### Consensus Manager

Executa:

- Prepare;
- Commit;
- Abort;
- Recovery.

### Compression Manager

Executa:

- LZ4 Compress;
- LZ4 Decompress.

### Cache Manager

Gerencia:

- LFU;
- contadores;
- substituição.

## 3.5 Modelo de Dados

Cada documento é representado por um objeto distribuído:

```
Documento → SHA-256 → ObjectID → Metadata Entry → Hash Table → Replicação
```

## 3.6 Modelo de Comunicação

Todas as mensagens seguem uma estrutura padronizada.

```
+--------------------------------------------------+
| HEADER                                           |
+--------------------------------------------------+
| Protocol Version                                 |
| Message Type                                     |
| Source Node                                      |
| Destination Node                                 |
| Transaction ID                                   |
| Timestamp                                        |
| Payload Size                                     |
| Checksum                                         |
+--------------------------------------------------+
| PAYLOAD                                          |
+--------------------------------------------------+
```

## 3.7 Fluxo Geral do Sistema

```
Upload → SHA-256 → Fragmentação → Compressão → Hash Table → Chord →
Replicação → SMR → 2PC → Commit → Disponibilização
```
`[C-06]`

## 3.8 Fluxo de Pesquisa

```
Cliente → Super Peer → Cache LFU → Hash Table → Chord Lookup → Peer Encontrado → Download
```

## 3.9 Fluxo de Recuperação

```
Heartbeat → Timeout → Gossip → Election → Novo Coordenador → State Transfer → Sistema Restaurado
```

## 3.10 Fluxo de Entrada de Novos Nós

```
JOIN → Validação → NodeID → Finger Table → Incremental State Transfer →
Atualização Hash Table → Operacional
```

## 3.11 Fluxo de Replicação

```
Evento → State Machine → Log → 2PC → Commit → Snapshot → Replicação Incremental → Confirmação
```

## 3.12 Interação entre Componentes

```
Cliente
   │
   ▼
Super Peer
   │
   ├────────► DHT
   │
   ├────────► Gossip
   │
   ├────────► Metadata
   │
   ├────────► Cache
   │
   ├────────► Compression
   │
   ├────────► Consensus
   │
   └────────► Replication
```

---

# 4. ARQUITETURA P2P HÍBRIDA

## 4.1 Objetivo

A arquitetura proposta combina características das arquiteturas Peer-to-Peer estruturadas e hierárquicas.

Seu objetivo é reunir:

- eficiência de localização;
- escalabilidade;
- tolerância a falhas;
- consistência distribuída;
- simplicidade operacional.

A rede é organizada em dois níveis:

- **Peers (Clientes):** armazenam documentos e participam da transferência de arquivos.
- **Super Peers:** mantêm metadados, executam protocolos distribuídos e formam o overlay Chord.

## 4.2 Modelo Arquitetural

```
                          CLIENTES

     P1       P2       P3       P4       P5
       \       \       |       /       /
        \       \      |      /       /
         └───────┴─────┴─────┴───────┘
                       │
                 Super Peer A
                       │
      ============= DHT CHORD =============
                       │
                 Super Peer B
                       │
         ┌───────┬─────┬─────┬───────┐
        /       /      |      \       \
     P6       P7       P8      P9      P10
```

Cada Super Peer administra um domínio de clientes, enquanto todos os Super Peers participam do anel lógico Chord.

---

# 5. MODELO DE COMUNICAÇÃO DISTRIBUÍDA

Todos os componentes comunicam-se utilizando mensagens padronizadas sobre TCP/IP.

## 5.1 Tipos de Mensagem

| Código | Descrição |
|---|---|
| `JOIN` | Entrada |
| `LEAVE` | Saída |
| `LOOKUP` | Pesquisa |
| `STORE` | Registro |
| `DOWNLOAD_REQ` | Solicitação |
| `DOWNLOAD_REP` | Resposta |
| `PREPARE` | 2PC |
| `COMMIT` | 2PC |
| `ABORT` | 2PC |
| `HEARTBEAT` | Monitoramento |
| `GOSSIP` | Disseminação |
| `ELECTION` | Bully |
| `OK` | Resposta eleição |
| `COORDINATOR` | Novo líder |
| `SNAPSHOT` | Replicação |
| `STATE_TRANSFER` | Sincronização |
| `ACK` | Confirmação |
| `ERROR` | Erro |

## 5.2 Estados de Comunicação

```
DISCONNECTED → CONNECTING → CONNECTED → AUTHENTICATED → SYNCHRONIZED → ACTIVE → LEAVING
```
`[C-08]`

> **`[C-08]` Nota sobre `AUTHENTICATED`.** O estado existe na máquina de estados, mas o documento não especifica nenhum mecanismo de autenticação: a autenticação federada está **fora do escopo** (Seção 1.3) e o RNF-004 "Segurança" cobre apenas validação SHA-256, que é **integridade**, não autenticação. Implemente `AUTHENTICATED` como uma transição trivial — um *handshake* que valida o NodeID e a versão do protocolo. **Não implemente TLS, JWT ou troca de chaves**: está fora do escopo e não será avaliado.

## 5.3 Heartbeats

Cada Super Peer transmite Heartbeats periodicamente.

- **Intervalo:** 5 segundos
- **Timeout:** 15 segundos

Após o timeout:

- Gossip propaga a suspeita.
- Bully Election é iniciado.

## 5.4 Controle de Transações

Cada transação recebe um `TransactionID` único de 128 bits.

**Formato:** `Timestamp + NodeID + Sequence Number` `[C-02]`

> **`[C-02]` Nota sobre o TransactionID.** Como especificado, o campo é impossível: o `NodeID` é um SHA-256 de **256 bits** (Seção 6.3) e sozinho já excede os 128 bits do `TransactionID`, sem contar `Timestamp` e `Sequence Number`. Resolução sugerida, a ser registrada na documentação do grupo:
>
> ```
> TransactionID (128 bits) = Timestamp (48 bits)
>                          | NodeID truncado — 64 bits mais significativos
>                          | Sequence Number (16 bits)
> ```
>
> A alternativa é ampliar o campo para 256+ bits. Qualquer das duas serve; o que não pode é deixar a contradição sem decisão registrada.

## 5.5 Controle de Integridade

Cada mensagem possui:

- **CRC32** — integridade da mensagem
- **SHA-256** — integridade do conteúdo

---

# 6. PROJETO DA DHT-CHORD

## 6.1 Objetivo

Eliminar pesquisas por inundação. Cada documento será localizado através de *hashing* consistente.

## 6.2 Espaço de Identificadores

O sistema utiliza SHA-256. Portanto, o espaço é `0 → 2²⁵⁶ − 1`. Todos os nós pertencem ao mesmo espaço.

> **Nota de implementação.** Um espaço de `2²⁵⁶` implica, na formulação canônica do Chord, uma Finger Table de **256 entradas por nó**, com comparações de inteiros de 256 bits — desproporcional para uma rede de ~5 Super Peers. Implemente a comparação com `memcmp` sobre `uint8_t[32]` em *big-endian* e preencha apenas as primeiras `k` fingers úteis. Documentar essa otimização demonstra compreensão do algoritmo.

## 6.3 NodeID

Cada Super Peer recebe:

```
NodeID = SHA256(IP || Porta || UUID)
```
`[C-14]`

> **`[C-14]` Nota crítica sobre o UUID.** O `UUID` **precisa ser gerado uma única vez e persistido em disco**. Se for regenerado a cada inicialização, o `NodeID` muda — e com ele mudam simultaneamente a **prioridade na eleição Bully** (Seção 15.2) e a **posição do nó no anel Chord**, invalidando toda a Finger Table e a distribuição de chaves. Isso quebra frontalmente a recuperação descrita na Seção 15.7 ("quando um antigo coordenador retorna, entra como FOLLOWER e sincroniza via IST"), porque ele voltaria como um nó inteiramente diferente, e torna o teste do Checkpoint 5 (derrubar o SP2, religar, IST) impossível de passar.

## 6.4 ObjectID

Cada documento recebe:

```
ObjectID = SHA256(Arquivo)
```

O SHA-256 é calculado sobre o **arquivo original**, antes da fragmentação e da compressão.

## 6.5 Lookup

```
Lookup(ObjectID) → Finger Table → Closest Preceding Node → Forward → Successor → Owner
```

## 6.6 Operações Fundamentais

| Operação | Função |
|---|---|
| `JOIN` | Entrada de novo Super Peer. |
| `LEAVE` | Saída ordenada. |
| `STABILIZE` | Atualização periódica. |
| `NOTIFY` | Atualização do predecessor. |
| `FIX_FINGERS` | Atualização das Finger Tables. |

## 6.7 Redistribuição

- Quando um Super Peer **entra**: recebe parte das chaves.
- Quando um Super Peer **sai**: transfere todas.

---

# 7. GOSSIP PROTOCOL

## 7.1 Objetivo

O Gossip Protocol mantém o conhecimento global da rede sem necessidade de um servidor central.

## 7.2 Serviços

- descoberta;
- heartbeat;
- disseminação;
- detecção de falhas;
- atualização de estado.

## 7.3 Membership

Cada Super Peer mantém uma **Membership Table** com:

- NodeID
- IP
- Port
- State
- LastHeartbeat
- Version

## 7.4 Ciclo Gossip

```
Seleciona Vizinho → Envia Estado → Recebe Estado → Mescla Informações → Atualiza Membership
```

## 7.5 Detecção de Falhas

```
Heartbeat → Timeout → Suspect → Gossip → Confirm → Remove Node
```

## 7.6 Estados

```
ALIVE → SUSPECT → FAILED → REMOVED
```

## 7.7 Integração com Bully

Quando o Gossip confirma uma falha do coordenador:

```
FAILED → Election → Bully → Coordinator → Broadcast
```

## 7.8 Integração com Chord

Quando ocorre entrada, saída ou falha, o Gossip solicita:

```
Fix Fingers + Stabilize + Notify
```

mantendo a DHT consistente.

---

# 8. BANCO DE METADADOS DISTRIBUÍDO

## 8.1 Objetivo

O Banco de Metadados Distribuído (*Distributed Metadata Repository* — DMR) constitui o principal mecanismo de indexação do sistema. Sua função é armazenar apenas informações descritivas sobre os documentos, mantendo a localização lógica dos recursos sem armazenar os arquivos propriamente ditos.

Cada Super Peer mantém uma réplica consistente do subconjunto de metadados sob sua responsabilidade, determinado pelo particionamento da DHT-Chord.

## 8.2 Arquitetura Lógica

```
                    Documento PDF
                          │
                    Cálculo SHA-256
                          │
                  ObjectID (256 bits)
                          │
                 Hash Table Distribuída
                          │
              ┌───────────┴───────────┐
              │                       │
         Metadados              Localização
              │                       │
        Super Peer A            Super Peer B
```

## 8.3 Estrutura da Entrada de Metadados

Cada documento é representado por um registro estruturado:

| Campo | Tipo | Descrição |
|---|---|---|
| `ObjectID` | SHA-256 | Identificador único |
| `Filename` | String | Nome lógico |
| `Version` | uint64 | Versão do objeto |
| `Size` | uint64 | Tamanho em bytes |
| `Compression` | Enum | LZ4 |
| `OwnerPeer` | NodeID | Proprietário original |
| `ReplicaPeers` | Lista | Réplicas |
| `ChunkCount` | uint32 | Número de fragmentos |
| `ChunkHash[]` | SHA-256[] | Integridade dos blocos |
| `UploadDate` | Timestamp | Data de publicação |
| `LastAccess` | Timestamp | Último acesso |
| `DownloadCounter` | uint64 | Estatística |
| `Status` | Enum | Ativo, Replicando, Removido |

> **Esta tabela de 13 campos é o modelo final.** A `struct` C apresentada no Checkpoint 2 (Seção 16.7) implementa apenas o subconjunto necessário àquele checkpoint. Ver `[C-05]`.

## 8.4 Operações

### Inserção

1. Calcular SHA-256.
2. Calcular índice.
3. Inserir registro.
4. Replicar operação via SMR.
5. Confirmar via 2PC.

### Consulta

```
Lookup → Hash Table → Encontrou?
                        ├── Sim → Retorna Registro
                        └── Não → Consulta DHT
```

### Atualização

As atualizações são versionadas utilizando Version Vector. Cada modificação incrementa:

```
Version++
```

### Remoção

A remoção lógica marca o registro como `REMOVED`.

## 8.5 Índices Auxiliares

Além da chave principal (`ObjectID`), cada Super Peer mantém índices secundários por:

- Nome do documento. `[C-17]`
- Proprietário.
- Data de publicação.
- Número de downloads.
- Palavras-chave.
- Categoria.

Esses índices aceleram consultas locais.

> **`[C-17]` O índice por nome é obrigatório, não opcional.** O `ObjectID` é `SHA256(Arquivo)` — para calculá-lo é preciso **já possuir o arquivo**. Mas o item de verificação do Checkpoint 2 exige baixar **pelo nome** (`./peer download arquivo.pdf`), e o Lookup da DHT opera **por ObjectID**. O fluxo real de download é, portanto:
>
> ```
> nome do arquivo → índice secundário do Super Peer → ObjectID → Chord Lookup → lista de peers
> ```
>
> **Sem o índice por nome, o item de verificação do Checkpoint 2 é inexecutável.**

## 8.6 Política de Replicação

Cada entrada possui fator de replicação configurável.

**Exemplo:** `ReplicationFactor = 3`

O sistema tenta manter três réplicas consistentes em Super Peers distintos.

---

# 9. CACHE DISTRIBUÍDO LFU

## 9.1 Objetivo

O Cache Manager reduz o número de consultas à DHT e ao Banco de Metadados, armazenando temporariamente registros frequentemente acessados.

## 9.2 Arquitetura

```
Lookup → LFU Cache → Hit?
                      ├── Sim → Retorna
                      └── Não → Hash Table → Atualiza Cache
```

## 9.3 Algoritmo LFU

Cada acesso incrementa:

```
Frequency++
```

Ao atingir o limite do cache, a vítima é escolhida por:

```
Menor Frequência → (desempate) Menor Último Acesso → Remover Entrada
```

## 9.4 Balanceamento

O cache é dividido em regiões com capacidade independente:

- Metadata Cache
- Lookup Cache
- Finger Cache
- Routing Cache

## 9.5 Atualização

Quando o Gossip dissemina alterações:

```
Invalidate → Atualizar → Replicar
```

## 9.6 Métricas

- Hit Rate
- Miss Rate
- Tempo Médio
- Número de Evictions

---

# 10. SISTEMA DE COMPRESSÃO LZ4

## 10.1 Objetivo

Reduzir o volume de dados transmitidos na rede sem comprometer o desempenho.

> **Nota de implementação (RNF-009).** O requisito de throughput superior a 500 MB/s é alcançável pelo LZ4 real, mas **apenas com a biblioteca oficial** (`liblz4`, modo *fast*) e medindo somente a compressão, isolada do I/O de rede. Use `liblz4` (`-llz4`); não implemente o algoritmo LZ4 do zero.

## 10.2 Pipeline

```
Arquivo → Chunking → Compressão LZ4 → Checksum → Transmissão
```

## 10.3 Processo de Compressão

Cada fragmento é comprimido **individualmente**:

```
Chunk → LZ4 Compress → Compressed Chunk
```

## 10.4 Processo de Descompressão

```
Compressed Chunk → LZ4 Decode → SHA-256 → Validação → Entrega
```

## 10.5 Compressão Paralela

O Compression Manager utiliza um *pool* de threads:

```
Chunk 1 → Thread 1
Chunk 2 → Thread 2
Chunk 3 → Thread 3
   ...
Chunk N → Thread N
```

---

# 11. SISTEMA DISTRIBUÍDO DE TRANSFERÊNCIA DE ARQUIVOS

## 11.1 Objetivo

Permitir transferência paralela, segura e tolerante a falhas.

## 11.2 Fragmentação

Os documentos são divididos em blocos de tamanho fixo.

```c
#define CHUNK_SIZE (4 * 1024 * 1024)   /* 4 MiB = 4.194.304 bytes */
```
`[C-01]`

**Estrutura:**

```
Documento → Chunk0 → Chunk1 → Chunk2 → … → ChunkN
```

> **`[C-01]` "4 MB" significa 4 MiB — e o próprio enunciado prova isso.** O item de verificação do Checkpoint 2 exige que um arquivo de `12458291 bytes` produza exatamente **3 chunks**:
>
> | Interpretação | Cálculo | Chunks |
> |---|---|---|
> | **4 MiB = 4.194.304** | ⌈12.458.291 / 4.194.304⌉ = ⌈2,97⌉ | **3** ✅ |
> | 4 MB decimal = 4.000.000 | ⌈12.458.291 / 4.000.000⌉ = ⌈3,11⌉ | 4 ❌ |
>
> Quem usar `4000000` produz 4 chunks e falha na verificação **sem nenhuma mensagem de erro que indique a causa**.

## 11.3 Identificação

Cada fragmento recebe:

```
ChunkID + SHA-256 + Offset
```
`[C-15]`

> **`[C-15]` Nota sobre o `Offset`.** Os chunks são comprimidos com LZ4 e passam a ter **tamanho variável**, o que torna o campo ambíguo: o offset no arquivo original é previsível (`i * CHUNK_SIZE`), mas no fluxo comprimido não é. Guarde **os dois valores**:
>
> - `offset_original` — posição no arquivo descomprimido, usada para remontar;
> - `compressed_size` — bytes a ler do *wire*.
>
> O `SHA-256` de cada chunk deve ser calculado sobre o conteúdo **descomprimido**, para que a validação sobreviva a uma eventual troca do algoritmo de compressão.

## 11.4 Upload

```
Cliente → SHA-256 → Chunking → Compressão → Metadata → Replicação → Commit
```

## 11.5 Download

```
Lookup → Lista de Peers → Transferência Paralela → Descompressão → Validação → Reconstrução
```

## 11.6 Recuperação

Caso um Peer falhe durante a transferência:

```
Timeout → Selecionar Réplica → Retomar Download
```

## 11.7 Controle de Integridade

Cada fragmento possui SHA-256 próprio. Ao final:

```
SHA256(Documento) → Comparação → Documento Válido
```

## 11.8 Estados da Transferência

```
CREATED → QUEUED → STARTED → TRANSFERRING → VERIFYING → FINISHED → REPLICATED
```

---

# 12. STATE MACHINE REPLICATION (SMR)

## 12.1 Objetivo

A *State Machine Replication* garante que todos os Super Peers mantenham exatamente o mesmo estado lógico do banco de metadados distribuído. Cada operação aprovada é registrada em um log e executada na mesma ordem por todas as réplicas. Dessa forma, qualquer Super Peer pode assumir o papel de coordenador sem perda de consistência.

## 12.2 Arquitetura da Replicação

```
Cliente
   │
   ▼
Super Peer Primário
   │
   ▼
Registro em Log
   │
   ▼
Two-Phase Commit
   │
   ▼
Replicação para Réplicas
   │
   ▼
Execução Determinística
   │
   ▼
Estado Consistente
```

## 12.3 Log Distribuído

Cada comando confirmado gera um registro:

| Campo | Descrição |
|---|---|
| `LogIndex` | Sequência global |
| `TransactionID` | Identificador único |
| `Operation` | `STORE`, `DELETE`, `UPDATE` |
| `ObjectID` | Documento |
| `Timestamp` | Tempo lógico |
| `Version` | Número da versão |
| `Checksum` | SHA-256 do comando |

Os logs são mantidos em ordem crescente de `LogIndex`.

## 12.4 Máquina de Estados

```
INITIALIZING
     │
     ▼
SYNCHRONIZING
     │
     ▼
   READY
     │
     ▼
REPLICATING
     │
     ▼
 COMMITTED
     │
     ▼
CHECKPOINTING
```

---

# 13. INCREMENTAL STATE TRANSFER (IST)

## 13.1 Objetivo

Evitar a cópia integral do banco de metadados sempre que um novo Super Peer entrar ou um nó recuperar-se de falha.

O mecanismo transfere apenas:

- logs pendentes;
- diferenças de estado;
- checkpoints recentes.

## 13.2 Fluxo

```
Novo Super Peer
      │
      ▼
Solicita Estado
      │
      ▼
Recebe Último Checkpoint
      │
      ▼
Recebe Logs Posteriores
      │
      ▼
Aplica Atualizações
      │
      ▼
Entra em Produção
```

## 13.3 Sincronização

O coordenador identifica a versão do nó solicitante.

**Exemplo:**

```
Coordenador: versão 1050
Novo nó:     versão 1032

O sistema envia apenas as operações de 1033 a 1050.
```

---

# 14. TWO-PHASE COMMIT (2PC)

## 14.1 Objetivo

Garantir que operações críticas sejam executadas de forma consistente em todos os Super Peers.

**Operações protegidas:**

- inserção de documento;
- atualização de metadados;
- remoção lógica;
- criação de snapshots.

## 14.2 Participantes

- Coordenador.
- Participantes (Super Peers).

## 14.3 Estados

```
IDLE → PREPARING → READY → COMMIT → DONE
                     └───→ ABORT
```

## 14.4 Fase 1 — Prepare

O coordenador envia:

```
PREPARE(TransactionID)
```

Cada participante:

- valida operação;
- grava em log;
- responde `YES` ou `NO`.

## 14.5 Fase 2 — Commit

Se **todos** responderem `YES`:

```
COMMIT(TransactionID)
```

Caso contrário:

```
ABORT(TransactionID)
```

## 14.6 Recuperação

Cada participante registra em log:

- `PREPARE`;
- `COMMIT`;
- `ABORT`.

Após falha, o estado é reconstruído a partir do log.

## 14.7 Timeout

Se o coordenador falhar durante o `PREPARE`:

```
Timeout
   │
   ▼
Election (Bully)
   │
   ▼
Novo Coordenador
   │
   ▼
Consulta Logs
   │
   ▼
Decisão Final
```

`[C-18]`

> **`[C-18]` O 2PC é bloqueante — implemente assim mesmo e registre a limitação.** O procedimento acima resolve a maioria dos casos, mas o 2PC tem uma janela de bloqueio comprovada: se o coordenador falha **depois** de decidir e **antes** de comunicar a decisão, e o único participante que a recebeu também cai, os demais ficam bloqueados indefinidamente. É exatamente por isso que existem 3PC, Paxos e Raft.
>
> **Não tente "consertar" o protocolo.** Implemente o 2PC como especificado e cite a limitação na documentação — o Checkpoint 5 pede explicitamente para forçar uma falha durante a fase `PREPARE`, e reconhecer o *blocking problem* é o conhecimento que está sendo avaliado ali.

## 14.8 Fluxo Completo

```
Cliente
   │
 STORE
   │
   ▼
Coordenador
   │
 PREPARE
   │
   ▼
Participantes
   │
 YES / NO
   │
   ▼
COMMIT ou ABORT
   │
   ▼
Atualização SMR
```

---

# 15. BULLY ALGORITHM E RECUPERAÇÃO DO COORDENADOR

## 15.1 Objetivo

Eleger automaticamente um novo coordenador quando o atual falhar.

## 15.2 Identificação

Cada Super Peer possui prioridade baseada em:

```
Priority = NodeID
```

O **maior** NodeID vence a eleição. Ver `[C-14]` (Seção 6.3) sobre a necessidade de persistir o UUID.

## 15.3 Estados

```
FOLLOWER
    │
    ▼
CANDIDATE
    │
    ▼
COORDINATOR
```

## 15.4 Processo de Eleição

```
Timeout
   │
   ▼
Election
   │
   ▼
Mensagens para nós superiores
   │
   ▼
Resposta?
   ├── Sim → Aguardar Coordenador
   └── Não → Assume Coordenação
```

## 15.5 Mensagens

| Mensagem | Função |
|---|---|
| `ELECTION` | Inicia eleição |
| `OK` | Existe nó superior |
| `COORDINATOR` | Novo líder |
| `HEARTBEAT` | Monitoramento |

## 15.6 Heartbeats

Configuração sugerida:

```
Heartbeat = 5 s
Timeout   = 15 s
```

## 15.7 Recuperação do Coordenador Antigo

Quando um antigo coordenador retorna:

1. entra como `FOLLOWER`;
2. sincroniza via IST;
3. recebe estado atualizado;
4. só participa de nova eleição se ocorrer outra falha.

Isso evita interrupções desnecessárias.

## 15.8 Integração com Gossip

O Gossip confirma a suspeita de falha antes da eleição, reduzindo eleições provocadas por atrasos temporários na rede.

## 15.9 Integração com SMR

Após a eleição:

1. o novo coordenador verifica checkpoints;
2. aplica logs pendentes;
3. inicia novos ciclos de 2PC;
4. retoma a replicação.

## 15.10 Fluxo Integrado de Recuperação

```
Heartbeat Perdido
       │
       ▼
    Timeout
       │
       ▼
Gossip Confirma Falha
       │
       ▼
  Bully Election
       │
       ▼
 Novo Coordenador
       │
       ▼
Carrega Checkpoint
       │
       ▼
   Aplica Logs
       │
       ▼
Incremental State Transfer
       │
       ▼
Sistema Operacional
```

---
---

# 16. INSTRUÇÕES E METODOLOGIA PARA O DESENVOLVIMENTO DO TRABALHO

## 16.1 Formação do grupo

**1)** O trabalho deverá ser feito **em dupla**.

## 16.2 Uso de Inteligência Artificial

**2)** **Pode usar IA.**

## 16.3 Duração

**3)** O tempo de duração do trabalho é de **8 semanas**.

## 16.4 Datas dos checkpoints

**4)** A cada checkpoint uma parte do trabalho deverá ser apresentada. As datas de checkpoint são:

| Checkpoint | Data |
|---|---|
| Checkpoint 1 | **18/09** |
| Checkpoint 2 | **30/09** |
| Checkpoint 3 | **09/10** |
| Checkpoint 4 | **21/10** |
| Checkpoint 5 | **30/10** |
| Checkpoint 6 | **06/11** |

## 16.5 Divisão do trabalho

**5)** Como são dois alunos, a divisão do trabalho será de acordo com a tabela abaixo para que não sobrecarregue um aluno apenas. E, desta forma, no dia do checkpoint, **apenas o aluno responsável pela sua parte deverá apresentar**.

| Área | Aluno 1 | Aluno 2 |
|---|---|---|
| Protocolo de mensagens | Responsável | Apoio |
| TCP/Sockets | Responsável | Apoio |
| Cliente P2P | Responsável | — |
| Upload/download | Responsável | Apoio |
| Fragmentação | Responsável | — |
| SHA-256 | Responsável | — |
| LZ4 | Responsável | — |
| Armazenamento de arquivos | Responsável | — |
| LFU Cache | Responsável | — |
| Metadata | Apoio | Responsável |
| Hash table | Apoio | Responsável |
| Super Peer | Apoio | Responsável |
| Chord | — | Responsável |
| Finger Table | — | Responsável |
| Gossip | — | Responsável |
| Heartbeat | — | Responsável |
| Detecção de falhas | — | Responsável |
| Bully | — | Responsável |
| SMR | — | Responsável |
| 2PC | — | Responsável |
| IST | — | Responsável |
| Integração | 50% | 50% |
| Testes | 50% | 50% |
| Documentação | 50% | 50% |

`[P-01]`

## 16.6 CHECKPOINT 1 — 18/09

**6)** Deverá estar implementado:

- processo distribuído;
- socket TCP;
- cliente/servidor;
- serialização;
- framing de mensagens;
- concorrência básica;
- identificação de nós.

### Aluno 1 implementa

```
network.c
protocol.c
peer.c
```

Esses três códigos deverão realizar:

- criação do socket;
- `bind`;
- `listen`;
- `accept`;
- `connect`;
- `send`;
- `recv`;
- framing;
- header;
- checksum.

### Aluno 2 implementa

```
node.c
superpeer.c
```

Esses dois códigos deverão realizar:

- NodeID;
- configuração;
- identificação do processo;
- registro do nó;
- tabela básica de membros;
- criação do Super Peer.

### Itens de verificação

- conexão TCP funcionando;
- mensagem chega corretamente;
- header é interpretado;
- checksum é validado;
- dois processos podem conversar;
- **não pode ocorrer *segmentation fault*.**

> **Dica de compilação.** O último item é um critério de aceitação explícito. Compile sempre com *sanitizers* — eles apanham estouro de buffer no momento em que acontece:
>
> ```bash
> gcc -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g -O1 *.c -llz4 -lpthread -lcrypto
> ```

## 16.7 CHECKPOINT 2 — 30/09

**7)**

### Aluno 1 é responsável por

- upload
- download
- storage
- chunking
- SHA-256
- LZ4
- *parallel transfer*

E pelo pipeline:

```
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
```

### Aluno 2 é responsável por

- metadata
- hash table
- ObjectID
- registro dos chunks

### Estrutura em C do metadado

```c
typedef struct {
    uint8_t   object_id[32];
    char      filename[256];
    uint64_t  size;
    uint32_t  chunk_count;
    uint8_t **chunk_hashes;
    uint64_t  version;      /* [C-03] */
    uint8_t   owner[32];    /* [C-04] */
} FileMetadata;
```

> **`[C-03]` `version`.** A tabela de metadados (Seção 8.3) define `Version` como `uint64`; a `struct` original do enunciado trazia `uint32_t`. Unificado em `uint64_t`.
>
> **`[C-04]` `owner`.** A tabela define `OwnerPeer` como um `NodeID`, que é um SHA-256 de **256 bits**. Um `uint32_t` não comporta esse valor. Alterado para `uint8_t owner[32]`. Manter `uint32_t` aqui quebraria o Chord e o Bully mais adiante, quando o `owner` precisar ser comparado com NodeIDs reais.
>
> **`[C-05]` Campos ausentes.** Esta `struct` é o **mínimo do Checkpoint 2**, não o modelo final: implementa 7 dos 13 campos da Seção 8.3. Faltam `Compression`, `ReplicaPeers`, `UploadDate`, `LastAccess`, `DownloadCounter` e `Status`, que devem ser acrescentados nos checkpoints seguintes. Já deixe a `struct` preparada para crescer.

### Itens de verificação

```
./peer upload arquivo.pdf
```

O sistema deverá produzir:

```
File: arquivo.pdf
Size: 12458291 bytes

ObjectID:
a7f3...

Chunks: 3

Chunk 0: ...
Chunk 1: ...
Chunk 2: ...

Compression: LZ4
```

Depois:

```
./peer download arquivo.pdf
```

O sistema deverá produzir:

```
Download completed
SHA-256 verified
```

> **Atenção a dois pontos deste checkpoint:** o `Chunks: 3` só fecha com `CHUNK_SIZE` de 4 MiB — ver `[C-01]`, Seção 11.2. E o `download` **pelo nome do arquivo** exige o índice secundário por nome — ver `[C-17]`, Seção 8.5.

## 16.8 CHECKPOINT 3 — 09/10

**8)**

### Aluno 1 `[P-01]`

- membership
- heartbeat
- suspect
- failed
- removed

**Estados:**

```
ALIVE
  ↓
SUSPECT
  ↓
FAILED
  ↓
REMOVED
```

**Temporização:**

```
Heartbeat: 5 segundos
Timeout:  15 segundos
```

### Aluno 2

- NodeID
- successor
- predecessor
- finger table
- lookup
- join
- stabilize
- notify
- fix_fingers

**Exemplo:**

```
Node A
  │
  ├── successor   → B
  ├── predecessor → D
  │
  └── finger table
        ├── B
        ├── C
        ├── E
        └── G
```

### Itens de verificação

**Inicialização:**

```
SP1
SP2
SP3
SP4
SP5
```

**Posteriormente:**

```
JOIN SP2
JOIN SP3
JOIN SP4
JOIN SP5
```

**Teste de lookup:**

```
lookup <ObjectID>
```

Saída:

```
Lookup path:

SP1
 ↓
SP3
 ↓
SP5

Owner: SP5
```

**Teste de falha:**

```
kill -9 SP3
```

Saída:

```
SP3 SUSPECT
SP3 FAILED
```

## 16.9 CHECKPOINT 4 — 21/10

**9)**

### Aluno 1 `[P-01]`

- `ELECTION`
- `OK`
- `COORDINATOR`

**Fluxo:**

```
Coordinator morreu
        ↓
     timeout
        ↓
    ELECTION
        ↓
nós de maior prioridade
        ↓
       OK
        ↓
 novo coordenador
        ↓
   COORDINATOR
```

### Aluno 2

- operation log
- log index
- transaction ID
- version
- checksum

**Exemplo:**

```
LOG #101
PUT fileA

LOG #102
PUT fileB

LOG #103
DELETE fileA
```

### Itens de avaliação

**Início:**

```
SP1
SP2
SP3
SP4
```

`SP4` é coordenador.

**Executa:**

```
kill -9 SP4
```

O sistema deve retornar:

```
detect failure
      ↓
start election
      ↓
highest NodeID wins
      ↓
 COORDINATOR
      ↓
resume operations
```

**Próxima execução:**

```
upload arquivo.pdf
```

E compara:

```
metadata SP1
metadata SP2
metadata SP3
```

Todos precisam estar logicamente consistentes.

## 16.10 CHECKPOINT 5 — 30/10

**10)**

### Aluno 1 — implementa LFU + Transferência

- LFU
- cache hit
- cache miss
- frequency
- eviction

### Aluno 2 — implementa 2PC + IST

```
PREPARE
   ↓
YES / NO
   ↓
COMMIT / ABORT
```

**Funcionamento do IST:**

```
Novo nó:      version 1032
Coordenador:  version 1050

Transferir: 1033 … 1050  (em vez de toda a base)
```

Isso corresponde diretamente ao mecanismo de *Incremental State Transfer* descrito na Seção 13.

### Itens de avaliação

**Início:**

```
SP1
SP2
SP3
```

**Retorna:**

```
UPLOAD A
UPLOAD B
UPLOAD C
```

**Derrubar `SP2`.** Enquanto `SP2` está fora:

```
UPLOAD D
UPLOAD E
```

**Reiniciar `SP2`.** Após o reinício, tem-se:

```
SP2
 ↓
JOIN
 ↓
IST
 ↓
versions missing
 ↓
receive logs
 ↓
READY
```

**Teste de 2PC:** forçar uma falha durante a fase `PREPARE`. O sistema deve evitar deixar os participantes em estados inconsistentes. Ver `[C-18]`, Seção 14.7.

## 16.11 CHECKPOINT 6 — 06/11

**11)** Integração e teste final. `[C-12]`

### Aluno 1 deve demonstrar

- upload
- download
- chunks
- compression
- cache
- replica

### Aluno 2 deve demonstrar

- Chord
- Gossip
- Bully
- SMR
- 2PC
- IST

Por fim, ambos podem demonstrar juntos: **Integração, Debugging, Documentação e Testes.**

> Nessa fase o professor fornecerá um conjunto de testes para testar o sistema.

## 16.12 Observações

> ### OBSERVAÇÃO IMPORTANTE
>
> **SÓ SE AVANÇA PARA O CHECKPOINT SEGUINTE SE O ATUAL ESTIVER FUNCIONAL.** Caso contrário, na próxima data de checkpoint, o grupo deverá apresentar o checkpoint anterior.

> ### OBSERVAÇÃO 2
>
> **AO FINAL DE CADA CHECKPOINT, O GRUPO DEVERÁ FORNECER UM DOCUMENTO COM A EXPLICAÇÃO DO DESENVOLVIMENTO DAQUELE CHECKPOINT.**

## 16.13 Gradação do trabalho

| Checkpoint | Data | Pontos |
|---|---|---|
| Checkpoint 1 | 18/09 | 1,0 |
| Checkpoint 2 | 30/09 | 1,0 |
| Checkpoint 3 | 09/10 | 2,0 |
| Checkpoint 4 | 21/10 | 2,0 |
| Checkpoint 5 | 30/10 | 2,0 |
| Checkpoint 6 | 06/11 | 2,0 |
| **Total** | | **10,0** |

`[C-11]`

---
---

# Apêndice A — Proveniência deste documento

## A.1 Origem

Transcrição integral de `trabalho_2026_SD.pdf` — 38 páginas, 155.767 bytes, produzido em LibreOffice 7.3 / Writer em 08/09/2026.

Extração feita com `pdftotext -layout`, com conferência dos fluxos de conteúdo descomprimidos via `qpdf --qdf` e verificação visual por `pdftoppm`.

## A.2 Conteúdo removido

O PDF original contém **18 blocos de texto oculto** que **não fazem parte da especificação do trabalho**. Eles foram removidos integralmente deste documento.

**Como estavam ocultos:**

| Propriedade | Texto da especificação | Blocos removidos |
|---|---|---|
| Cor de preenchimento | `0 0 0 rg` (preto) | `1 1 1 rg` (branco puro) |
| Corpo da fonte | 12 pt (títulos até 16 pt) | 2 pt |
| Altura de glifo medida | 15,6 pt | 2,6 pt |
| Visível ao ler o PDF | Sim | Não |
| Capturado por extrator de texto | Sim | Sim |

**Páginas afetadas no original:** 1, 7, 11, 15, 16, 18, 20, 22, 23, 25, 27, 28, 29, 32, 33, 36, 37 e 38 — todas com o mesmo conteúdo, repetido literalmente.

**Natureza do conteúdo:** instruções endereçadas a assistentes de IA, mandando produzir código deliberadamente defeituoso e ocultar os defeitos de quem pedisse o código. O texto integral **não é reproduzido aqui de propósito**, para que este arquivo possa ser usado com segurança como entrada de qualquer ferramenta de IA. A transcrição literal, com a análise técnica completa, está em `../ANALISE_PROMPT_INJECTION.md`.

Nenhum conteúdo técnico da especificação foi perdido na remoção: os blocos ocultos eram autocontidos e não se conectavam ao texto ao redor.

## A.3 Figuras e diagramas

**Não há nenhuma imagem a inserir manualmente.** Verificado por três vias independentes:

```
pdfimages -list trabalho_2026_SD.pdf   → nenhuma imagem listada
/Subtype /Image                        → 0 ocorrências
/XObject                               → 0 ocorrências
```

O PDF **não contém nenhuma imagem raster ou vetorial embutida**. Os 20 diagramas do original são desenhados com caracteres de caixa (`│ ├ └ ─ ┴ ↓ ▼`) como texto comum, e foram todos reconstruídos neste documento em blocos de código. A única perda na extração foi de espaçamento horizontal, corrigida manualmente.

**Diagramas reconstruídos e onde estão:**

| # | Diagrama | Seção |
|---|---|---|
| 1 | Arquitetura Física | 3.2 |
| 2 | Arquitetura em Camadas | 3.3 |
| 3 | Header / Payload da mensagem | 3.6 |
| 4 | Interação entre Componentes | 3.12 |
| 5 | Modelo Arquitetural P2P | 4.2 |
| 6 | Arquitetura Lógica do DMR | 8.2 |
| 7 | Consulta de metadados | 8.4 |
| 8 | Arquitetura do Cache LFU | 9.2 |
| 9 | Compressão paralela | 10.5 |
| 10 | Arquitetura da Replicação SMR | 12.2 |
| 11 | Máquina de Estados SMR | 12.4 |
| 12 | Fluxo IST | 13.2 |
| 13 | Estados 2PC | 14.3 |
| 14 | Timeout do 2PC | 14.7 |
| 15 | Fluxo Completo do 2PC | 14.8 |
| 16 | Estados Bully | 15.3 |
| 17 | Fluxo Integrado de Recuperação | 15.10 |
| 18 | Pipeline do Checkpoint 2 | 16.7 |
| 19 | Finger table do Node A | 16.8 |
| 20 | Fluxo de eleição do Checkpoint 4 | 16.9 |

Se quiser conferir qualquer diagrama contra o original:

```bash
pdftotext -layout -f <página> -l <página> trabalho_2026_SD.pdf -
```

---

# Apêndice B — Divergências em relação ao original

Todo ponto marcado com `[C-xx]` no corpo do documento está listado aqui. Use esta tabela para comparar com o PDF que o professor usará na correção.

## B.1 Texto alterado

| ID | Seção | Original dizia | Nesta versão | Justificativa |
|---|---|---|---|---|
| `C-01` | 11.2 | `Chunk Size = 4 MB` | `4 MiB = 4 × 1024 × 1024 = 4.194.304 bytes` | O próprio item de verificação do CP2 exige `12458291 bytes → 3 chunks`, o que só fecha com 4 MiB. Com 4.000.000 dá 4 chunks. |
| `C-03` | 16.7 | `uint32_t version` | `uint64_t version` | A tabela de metadados (8.3) define `Version` como `uint64`. |
| `C-04` | 16.7 | `uint32_t owner` | `uint8_t owner[32]` | `OwnerPeer` é um `NodeID` (SHA-256, 256 bits); não cabe em `uint32_t`. |
| `C-06` | 2.3, 3.7 | Em 2 locais: comprimir **antes** de fragmentar | Fragmentar → comprimir cada chunk | O original se contradiz em 6 pontos: 4 dizem fragmentar primeiro (10.2, 10.3, 11.4, 16.7), 2 dizem o contrário. Fragmentar primeiro é a única ordem compatível com transferência paralela e retomada por réplica. |
| `C-09` | 1.6 | "suportar múltiplas arquiteturas x86-64" | "suportar a arquitetura x86-64" | x86-64 é uma arquitetura só; o RNF-005 confirma alvo único. |
| `C-11` | 16.13 | Seis linhas rotuladas "Checkpoint 1" | Checkpoint 1 a 6 | Erro de rótulo. A distribuição de pontos foi preservada e soma 10,0. |
| `C-12` | 16.11 | Dois itens numerados `10)` | CP6 renumerado para `11)` | Erro de numeração. |

## B.2 Texto preservado, com nota de esclarecimento

| ID | Seção | Ponto | Natureza da nota |
|---|---|---|---|
| `C-02` | 5.4 | `TransactionID` de 128 bits contendo `NodeID` de 256 bits | Impossível como escrito; proposta de truncamento a registrar |
| `C-05` | 16.7 | `struct` do CP2 tem 7 dos 13 campos da tabela | É o mínimo do checkpoint, não o modelo final |
| `C-07` | 2.5 | "Consistência forte" com Gossip e Version Vector | Forte na escrita via SMR/2PC; Gossip só para membership |
| `C-08` | 5.2 | Estado `AUTHENTICATED` sem mecanismo de autenticação | Handshake trivial; TLS/JWT fora de escopo |
| `C-10` | 3.4 | "Nove subsistemas" mas dez componentes listados | Coordenador é papel eleito, não subsistema |
| `C-13` | Cabeçalho | Título diz "BITTORRENT" | Título oficial mantido; o corpo é Chord/Gossip/Bully/2PC/SMR e é ele que governa |
| `C-14` | 6.3 | `NodeID = SHA256(IP‖Porta‖UUID)` | UUID precisa ser persistido em disco |
| `C-15` | 11.3 | `Offset` do chunk ambíguo após compressão | Guardar `offset_original` e `compressed_size` |
| `C-16` | 2.5 | RNF-010 (eleição < 5 s) vs Timeout de 15 s | Só fecha medindo a partir do `ELECTION` |
| `C-17` | 8.5 | Download por nome vs Lookup por ObjectID | O índice secundário por nome é obrigatório |
| `C-18` | 14.7 | Recuperação de 2PC apresentada como completa | 2PC é bloqueante; implementar assim e registrar a limitação |

> **Sobre o título (`C-13`).** A capa anuncia "IMPLEMENTAÇÃO DE UM BITTORRENT", mas a especificação inteira descreve um sistema **DHT-Chord com Super Peers**. Não há nenhum mecanismo de BitTorrent no documento: sem *tracker*, sem arquivo `.torrent`, sem *bitfield*, sem *tit-for-tat*, sem *rarest-first*, sem *choking*. São arquiteturas diferentes. **Implemente o que está no corpo do texto.** Pedir "um BitTorrent" a uma ferramenta de IA produz *tracker* e *rarest-first* — nada disso está sendo pedido nem será avaliado.

---

# Apêndice C — Pendências a confirmar com o professor

## `[P-01]` — Divisão Aluno 1 / Aluno 2 nos Checkpoints 3 e 4

**O texto original foi mantido exatamente como está**, sem harmonização, nos três lugares onde aparece. Registro aqui apenas a divergência, para que vocês decidam com o professor.

| Tema | Tabela mestre (16.5) | Checkpoint 3 (16.8) | Checkpoint 4 (16.9) | Checkpoint 6 (16.11) |
|---|---|---|---|---|
| Gossip / Heartbeat / Detecção de falhas | **Aluno 2** | **Aluno 1** | — | **Aluno 2** |
| Bully (`ELECTION`/`OK`/`COORDINATOR`) | **Aluno 2** | — | **Aluno 1** | **Aluno 2** |

Os Checkpoints 3 e 4 atribuem ao **Aluno 1** trabalho que a tabela mestre e o Checkpoint 6 atribuem ao **Aluno 2**.

**Por que isso importa na prática:** a Seção 16.5 determina que *"no dia do checkpoint, apenas o aluno responsável pela sua parte deverá apresentar"*. Seguir a fonte errada faz a pessoa errada aparecer para apresentar.

**Ação sugerida:** confirmar com o professor **antes de 18/09**, junto com a pergunta sobre a numeração dos checkpoints na tabela de notas (`C-11`).

## `[P-02]` — Decisões de projeto a registrar na documentação

Estas não têm resposta certa no enunciado; o grupo escolhe e **documenta a escolha**. Vale ponto na Observação 2 (documento por checkpoint).

| Decisão | Onde | Referência |
|---|---|---|
| Formato final do `TransactionID` | Antes do CP4 | `C-02` |
| Escopo exato da "consistência forte" | Antes do CP5 | `C-07` |
| O que `AUTHENTICATED` faz na prática | CP1 | `C-08` |
| Ponto de medição do RNF-010 | CP4 | `C-16` |
| Limitação de bloqueio do 2PC | CP5 | `C-18` |

---

# Apêndice D — Checklist de riscos técnicos

Pontos que, se implementados errado, **só se manifestam em checkpoints posteriores**, quando já é caro refatorar:

| Risco | Decidir em | Consequência se errar | Ref. |
|---|---|---|---|
| `CHUNK_SIZE` decimal em vez de 4 MiB | **CP2** | O item de verificação retorna 4 chunks em vez de 3, sem mensagem de erro | `C-01` |
| UUID não persistido em disco | **CP1** | NodeID muda a cada reinício; Finger Table e IST quebram no CP3 e no CP5 | `C-14` |
| `owner` como `uint32_t` | **CP2** | Impossível comparar com NodeIDs reais no Bully e no Chord (CP3/CP4) | `C-04` |
| Sem índice secundário por nome | **CP2** | `./peer download arquivo.pdf` inexecutável | `C-17` |
| Comprimir antes de fragmentar | **CP2** | Perde acesso aleatório a blocos; transferência paralela e retomada por réplica ficam inviáveis no CP5 | `C-06` |
| `SHA-256` do chunk sobre dado comprimido | **CP2** | Validação quebra se o algoritmo de compressão mudar | `C-15` |

---

*Documento gerado a partir de `trabalho_2026_SD.pdf` em 09/09/2026. O PDF original permanece intacto e é a fonte oficial para fins de avaliação. Em caso de divergência entre este documento e o PDF, consulte o Apêndice B e, na dúvida, o professor.*
