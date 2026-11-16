#!/usr/bin/env bash
# Inspeção dos Super Peers: membership, Chord, coordenador, estado do SMR, metadados e eventos.
# Fontes: os logs espelhados em cluster/logs (o que cada nó imprime) e, para a finger table
# e o lookup, o próprio Super Peer ao vivo (bin/client --cmd topology / --cmd lookup).
#
# Linhas do superpeer.c usadas aqui:
#   NodeID: <hex>                              (node_print, na partida)
#   Successor: <SP> (<ip:porta>)               Predecessor: <SP> (<ip:porta>)
#   COORDINATOR <SP>    start election    highest NodeID wins: <SP>
#   Gossip: membro <SP> (<ip:porta>) <ESTADO>  Gossip: <SP> <ESTADO> por <origem>
#   Gossip: <SP> voltou (<antes> -> ALIVE)     SUSPECT|FAILED|REMOVED <SP> (<ip:porta>): ...
#   LOG #<n> seguido de PUT <caminho>          LFU CACHE HIT|MISS|EVICT ... hits= misses= evictions=

# Nomes pedidos (padrão: todos os Super Peers). Confere se existem.
ins_names() {
    local n
    INS_NAMES=()
    if [[ $# -eq 0 ]]; then
        INS_NAMES=("${NODE_LIST[@]}")
        return 0
    fi
    for n in "$@"; do
        [[ -n "${NODE_HOST[$n]:-}" ]] || { echo "erro: Super Peer desconhecido: $n" >&2; return 1; }
        INS_NAMES+=("$n")
    done
}

# Log (vida atual) do nó sem o carimbo de hora. Falha (com aviso) se o log não existe.
ins_cat() {
    local f
    f="$(node_log "$1")"
    [[ -e "$f" ]] || { echo "sem log de $1 (o cluster está no ar? rode: cluster.sh up)" >&2; return 1; }
    sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} //' "$f"
}

# Último valor de um campo no log: ins_last <SP> <awk-regex> <campo>
ins_last() {
    ins_cat "$1" 2>/dev/null | awk -v re="$2" -v k="$3" '$0 ~ re { v = $k } END { print v }'
}

# NodeID (hex) do nó, lido do log; vazio se o nó ainda não imprimiu.
ins_nodeid() { ins_last "$1" '^NodeID: ' 2; }

# Carrega INS_ID[nome] com o NodeID de todos os Super Peers.
ins_load_ids() {
    local n
    declare -gA INS_ID=()
    for n in "${NODE_LIST[@]}"; do INS_ID[$n]="$(ins_nodeid "$n")"; done
}

# Estado de cada membro segundo o log do observador (o último evento de cada um).
# Preenche INS_MS[SP]=ALIVE|SUSPECT|FAILED|REMOVED.
ins_load_members() {
    local who st
    declare -gA INS_MS=()
    while read -r who st; do
        INS_MS[$who]="$st"
    done < <(ins_cat "$1" 2>/dev/null | awk '
        /^Gossip: membro [^ ]+ \([^)]*\) [A-Z]+$/ { s[$3] = $NF; next }
        /^Gossip: [^ ]+ voltou /                  { s[$2] = "ALIVE"; next }
        /^Gossip: [^ ]+ (ALIVE|SUSPECT|FAILED|REMOVED) por / { s[$2] = $3; next }
        /^(SUSPECT|FAILED|REMOVED) [^ ]+ \(/      { s[$2] = $1 }
        END { for (k in s) print k, s[k] }')
}

# Processo vivo? (alive|dead|?)
ins_proc() { rrun "${NODE_HOST[$1]}" alive "$1" 2>/dev/null | awk '{ print $2 }'; }

# --- members ---------------------------------------------------------------

# Matriz observador x membro: o que cada Super Peer sabe de cada um pelo Gossip.
cmd_members() {
    ins_names "$@" || return 1
    local obs m cell
    printf '%-9s' 'OBSERVA'
    for m in "${NODE_LIST[@]}"; do printf '%-9s' "$m"; done
    echo
    for obs in "${INS_NAMES[@]}"; do
        ins_load_members "$obs"
        printf '%-9s' "$obs"
        for m in "${NODE_LIST[@]}"; do
            if [[ "$m" == "$obs" ]]; then
                cell=eu
            else
                cell="${INS_MS[$m]:--}"
            fi
            printf '%-9s' "$cell"
        done
        echo
    done
    echo
    echo "Fonte: 'Gossip: membro', 'Gossip: <SP> <estado> por', 'voltou' e SUSPECT/FAILED/REMOVED do log de cada observador."
    echo "'-' = o observador nunca registrou esse membro."
}

# --- chord -----------------------------------------------------------------

# Anel pelo NodeID, com o sucessor que cada nó reporta e o que a ordem do anel espera.
# Com --fingers, a finger table ao vivo de cada nó (MSG_TOPOLOGY).
cmd_chord() {
    local fingers=0 a
    local -a want=()
    for a in "$@"; do
        case "$a" in
        --fingers) fingers=1 ;;
        *) want+=("$a") ;;
        esac
    done
    ins_names "${want[@]}" || return 1
    ins_load_ids

    local -a ring=()
    mapfile -t ring < <(for n in "${NODE_LIST[@]}"; do [[ -n "${INS_ID[$n]}" ]] && echo "${INS_ID[$n]} $n"; done | sort | awk '{ print $2 }')
    if [[ ${#ring[@]} -eq 0 ]]; then
        echo "nenhum Super Peer imprimiu o NodeID ainda" >&2
        return 1
    fi

    printf '%-4s %-8s %-14s %-9s %-11s %-9s %s\n' POS NOME NODEID SUCESSOR PREDECESSOR ESPERADO STATUS
    local i name succ pred expect pos j cand out
    for name in "${ring[@]}"; do
        [[ " ${INS_NAMES[*]} " == *" $name "* ]] || continue
        ins_load_members "$name"
        pos=0
        for i in "${!ring[@]}"; do [[ "${ring[$i]}" == "$name" ]] && pos=$i; done
        # esperado: o próximo no anel que o próprio nó não dá como FAILED/REMOVED
        expect="$name"
        for ((j = 1; j < ${#ring[@]}; j++)); do
            cand="${ring[$(((pos + j) % ${#ring[@]}))]}"
            case "${INS_MS[$cand]:-ALIVE}" in
            FAILED | REMOVED) continue ;;
            esac
            expect="$cand"
            break
        done
        succ="$(ins_last "$name" '^Successor: ' 2)"
        pred="$(ins_last "$name" '^Predecessor: ' 2)"
        printf '%-4s %-8s %-14s %-9s %-11s %-9s %s\n' "$pos" "$name" "${INS_ID[$name]:0:12}…" "${succ:--}" "${pred:--}" "$expect" \
            "$([[ -z "$succ" ]] && echo 'sem sucessor no log' || { [[ "$succ" == "$expect" ]] && echo ok || echo DIVERGE; })"
    done
    echo
    echo "Ordem do anel = NodeID crescente. ESPERADO ignora quem o nó já marcou FAILED/REMOVED."

    if ((fingers)); then
        for name in "${INS_NAMES[@]}"; do
            echo
            echo "Finger table de $name (ao vivo):"
            out="$(node_state_text "$name" 2>/dev/null)" && grep '^  finger' <<<"$out" ||
                echo "  sem resposta"
        done
    fi
}

# --- coordinator -----------------------------------------------------------

# Quem cada Super Peer reconhece como coordenador, e se todos concordam.
cmd_coordinator() {
    ins_names "$@" || return 1
    ins_load_ids
    local name coord elections won agreed="" mixed=0 best="" n proc
    printf '%-8s %-6s %-12s %-10s %s\n' NOME PROC COORDENADOR ELEICOES 'VENCEU(vezes)'
    for name in "${INS_NAMES[@]}"; do
        coord="$(ins_last "$name" '^COORDINATOR ' 2)"
        elections="$(ins_cat "$name" 2>/dev/null | grep -c '^start election' || true)"
        won="$(ins_cat "$name" 2>/dev/null | grep -c '^highest NodeID wins: ' || true)"
        proc="$(ins_proc "$name")"
        printf '%-8s %-6s %-12s %-10s %s\n' "$name" "${proc:-?}" "${coord:--}" "$elections" "$won"
        # um nó fora do ar guarda só o que sabia ao cair: não entra na concordância
        [[ -n "$coord" && "$proc" != dead ]] || continue
        [[ -z "$agreed" ]] && agreed="$coord"
        [[ "$agreed" == "$coord" ]] || mixed=1
    done
    # o Bully elege o maior NodeID entre os vivos
    for n in "${NODE_LIST[@]}"; do
        [[ -n "${INS_ID[$n]}" && "$(ins_proc "$n")" == alive ]] || continue
        [[ -z "$best" || "${INS_ID[$n]}" > "${INS_ID[$best]}" ]] && best="$n"
    done
    echo
    if [[ -z "$agreed" ]]; then
        echo "nenhum coordenador anunciado ainda"
    elif ((mixed)); then
        echo "DIVERGÊNCIA: os Super Peers vivos não concordam sobre o coordenador"
    else
        if [[ -n "$best" && "$best" != "$agreed" ]]; then
            echo "todos os vivos reconhecem $agreed. O maior NodeID vivo é $best: ele voltou com o sistema em operação e entrou como FOLLOWER (Seção 15.7)"
        else
            echo "todos os vivos reconhecem $agreed (maior NodeID entre os vivos)"
        fi
    fi
}

# --- state -----------------------------------------------------------------

# INS_FILES[arquivo]=versões: quantas vezes cada nome foi aplicado (PUT) no nó.
# Roda no shell atual (não numa substituição de processo) para o array sobreviver.
ins_load_files() {
    local f v
    declare -gA INS_FILES=()
    while read -r f v; do
        INS_FILES[$f]="$v"
    done < <(ins_cat "$1" 2>/dev/null | awk '
        substr($0, 1, 4) == "PUT " { s = substr($0, 5); sub(/.*\//, "", s); n[s]++ }
        END { for (k in n) print k, n[k] }')
}

# Estado replicado de um nó tirado do log: "idx objetos hits misses evictions".
ins_state_of() {
    ins_cat "$1" 2>/dev/null | awk '
        /^LOG #[0-9]+$/ { idx = substr($0, 6) + 0 }
        substr($0, 1, 4) == "PUT " { s = substr($0, 5); sub(/.*\//, "", s); if (!(s in seen)) { seen[s] = 1; obj++ } }
        /^LFU CACHE/ || /^LFU Cache stats/ {
            if (match($0, /hits=[0-9]+/)) hits = substr($0, RSTART + 5, RLENGTH - 5)
            if (match($0, /misses=[0-9]+/)) miss = substr($0, RSTART + 7, RLENGTH - 7)
            if (match($0, /evictions=[0-9]+/)) ev = substr($0, RSTART + 10, RLENGTH - 10)
        }
        END { print idx + 0, obj + 0, (hits == "" ? "-" : hits), (miss == "" ? "-" : miss), (ev == "" ? "-" : ev) }'
}

# LogIndex, objetos e cache LFU de cada Super Peer, pelo log.
# Com --files, a matriz arquivo x Super Peer com quantas versões cada um aplicou.
cmd_state() {
    local files=0 a
    local -a want=()
    for a in "$@"; do
        case "$a" in
        --files) files=1 ;;
        *) want+=("$a") ;;
        esac
    done
    ins_names "${want[@]}" || return 1

    printf '%-8s %-6s %-9s %-8s %-8s %-8s %s\n' NOME PROC LOGINDEX OBJETOS HITS MISSES EVICTIONS
    local name idx obj hits miss ev proc f cell
    local -A seen_idx=() seen_obj=() matrix=() all_files=()
    for name in "${INS_NAMES[@]}"; do
        [[ -e "$(node_log "$name")" ]] || { printf '%-8s %s\n' "$name" "sem log"; continue; }
        read -r idx obj hits miss ev < <(ins_state_of "$name")
        ins_load_files "$name"
        proc="$(ins_proc "$name")"
        printf '%-8s %-6s %-9s %-8s %-8s %-8s %s\n' "$name" "${proc:-?}" "$idx" "$obj" "$hits" "$miss" "$ev"
        if [[ "$proc" == alive ]]; then
            seen_idx[$idx]=1
            seen_obj[$obj]=1
        fi
        for f in "${!INS_FILES[@]}"; do
            matrix[$name/$f]="${INS_FILES[$f]}"
            all_files[$f]=1
        done
    done
    echo
    if [[ ${#seen_idx[@]} -eq 0 ]]; then
        echo "nenhum Super Peer vivo com log (o cluster está no ar? rode: cluster.sh up)"
        return 1
    elif [[ ${#seen_idx[@]} -le 1 && ${#seen_obj[@]} -le 1 ]]; then
        echo "consistente: mesmo LogIndex e mesmo número de objetos nos Super Peers vivos"
    else
        echo "DIVERGÊNCIA: LogIndex ou número de objetos diferem entre os vivos (SMR propagando, ou nó atrasado)"
    fi
    echo "Fonte: último 'LOG #n', nomes dos 'PUT' e contadores do LFU no log da vida atual de cada nó."

    if ((files)); then
        echo
        printf '%-28s' ARQUIVO
        for name in "${INS_NAMES[@]}"; do printf '%-8s' "$name"; done
        echo
        while read -r f; do
            [[ -n "$f" ]] || continue
            printf '%-28s' "$f"
            for name in "${INS_NAMES[@]}"; do
                cell="${matrix[$name/$f]:-}"
                [[ -n "$cell" ]] && cell="v$cell" || cell='-'
                printf '%-8s' "$cell"
            done
            echo
        done < <(printf '%s\n' "${!all_files[@]}" | sort)
    fi
}

# --- metadata --------------------------------------------------------------

# Metadados (arquivo, tamanho, ObjectID, chunks) que os clientes imprimiram no upload
# (o Super Peer não imprime o metadado). Filtro opcional: parte do nome do arquivo.
cmd_metadata() {
    local filter="${1:-}" c f
    for c in "${CLIENT_LIST[@]}"; do
        f="$(client_log "$c")"
        [[ -e "$f" ]] || continue
        sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} //' "$f" | awk -v who="$c" '
            function flush() { if (file != "") printf "%s\t%s\t%s\t%s\t%s\t%s\n", base, size, oid, cc, ch, who; file = "" }
            /^File: / { flush(); file = substr($0, 7); base = file; sub(/.*\//, "", base); size = ""; oid = ""; cc = ""; ch = ""; want_oid = 0; next }
            file != "" && /^Size: /    { size = $2; next }
            file != "" && /^ObjectID:/ { want_oid = 1; next }
            file != "" && want_oid && /^[0-9a-f]{64}$/ { oid = $0; want_oid = 0; next }
            file != "" && /^Chunks: /  { cc = $2; next }
            file != "" && /^Chunk [0-9]+: / { ch = ch (ch == "" ? "" : ",") $3; next }
            /^\[done / { flush() }
            END { flush() }'
    done | awk -F'\t' -v filter="$filter" '
        filter != "" && index($1, filter) == 0 { next }
        { key = $1 "\t" $3; rec[key] = $2 "\t" $4 "\t" $5; if (index(" " who[key] " ", " " $6 " ") == 0) who[key] = who[key] (who[key] == "" ? "" : " ") $6; name[key] = $1; oid[key] = $3 }
        END {
            for (k in rec) {
                split(rec[k], r, "\t")
                printf "Arquivo   %s\nTamanho   %s bytes\nObjectID  %s\nChunks    %s\n", name[k], r[1], oid[k], r[2]
                n = split(r[3], c, ",")
                for (i = 1; i <= n; i++) printf "  chunk %d  %s\n", i - 1, c[i]
                printf "Enviado por: %s\n\n", who[k]
            }
        }'
}

# --- lookup ----------------------------------------------------------------

# Caminho Chord até o dono de um objeto. <nome|object-id> [SP]; com o nome, o ObjectID
# sai dos logs de upload dos clientes.
cmd_lookup() {
    [[ $# -ge 1 ]] || { echo "uso: lookup <nome|object-id> [SP]" >&2; return 1; }
    local key="$1" via="${2:-${NODE_LIST[0]}}" oid h abs
    [[ -n "${NODE_HOST[$via]:-}" ]] || { echo "erro: Super Peer desconhecido: $via" >&2; return 1; }
    if [[ "$key" =~ ^[0-9a-fA-F]{64}$ ]]; then
        oid="$key"
    else
        oid="$(cmd_metadata "$key" 2>/dev/null | awk -v f="$key" '$1 == "Arquivo" { ok = ($2 == f) } ok && $1 == "ObjectID" { print $2; exit }')"
        [[ -n "$oid" ]] || { echo "ObjectID de '$key' não está nos logs dos clientes (nome exato; ou passe o ObjectID de 64 dígitos)" >&2; return 1; }
    fi
    h="${NODE_HOST[$via]}"
    abs="$(host_abs "$h")" || return 1
    echo "consultando $via ($(node_endpoint "$via")) pelo ObjectID $oid"
    REXEC_TIMEOUT=20 rexec "$h" "cd '$abs/run' && '$abs/bin/client' --cmd lookup --object-id '$oid' --host '$(node_ip "$via")' --port ${NODE_PORT[$via]} --uuid '$abs/run/harness.uuid'"
}

# --- events ----------------------------------------------------------------

# Últimos eventos de um Super Peer, por categoria. Sem categoria: tudo, menos o ruído
# periódico (heartbeat do Gossip, RX). 'all' inclui o ruído.
# uso: events <SP> [membership|election|chord|2pc|ist|lfu|log|chunks|all|regex] [N]
cmd_events() {
    [[ $# -ge 1 ]] || { echo "uso: events <SP> [categoria|regex] [N]  (categorias: membership election chord 2pc ist lfu log chunks all)" >&2; return 1; }
    local name="$1" cat="${2:-}" n="${3:-40}" re
    ins_names "$name" || return 1
    case "$cat" in
    membership) re='^(SUSPECT|FAILED|REMOVED|Gossip: membro|Gossip: [^ ]+ (voltou|ALIVE|SUSPECT|FAILED|REMOVED)|JOIN|LEAVE|detect failure)' ;;
    election) re='^(start election|ELECTION|OK <-|COORDINATOR|highest NodeID|Coordenador|sem COORDINATOR|Bully|resume operations)' ;;
    chord) re='^(Chord|Successor|Predecessor|Lookup)' ;;
    2pc) re='^2PC' ;;
    ist) re='^IST' ;;
    lfu) re='^LFU' ;;
    log) re='^(LOG #|PUT |SMR)' ;;
    chunks) re='^(CHUNK_STORE|DOWNLOAD_REP)' ;;
    all) re='' ;;
    '') re='' ;;
    *) re="$cat" ;;
    esac
    if [[ -n "$re" ]]; then
        # o carimbo fica: aqui ele é o que interessa
        grep -E "^[0-9:.]{12} ${re#^}" "$(node_log "$name")" | tail -n "$n"
    elif [[ "$cat" == all ]]; then
        tail -n "$n" "$(node_log "$name")"
    else
        grep -Ev '^[0-9:.]{12} (Gossip: heartbeat|RX |  finger\[|Finger table)' "$(node_log "$name")" | tail -n "$n"
    fi
}

# --- overview --------------------------------------------------------------

# Uma linha por Super Peer, com o que os logs e o ssh dizem.
cmd_overview() {
    ins_names "$@" || return 1
    ins_load_ids
    local name proc succ coord idx alive susp fail st m
    printf '%-8s %-6s %-14s %-9s %-12s %-8s %s\n' NOME PROC NODEID SUCESSOR COORDENADOR LOGINDEX 'MEMBROS(A/S/F)'
    for name in "${INS_NAMES[@]}"; do
        proc="$(ins_proc "$name")"
        succ="$(ins_last "$name" '^Successor: ' 2)"
        coord="$(ins_last "$name" '^COORDINATOR ' 2)"
        idx="$(ins_cat "$name" 2>/dev/null | awk '/^LOG #[0-9]+$/ { v = substr($0, 6) } END { print v }')"
        ins_load_members "$name"
        alive=0; susp=0; fail=0
        for m in "${!INS_MS[@]}"; do
            [[ "$m" == "$name" ]] && continue
            st="${INS_MS[$m]}"
            case "$st" in
            ALIVE) alive=$((alive + 1)) ;;
            SUSPECT) susp=$((susp + 1)) ;;
            FAILED | REMOVED) fail=$((fail + 1)) ;;
            esac
        done
        printf '%-8s %-6s %-14s %-9s %-12s %-8s %s\n' "$name" "${proc:-?}" "${INS_ID[$name]:+${INS_ID[$name]:0:12}…}" \
            "${succ:--}" "${coord:--}" "${idx:-0}" "$alive/$susp/$fail"
    done
}
