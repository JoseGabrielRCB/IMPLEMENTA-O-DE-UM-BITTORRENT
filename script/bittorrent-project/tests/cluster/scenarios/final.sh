# Roteiro do CP6: uma fase por item da lista do professor, com os Super Peers e
# clientes espalhados pelas máquinas do cluster.conf.
#   Aluno 2: Chord, Gossip, Bully, SMR, 2PC, IST
#   Aluno 1: upload, download, chunks, compression, cache, replica
#
# As conferências usam as linhas que o superpeer.c e o peer.c imprimem de fato
# (LOG #n + PUT <caminho>, 2PC [...], IST: ..., LFU CACHE ..., SUSPECT <SP> (...)).

N=${#NODE_LIST[@]}
((N >= 3)) || die "o cenário final precisa de pelo menos 3 Super Peers (há $N)"
NAMES_ABC=(documento_a.pdf documento_b.pdf documento_c.pdf)
NAMES_ALL=(documento_a.pdf documento_b.pdf documento_c.pdf documento_d.pdf documento_e.pdf)

# regex de "PUT <caminho>/<nome>" (o caminho do upload vai inteiro para o log)
put_re() { printf '^PUT (.*/)?%s$' "${1//./\\.}"; }

phase "Preparo: arquivos de teste nas máquinas"
cl fixtures >/dev/null || die "fixtures falhou"
phase_done

phase "Bully e Chord: boot de $N Super Peers em ${#HOST_LIST[@]} máquina(s)"
boot_cluster
phase_done "coordenador $COORD"

phase "Clientes persistentes: ${#CLIENT_LIST[@]} abertos"
clients_start || die "clientes não subiram"
C1="${CLIENT_LIST[0]}"
E1="${CLIENT_SP[$C1]}"
C_OTHER="$(other_host_client "$C1")"
phase_done "$C1 entra por $E1; $C_OTHER em outra máquina"

# O nó que cai não pode ser o coordenador nem a porta de entrada de C1.
VICTIM=""
for n in "${NODE_LIST[@]}"; do
    [[ "$n" != "$COORD" && "$n" != "$E1" ]] && { VICTIM="$n"; break; }
done
[[ -n "$VICTIM" ]] || die "sem Super Peer disponível para derrubar"

phase "SMR e 2PC: uploads de A, B e C por $C1 (YES esperados: $((N - 1)))"
for l in a b c; do
    cdo "$C1" upload "$(client_file "$C1" "documento_${l}.pdf")"
done
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
assert_round "$(node_log "$COORD")" documento_a.pdf $((N - 1)) 1
assert_round "$(node_log "$COORD")" documento_b.pdf $((N - 1)) 2
assert_round "$(node_log "$COORD")" documento_c.pdf $((N - 1)) 3
for n in "${NODE_LIST[@]}"; do
    [[ "$n" == "$COORD" ]] && continue
    assert_log_sequence "$(node_log "$n")" "${NAMES_ABC[@]}"
done
for n in "${NODE_LIST[@]}"; do
    cl_assert_state "$n" 3 3 "${NAMES_ABC[@]}"
done
phase_done "mesmo log e mesmo estado nos $N nós"

phase "Chord: lookup pelo ObjectID de A"
# o upload imprime "ObjectID:" e o hex na linha seguinte
OID="$(client_output "$C1" 1 | awk '/^ObjectID:$/ { getline; print; exit }')"
[[ "${#OID}" -eq 64 ]] || die "não achei o ObjectID de A na saída do upload de $C1"
cdo "$C1" lookup "$OID"
client_log_has "$C1" "Lookup path:" || die "lookup sem 'Lookup path:'"
OWNER="$(client_output "$C1" "$(client_sent "$C1")" | sed -n 's/^Owner: //p' | tail -n1)"
[[ -n "${NODE_HOST[$OWNER]:-}" ]] || die "dono '$OWNER' não é um Super Peer do cluster"
say "ObjectID $OID pertence a $OWNER ($(node_endpoint "$OWNER"))"
phase_done "dono: $OWNER"

phase "Gossip e detecção: kill -9 de $VICTIM"
DETECT_RE="^((SUSPECT|FAILED) ${VICTIM} \\(|Gossip: ${VICTIM} (SUSPECT|FAILED) por )"
KILL_MS="$(now_ms)"
node_kill "$VICTIM"
wait_log "$(node_log "$COORD")" "$DETECT_RE" 40 "$COORD marcar $VICTIM como SUSPECT ou FAILED"
SUSPECT_MS="$(stamp_ms "$(node_log "$COORD")" "$DETECT_RE")"
[[ -n "$SUSPECT_MS" ]] && metric "detecção (kill até SUSPECT no coordenador)" "$(fmt_s $((SUSPECT_MS - KILL_MS)))" "~15 s (heartbeat 5 s, timeout 15 s)"
phase_done

phase "2PC com um participante fora: uploads de D e E (YES esperados: $((N - 2)))"
for l in d e; do
    cdo "$C1" upload "$(client_file "$C1" "documento_${l}.pdf")"
done
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
assert_round "$(node_log "$COORD")" documento_d.pdf $((N - 2)) 4
assert_round "$(node_log "$COORD")" documento_e.pdf $((N - 2)) 5
assert_name_absent "$(node_log "$VICTIM")" documento_d.pdf
assert_name_absent "$(node_log "$VICTIM")" documento_e.pdf
for n in "${NODE_LIST[@]}"; do
    [[ "$n" == "$VICTIM" ]] && continue
    cl_assert_state "$n" 5 5 "${NAMES_ALL[@]}"
done
phase_done

phase "IST: $VICTIM volta e recebe A até E"
node_start "$VICTIM" >/dev/null || die "não consegui subir $VICTIM de novo"
wait_log "$(node_log "$VICTIM")" '^IST: READY \(sincronizado ate LOG #5\)' 60 "IST de $VICTIM chegar ao LOG #5"
assert_lines_in_order "$(node_log "$VICTIM")" \
    '^IST: SYNCHRONIZING \(local version=0, coordinator version=5,' \
    '^IST: receive logs \(1 \.\. 5\)' \
    '^LOG #1$' "$(put_re documento_a.pdf)" '^LOG #2$' "$(put_re documento_b.pdf)" \
    '^LOG #3$' "$(put_re documento_c.pdf)" '^LOG #4$' "$(put_re documento_d.pdf)" \
    '^LOG #5$' "$(put_re documento_e.pdf)" \
    '^IST: READY'
IST_START="$(stamp_ms "$(node_log "$VICTIM")" '^IST: SYNCHRONIZING')"
IST_END="$(stamp_ms "$(node_log "$VICTIM")" '^IST: READY')"
[[ -n "$IST_START" && -n "$IST_END" ]] && metric "IST (SYNCHRONIZING até READY)" "$(fmt_s $((IST_END - IST_START)))" "< 2 s (RNF-008)"
cl_assert_state "$VICTIM" 5 5 "${NAMES_ALL[@]}"
phase_done

phase "Cache LFU em $E1: duas consultas de A"
# O cache do nó pode já ter A (o PUT insere em alguns nós), então a conferência é relativa:
# as duas consultas geram dois eventos novos em $E1 e a última é um HIT com frequência >= 2.
LFU_RE='^LFU CACHE (HIT|MISS): documento_a\.pdf '
LFU_BEFORE="$(count_lines "$(node_log "$E1")" "$LFU_RE")"
cdo "$C1" lookup documento_a.pdf
cdo "$C1" lookup documento_a.pdf
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
wait_count "$(node_log "$E1")" "$LFU_RE" $((LFU_BEFORE + 1)) 10 "dois eventos LFU de A em $E1"
LFU_LAST="$(logical_log <"$(node_log "$E1")" | grep -E "$LFU_RE" | tail -n1)"
[[ "$LFU_LAST" =~ ^LFU\ CACHE\ HIT:\ documento_a\.pdf\ \(frequency=([0-9]+), ]] && ((BASH_REMATCH[1] >= 2)) ||
    die "a segunda consulta de A em $E1 não foi HIT com frequência >= 2: $LFU_LAST"
say "$E1: $LFU_LAST"
for n in "${NODE_LIST[@]}"; do
    [[ "$n" == "$E1" ]] && continue
    assert_pattern_absent "$(node_log "$n")" '^LFU CACHE HIT: documento_a\.pdf '
done
phase_done "${LFU_LAST#LFU CACHE }"

phase "Download: A por $C1 e por $C_OTHER (outra máquina), comparando o sha256"
download_and_check "$C1" documento_a.pdf a-c1.out documento_a.pdf
download_and_check "$C_OTHER" documento_a.pdf a-other.out documento_a.pdf
client_log_has "$C_OTHER" "SHA-256 verified" || die "download sem 'SHA-256 verified'"
phase_done

phase "Chunks e compressão: arquivo de 9 MiB por $C_OTHER"
cdo "$C_OTHER" upload "$(client_file "$C_OTHER" documento_big.pdf)"
client_log_has "$C_OTHER" "Chunks: 3" || die "upload de 9 MiB sem 'Chunks: 3'"
client_log_has "$C_OTHER" "Compression: LZ4" || die "upload sem 'Compression: LZ4'"
download_and_check "$C1" documento_big.pdf big-c1.out documento_big.pdf
phase_done "3 chunks de 4 MiB, sha256 idêntico"

phase "Réplica: derruba $OWNER (dono de A) e baixa A de novo"
REPL_CLIENT=""
for c in "${CLIENT_LIST[@]}"; do
    [[ "${CLIENT_SP[$c]}" != "$OWNER" ]] && { REPL_CLIENT="$c"; break; }
done
[[ -n "$REPL_CLIENT" ]] || die "todo cliente entra por $OWNER"
ALIVE=()
for n in "${NODE_LIST[@]}"; do
    [[ "$n" != "$OWNER" ]] && ALIVE+=("$n")
done
WINS_BEFORE="$(count_any '^highest NodeID wins: ' "${ALIVE[@]}")"
KILL_MS="$(now_ms)"
node_kill "$OWNER"
if [[ "$OWNER" == "$COORD" ]]; then
    say "$OWNER era o coordenador: esperando a nova eleição"
    wait_any_new '^highest NodeID wins: ' "$WINS_BEFORE" 90 "algum Super Peer vivo vencer a nova eleição" "${ALIVE[@]}"
    metric "reeleição (kill do coordenador até a vitória)" "$(fmt_s $(($(now_ms) - KILL_MS)))" "detecção ~15 s + eleição < 5 s"
fi
sleep 3
download_and_check "$REPL_CLIENT" documento_a.pdf a-replica.out documento_a.pdf
phase_done "A baixado por $REPL_CLIENT sem $OWNER"
