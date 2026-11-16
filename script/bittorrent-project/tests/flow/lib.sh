#!/usr/bin/env bash
# Funções do harness de fluxo. Cada processo tem o próprio arquivo de log.

source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

FLOW_LOG="$PROJECT_ROOT/tests/flow/logs"
FLOW_OUT="$PROJECT_ROOT/tests/flow/out"
FLOW_CONF="$CONFIG_DIR/flow.conf"
LAST_PID=""

mkdir -p "$FLOW_LOG" "$FLOW_OUT"

say() { printf '%s [harness] %s\n' "$(date +%H:%M:%S.%3N)" "$*"; }

# Prefixa cada linha com o instante em que a captura a leu.
# O programa em si não muda. As conferências ignoram esse prefixo.
stamp_stream() {
    local line
    while IFS= read -r line || [[ -n "${line:-}" ]]; do
        printf '%s %s\n' "$(date +%H:%M:%S.%3N)" "$line"
    done
}

# Tira o prefixo HH:MM:SS.mmm para comparar com o texto original.
logical_log() {
    sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} //'
}

die() {
    say "FALHA: $*"
    fail "$*"
    summary
    exit 1
}

start_node() {
    local port="$1"
    local logfile="$2"
    local tag="$3"

    : > "$logfile"
    say "subindo $tag na porta $port → $(basename "$logfile")"
    stdbuf -oL -eL "$NODE_BIN" --config "$FLOW_CONF" --port "$port" \
        > >(stamp_stream >>"$logfile") 2>&1 &
    LAST_PID=$!
    PIDS+=("$LAST_PID")
}

