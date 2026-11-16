#!/usr/bin/env bash
# Passos reaproveitados pelos cenários.

cl() { bash "$CLUSTER_DIR/cluster.sh" "$@"; }

# Primeiro cliente de cada host (um por máquina), para cenários que não precisam de todos.
first_clients() {
    local h
    for h in "${HOST_LIST[@]}"; do
        clients_on_host "$h" | head -n1
    done
}

# Coordenador que cada Super Peer reconhece por último ("COORDINATOR <nome>"),
# uma linha "<SP> <coordenador>" por nó (vazio se ainda não houve).
coordinators_seen() {
    local n
    for n in "$@"; do
        printf '%s %s\n' "$n" "$(logical_log <"$(node_log "$n")" 2>/dev/null |
            awk '/^COORDINATOR /{ c = $2 } END { print c }')"
    done
}

# Espera os nós dados concordarem no coordenador (duas leituras seguidas iguais,
# com 3 s entre elas) e imprime o nome dele.
wait_coordinator_agreement() {
    local timeout="$1" end prev="" cur
    shift
    end=$((SECONDS + timeout))
    while ((SECONDS < end)); do
        # um nó sem COORDINATOR ainda gera linha vazia, e aí há mais de um valor
        cur="$(coordinators_seen "$@" | awk '{ print $2 }' | sort -u)"
        if [[ -n "$cur" && "$cur" != *$'\n'* ]]; then
            if [[ "$cur" == "$prev" ]]; then
                printf '%s' "$cur"
                return 0
            fi
            prev="$cur"
        else
            prev=""
        fi
        sleep 3
    done
    coordinators_seen "$@" >&2
    return 1
}

# Sobe os Super Peers e espera: o Bully escolher o coordenador (maior NodeID, que só
# se conhece depois de subir), todos o reconhecerem e cada nó publicar o sucessor
# Chord. Define COORD e a métrica de eleição.
boot_cluster() {
    local n first_ms ms
    local to=$((40 + 2 * ${#NODE_LIST[@]}))

    cl up || die "cluster.sh up falhou"
    say "esperando até ${to}s: todos os Super Peers reconhecerem o mesmo coordenador"
    COORD="$(wait_coordinator_agreement "$to" "${NODE_LIST[@]}")" ||
        die "os Super Peers não concordaram no coordenador em ${to}s"
    [[ -n "${NODE_HOST[$COORD]:-}" ]] || die "coordenador '$COORD' não é um Super Peer do cluster.conf"
    wait_log "$(node_log "$COORD")" "^highest NodeID wins: ${COORD}\$" 5 "$COORD registrar a própria vitória"

    for n in "${NODE_LIST[@]}"; do
        wait_log "$(node_log "$n")" '^Successor: ' 30 "$n publicar o sucessor"
    done

    first_ms=""
    for n in "${NODE_LIST[@]}"; do
        ms="$(first_stamp_ms "$(node_log "$n")")"
        if [[ -z "$first_ms" ]] || ((ms < first_ms)); then first_ms="$ms"; fi
    done
    ms="$(stamp_ms "$(node_log "$COORD")" "^highest NodeID wins: ${COORD}\$")"
    metric "eleição no boot (primeiro nó no ar até a última vitória de $COORD)" "$(fmt_s $((ms - first_ms)))" "< 5 s depois do último nó (RNF-010)"
    ok "coordenador $COORD e anel com sucessor em ${#NODE_LIST[@]} nós"
}

# Diretório de saída do cliente (absoluto no host dele).
client_out() { printf '%s/%s' "$(client_dir "$1")" "$2"; }

# Primeiro cliente de um host diferente do de $1 (se só há um host, o segundo cliente).
other_host_client() {
    local from="$1" c
    for c in "${CLIENT_LIST[@]}"; do
        [[ "${CLIENT_HOST[$c]}" != "${CLIENT_HOST[$from]}" ]] && { printf '%s' "$c"; return 0; }
    done
    for c in "${CLIENT_LIST[@]}"; do
        [[ "$c" != "$from" ]] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

# download <cliente> <nome> <saida-relativa>; confere o sha256 contra o arquivo original.
download_and_check() {
    local c="$1" name="$2" outname="$3" src="$4" h want got
    h="${CLIENT_HOST[$c]}"
    cdo "$c" download "$name" "$(client_out "$c" "$outname")"
    want="$(remote_sha "$h" "$(client_file "$c" "$src")")"
    got="$(remote_sha "$h" "$(client_out "$c" "$outname")")"
    [[ -n "$want" && "$want" == "$got" ]] || die "download de $name por $c difere do original (sha256 $got, esperado $want)"
    say "download de $name por $c idêntico ao original ($want)"
}
