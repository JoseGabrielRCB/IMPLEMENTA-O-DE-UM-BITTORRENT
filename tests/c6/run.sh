#!/usr/bin/env bash
set -u
source "$(dirname "$0")/../common.sh"

# Bateria final do CP6 (Secao 16.11): demonstra as partes do Aluno 1 e do Aluno 2,
# incluindo os cenarios de falha pedidos nos itens de verificacao do CP4 e do CP5.

section "C6 — Integracao e Teste Final (Aluno 1 + Aluno 2)"
require_bin "$NODE_BIN" || exit 1
require_bin "$CLIENT_BIN" || exit 1
"$TEST_ROOT/generate_fixtures.sh" >/dev/null

LOG="$LOG_DIR/c6.log"
LOG_SP2="$LOG_DIR/c6_sp2_restart.log"
LOG_2PC="$LOG_DIR/c6_2pc.log"
OUT="$LOG_DIR/downloaded_c6.pdf"
OUT_FAILOVER="$LOG_DIR/downloaded_c6_failover.pdf"
: > "$LOG"; : > "$LOG_SP2"; : > "$LOG_2PC"
rm -f "$OUT" "$OUT_FAILOVER"

# confere um padrao apenas nas linhas do log a partir de "inicio"
assert_after() {
    local file="$1" inicio="$2" pattern="$3" desc="$4"
    if tail -n +"$inicio" "$file" | grep -Eq "$pattern"; then
        ok "$desc"
    else
        fail "$desc — padrão não encontrado após a linha $inicio: $pattern"
    fi
}

assert_not_contains() {
    local file="$1" pattern="$2" desc="$3"
    if grep -Eq "$pattern" "$file"; then
        fail "$desc — padrão inesperado: $pattern"
    else
        ok "$desc"
    fi
}

start_sp() {   # start_sp <porta> <log>
    "$NODE_BIN" --config "$CONFIG_DIR/c6.conf" --port "$1" >>"$2" 2>&1 &
    echo "$!"
}

SP=()   # SP[0]=SP1(5101) SP[1]=SP2(5102) SP[2]=SP3(5103)
for port in 5101 5102 5103; do
    pid=$(start_sp "$port" "$LOG"); SP+=("$pid"); PIDS+=("$pid")
done
sleep 8

# ---------------------------------------------------------------- Aluno 1
section "Aluno 1: upload, chunks, compression, cache, replica, download"

# upload multi-chunk (10 MiB -> 3 chunks de 4 MiB) -> 2PC + SMR + replica dos chunks
"$CLIENT_BIN" --cmd upload --file "$DATA_DIR/documento_c.pdf" --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
# lookup repetido (LFU MISS -> HIT)
"$CLIENT_BIN" --cmd lookup --name documento_c.pdf --host 127.0.0.1 --port 5102 >>"$LOG" 2>&1 || true
"$CLIENT_BIN" --cmd lookup --name documento_c.pdf --host 127.0.0.1 --port 5102 >>"$LOG" 2>&1 || true
# download paralelo entre replicas
"$CLIENT_BIN" --cmd download --name documento_c.pdf --output "$OUT" --host 127.0.0.1 --port 5103 >>"$LOG" 2>&1 || true

assert_contains "$LOG" 'Chunks: 3' "Fragmentacao em 3 chunks de 4 MiB"
assert_contains "$LOG" 'Compression: LZ4' "Compressao LZ4 ativa"
assert_contains "$LOG" 'LFU CACHE MISS' "Cache LFU: MISS na primeira consulta"
assert_contains "$LOG" 'LFU CACHE HIT' "Cache LFU: HIT na consulta repetida"
assert_contains "$LOG" 'com 3 copias' "Chunks replicados nos 3 Super Peers (CHUNK_STORE)"
assert_contains "$LOG" 'DOWNLOAD_REP replica' "Download paralelo distribuido entre replicas"
assert_contains "$LOG" 'SHA-256 verified' "Reconstituicao e verificacao SHA-256"
[[ -f "$OUT" ]] && assert_file_equal "$DATA_DIR/documento_c.pdf" "$OUT" || fail "Arquivo baixado nao existe: $OUT"

# replica: com o SP2 derrubado, o download troca de replica e termina (Secao 11.6)
inicio=$(( $(wc -l < "$LOG") + 1 ))
kill -9 "${SP[1]}" 2>/dev/null || true
sleep 1
"$CLIENT_BIN" --cmd download --name documento_c.pdf --output "$OUT_FAILOVER" --host 127.0.0.1 --port 5103 >>"$LOG" 2>&1 || true
assert_after "$LOG" "$inicio" 'selecionando proxima replica' "Replica: falha no SP2 detectada e outra replica selecionada"
[[ -f "$OUT_FAILOVER" ]] && assert_file_equal "$DATA_DIR/documento_c.pdf" "$OUT_FAILOVER" || fail "Download com failover nao gerou arquivo"