stop_pid() {
    local pid="$1"
    local who="${2:-pid $pid}"

    say "encerrando $who (kill -9 $pid)"
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

wait_log() {
    local file="$1"
    local pattern="$2"
    local timeout="$3"
    local desc="$4"
    local hit

    local start now
    say "esperando até ${timeout}s em $(basename "$file"): $desc"
    say "  padrão: $pattern"
    start=$(date +%s)
    while true; do
        if logical_log <"$file" | grep -Eq "$pattern"; then
            break
        fi
        now=$(date +%s)
        if (( now - start >= timeout )); then
            die "tempo esgotado (${timeout}s) em $(basename "$file"): $desc. Padrão: $pattern"
        fi
        sleep 1
    done
    # padrão pelo ambiente: com awk -v as barras invertidas da regex seriam consumidas
    hit=$(PAT="$pattern" awk '
        {
            line = $0
            sub(/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} /, "", line)
            if (line ~ ENVIRON["PAT"]) {
                found = $0
            }
        }
        END { if (found) print found }
    ' "$file")
    say "apareceu em $(basename "$file"): $hit"
}

run_client() {
    local logfile="$1"
    local tag
    local fifo
    local stamper
    local rc

    shift
    tag=$(basename "$logfile" .log)
    : > "$logfile"
    say "cliente $tag: $CLIENT_BIN $*"
    # FIFO em vez de >(...): o wait abaixo garante que o carimbo gravou a última
    # linha antes de qualquer conferência ler o log.
    fifo="$logfile.fifo"
    rm -f "$fifo"
    mkfifo "$fifo"
    stamp_stream <"$fifo" >>"$logfile" &
    stamper=$!
    "$CLIENT_BIN" "$@" >"$fifo" 2>&1
    rc=$?
    wait "$stamper"
    rm -f "$fifo"
    if [[ "$rc" -ne 0 ]]; then
        die "cliente saiu com $rc ($*) — ver $(basename "$logfile")"
    fi
    say "cliente $tag terminou com sucesso"
}


# --- conferências sobre o log do Super Peer ------------------------------
# O SMR imprime "LOG #<n>" e, logo em seguida, "PUT <caminho do upload>". O caminho é o
# que o cliente passou (muitas vezes absoluto), então o nome casa pelo basename.
# Funções awk compartilhadas pelas conferências abaixo.
AWK_PUT='
function put_name(line,    s) {
    if (substr(line, 1, 4) != "PUT ") return ""
    s = substr(line, 5)
    sub(/.*\//, "", s)
    return s
}'

# 2PC no log do coordenador: "2PC [IDLE -> PREPARING] LOG #<idx>", <expect> votos
# "2PC YES <-", "LOG #<idx>" seguido de "PUT <nome>" e "2PC [COMMIT -> DONE] LOG #<idx>".
assert_round() {
    local file="$1"
    local name="$2"
    local expect_yes="$3"
    local idx="$4"
    local result

    say "conferindo 2PC de $name em $(basename "$file"): PREPARE, $expect_yes YES, LOG #$idx, COMMIT"
    result=$(awk -v name="$name" -v idx="$idx" -v expect="$expect_yes" "$AWK_PUT"'
        { lines[++n] = $0 }
        END {
            put = 0
            for (i = 2; i <= n; i++) {
                if (put_name(lines[i]) == name && lines[i - 1] == "LOG #" idx) {
                    put = i
                }
            }
            if (!put) {
                print "sem LOG #" idx " seguido de PUT " name
                exit
            }
            start = "2PC [IDLE -> PREPARING] LOG #" idx " "
            prepare = 0
            for (i = put; i >= 1; i--) {
                if (index(lines[i], start) == 1) {
                    prepare = i
                    break
                }
            }
            if (!prepare) {
                print "sem \"" start "\" antes de PUT " name
                exit
            }
            done_line = "2PC [COMMIT -> DONE] LOG #" idx
            commit = 0
            for (i = put; i <= n; i++) {
                if (lines[i] == done_line) {
                    commit = i
                    break
                }
                if (index(lines[i], "2PC [PREPARING -> ABORT] LOG #" idx " ") == 1) {
                    print "2PC de " name " abortado"
                    exit
                }
            }
            if (!commit) {
                print "sem \"" done_line "\" depois de PUT " name
                exit
            }
            yes = 0
            for (i = prepare + 1; i < commit; i++) {
                if (index(lines[i], "2PC YES <- ") == 1) {
                    yes++
                }
            }
            if (yes != expect + 0) {
                print "YES=" yes " esperado " expect " em " name
                exit
            }
            print "ok"
        }
    ' < <(logical_log <"$file"))
    if [[ "$result" != "ok" ]]; then
        die "$(basename "$file"): $result"
    fi
    say "2PC de $name ok em $(basename "$file")"
}

# LOG #<n> imediatamente seguido de PUT <nome>, na ordem dos nomes (n = 1, 2, ...).
assert_log_sequence() {
    local file="$1"
    shift
    local idx=1
    local name
    local result

    say "conferindo a sequência do log em $(basename "$file"): $*"
    for name in "$@"; do
        result=$(awk -v name="$name" -v idx="$idx" "$AWK_PUT"'
            prev == "LOG #" idx && put_name($0) == name { found = 1 }
            { prev = $0 }
            END { if (!found) print "sem LOG #" idx " / PUT " name }
        ' < <(logical_log <"$file"))
        if [[ -n "$result" ]]; then
            die "$(basename "$file"): $result"
        fi
        say "  $(basename "$file") tem LOG #$idx PUT $name"
        idx=$((idx + 1))
    done
}

# Cada padrão (regex estendida) precisa casar com uma linha, nesta ordem.
assert_lines_in_order() {
    local file="$1"
    shift
    local missing

    say "conferindo a ordem em $(basename "$file"): $*"
    # Os padrões vão pelo ambiente: com awk -v as barras invertidas seriam interpretadas.
    missing=$(PATS="$(printf '%s\n' "$@")" awk '
        BEGIN {
            n = split(ENVIRON["PATS"], pats, "\n")
            if (n > 0 && pats[n] == "") {
                n--
            }
            p = 1
        }
        p <= n && $0 ~ pats[p] { p++ }
        END {
            if (p <= n) {
                print pats[p]
            }
        }
    ' < <(logical_log <"$file")) || die "padrão inválido em assert_lines_in_order: $*"
    if [[ -n "$missing" ]]; then
        die "$(basename "$file") não teve linha com '$missing' depois das anteriores"
    fi
    say "ordem ok em $(basename "$file")"
}

assert_exact_absent() {
    local file="$1"
    local line="$2"

    say "confirmando que $(basename "$file") não tem a linha '$line'"
    if logical_log <"$file" | grep -qx -F "$line"; then
        die "$(basename "$file") contém a linha '$line'"
    fi
    say "$(basename "$file") não tem '$line'"
}

# Nenhuma linha casa com o padrão (regex estendida).
assert_pattern_absent() {
    local file="$1"
    local pattern="$2"

    say "confirmando que $(basename "$file") não tem linha com '$pattern'"
    if logical_log <"$file" | grep -Eq "$pattern"; then
        die "$(basename "$file") tem linha com '$pattern'"
    fi
    say "$(basename "$file") não tem '$pattern'"
}

assert_name_absent() {
    local file="$1"
    local name="$2"
    local hit

    say "confirmando que $(basename "$file") não aplicou PUT $name"
    hit=$(awk -v name="$name" "$AWK_PUT"' put_name($0) == name { print; exit }' < <(logical_log <"$file"))
    if [[ -n "$hit" ]]; then
        die "$(basename "$file") contém $hit"
    fi
    say "$(basename "$file") não tem PUT $name"
}

# Estado replicado tirado do log: último LOG #, quantos nomes distintos e quais.
# Imprime "LogIndex <n>", "Objects: <k>" e "<nome> v<versões>" (uma linha por nome).
log_state() {
    local file="$1"
    awk "$AWK_PUT"'
        /^LOG #[0-9]+$/ { idx = substr($0, 6) + 0 }
        {
            f = put_name($0)
            if (f != "") {
                if (!(f in seen)) { order[++k] = f }
                seen[f]++
            }
        }
        END {
            print "LogIndex " (idx + 0)
            print "Objects: " (k + 0)
            for (i = 1; i <= k; i++) { print order[i] " v" seen[order[i]] }
        }
    ' < <(logical_log <"$file")
}

# assert_log_state <log> <LogIndex> <Objects> <nomes...>
assert_log_state() {
    local file="$1"
    local index="$2"
    local objects="$3"
    shift 3
    local text name

    say "conferindo o estado em $(basename "$file"): LogIndex $index, Objects: $objects, nomes: $*"
    text="$(log_state "$file")"
    grep -qx "LogIndex $index" <<<"$text" || die "$(basename "$file") sem LogIndex $index"$'\n'"$text"
    grep -qx "Objects: $objects" <<<"$text" || die "$(basename "$file") sem Objects: $objects"$'\n'"$text"
    for name in "$@"; do
        grep -q "^${name} v" <<<"$text" || die "$(basename "$file") sem $name"$'\n'"$text"
    done
    ok "$(basename "$file") LogIndex $index, $objects objetos"
}
