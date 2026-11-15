#!/usr/bin/env bash
set -u
source "$(dirname "$0")/../common.sh"

section "C6 — Integracao e Teste Final (Aluno 1 + Aluno 2)"
require_bin "$NODE_BIN" || exit 1
require_bin "$CLIENT_BIN" || exit 1
"$TEST_ROOT/generate_fixtures.sh" >/dev/null

LOG="$LOG_DIR/c6.log"
: > "$LOG"
rm -f "$LOG_DIR/downloaded_c6.pdf"

for port in 5101 5102 5103; do
    "$NODE_BIN" --config "$CONFIG_DIR/c6.conf" --port "$port" >>"$LOG" 2>&1 &
    PIDS+=("$!")
done
sleep 8

# 1) Upload de arquivo multi-chunk (10 MiB -> 3 chunks de 4 MiB) -> 2PC + SMR
"$CLIENT_BIN" --cmd upload --file "$DATA_DIR/documento_c.pdf" --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true

# 2) Lookup repetido (LFU Cache MISS -> HIT com incremento de frequency)
"$CLIENT_BIN" --cmd lookup --name documento_c.pdf --host 127.0.0.1 --port 5102 >>"$LOG" 2>&1 || true
"$CLIENT_BIN" --cmd lookup --name documento_c.pdf --host 127.0.0.1 --port 5102 >>"$LOG" 2>&1 || true

# 3) Download paralelo distribuido entre replicas + verificacao SHA-256 e LZ4
"$CLIENT_BIN" --cmd download --name documento_c.pdf --output "$LOG_DIR/downloaded_c6.pdf" --host 127.0.0.1 --port 5103 >>"$LOG" 2>&1 || true

# 4) JOIN/IST sincronizacao incremental
"$CLIENT_BIN" --cmd join --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true

# Verificacoes do Aluno 1 (upload, download, chunks, compression, cache, replica)
assert_contains "$LOG" 'Chunks: 3' "Aluno 1: Fragmentacao em 3 chunks de 4 MiB"
assert_contains "$LOG" 'Compression: LZ4' "Aluno 1: Compressao LZ4 ativa"
assert_contains "$LOG" 'LFU CACHE HIT' "Aluno 1: Cache LFU funcionando (MISS -> HIT)"
assert_contains "$LOG" 'DOWNLOAD_REP replica' "Aluno 1: Download paralelo distribuido entre replicas"
assert_contains "$LOG" 'SHA-256 verified' "Aluno 1: Reconstituicao e verificacao SHA-256"

# Verificacoes do Aluno 2 (Chord, Gossip, Bully, SMR, 2PC, IST)
assert_contains "$LOG" 'Successor:|Finger table:' "Aluno 2: Anel Chord estabilizado"
assert_contains "$LOG" 'ALIVE' "Aluno 2: Gossip e Heartbeat ativos"
assert_contains "$LOG" 'Coordinator|COORDINATOR' "Aluno 2: Eleicao Bully concluida"
assert_contains "$LOG" 'LOG #1' "Aluno 2: Replicacao SMR (LOG #1)"
assert_contains "$LOG" '2PC.*PREPARE|RX PREPARE' "Aluno 2: Consenso 2PC (PREPARE -> COMMIT)"
assert_contains "$LOG" 'IST|STATE_TRANSFER' "Aluno 2: Incremental State Transfer (IST)"
assert_no_crash "$LOG"
summary