# ---------------------------------------------------------------- Aluno 2
section "Aluno 2: Chord, Gossip, Bully, SMR, 2PC, IST"

assert_contains "$LOG" 'Successor:|Finger table:' "Chord: anel estabilizado com successor e finger table"
assert_contains "$LOG" 'LOG #1' "SMR: LOG #1 replicado"
assert_contains "$LOG" '2PC PREPARE ->' "2PC: fase PREPARE enviada aos participantes"
assert_contains "$LOG" '2PC \[COMMIT -> DONE\]' "2PC: COMMIT concluido"

# Gossip: SP2 morto -> SUSPECT (Secao 16.8)
sleep $((FAILURE_TIMEOUT_SEC + 5))
assert_contains "$LOG" 'SUSPECT SP2' "Gossip: SP2 marcado como SUSPECT apos o timeout"

# IST (CP5, Secao 16.10): uploads com SP2 fora, depois SP2 volta e recebe so o que falta
"$CLIENT_BIN" --cmd upload --file "$DATA_DIR/documento_a.pdf" --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
"$CLIENT_BIN" --cmd upload --file "$DATA_DIR/documento_b.pdf" --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
pid=$(start_sp 5102 "$LOG_SP2"); SP[1]="$pid"; PIDS+=("$pid")
wait_for_pattern "$LOG_SP2" 'IST: READY' 30 || true
assert_contains "$LOG_SP2" 'IST: SYNCHRONIZING .*versions missing=[1-9]' "IST: SP2 religado identifica versoes faltantes"
assert_contains "$LOG_SP2" 'IST: receive logs' "IST: SP2 recebe apenas os logs faltantes"
assert_contains "$LOG_SP2" 'IST: READY' "IST: SP2 chega ao estado READY"
sleep $((HEARTBEAT_SEC + 2))
assert_contains "$LOG" 'Gossip: SP2 voltou' "Gossip: SP2 religado reconhecido como ALIVE"

# Bully (CP4, Secao 16.9): derruba o coordenador atual e espera a nova eleicao
COORD=$(cat "$LOG" "$LOG_SP2" | grep -Eo 'highest NodeID wins: SP[0-9]' | tail -1 | awk '{print $NF}')
case "$COORD" in
    SP1) idx=0 ;; SP2) idx=1 ;; SP3) idx=2 ;; *) idx=-1 ;;
esac
if [[ "$idx" -lt 0 ]]; then
    fail "Bully: coordenador atual nao identificado no log"
else
    log "Coordenador atual: $COORD — kill -9"
    inicio=$(( $(wc -l < "$LOG") + 1 )); inicio2=$(( $(wc -l < "$LOG_SP2") + 1 ))
    kill -9 "${SP[$idx]}" 2>/dev/null || true
    sleep $((FAILURE_TIMEOUT_SEC + 10))
    cat <(tail -n +"$inicio" "$LOG") <(tail -n +"$inicio2" "$LOG_SP2") > "$LOG_DIR/c6_bully.log"
    assert_contains "$LOG_DIR/c6_bully.log" "SUSPECT $COORD|detect failure: $COORD" "Bully: falha do coordenador $COORD detectada"
    assert_contains "$LOG_DIR/c6_bully.log" 'ELECTION ->' "Bully: ELECTION enviada aos nos de maior prioridade"
    assert_contains "$LOG_DIR/c6_bully.log" 'highest NodeID wins' "Bully: eleicao concluida pelo maior NodeID"
    assert_contains "$LOG_DIR/c6_bully.log" 'COORDINATOR SP' "Bully: novo COORDINATOR anunciado"
fi

assert_no_crash "$LOG"
assert_no_crash "$LOG_SP2"

# 2PC com falha na fase PREPARE (CP5, Secao 16.10): deve abortar sem aplicar
section "Aluno 2: 2PC com falha forcada no PREPARE"
cleanup; PIDS=(); sleep 2
for port in 5101 5102 5103; do
    SIMULATE_PREPARE_FAIL=1 "$NODE_BIN" --config "$CONFIG_DIR/c6.conf" --port "$port" >>"$LOG_2PC" 2>&1 &
    PIDS+=("$!")
done
sleep 8
"$CLIENT_BIN" --cmd upload --file "$DATA_DIR/small.txt" --host 127.0.0.1 --port 5101 >>"$LOG_2PC" 2>&1 || true
assert_contains "$LOG_2PC" 'falha simulada durante a fase PREPARE' "2PC: falha forcada durante o PREPARE"
assert_contains "$LOG_2PC" '2PC \[PREPARING -> ABORT\]' "2PC: transacao abortada"
assert_not_contains "$LOG_2PC" '2PC \[COMMIT -> DONE\]' "2PC: nenhum participante aplicou a transacao abortada"
assert_no_crash "$LOG_2PC"

summary
