#!/usr/bin/env bash
# Abre a interface gráfica do harness (tkinter + matplotlib).
# Na primeira vez cria gui/.venv e instala o matplotlib (precisa de internet).
# Usa o mesmo cluster.conf do cluster.sh (ou CLUSTER_CONF).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"

"$PY" -c 'import tkinter' 2>/dev/null ||
    { echo "falta o tkinter: sudo apt install python3-tk" >&2; exit 1; }
if [[ ! -x "$DIR/.venv/bin/python" ]]; then
    echo "primeira execução: criando $DIR/.venv e instalando o matplotlib (precisa de internet)"
    "$PY" -m venv "$DIR/.venv" ||
        { echo "falta o venv: sudo apt install python3-venv python3-pip" >&2; exit 1; }
    "$DIR/.venv/bin/pip" install -q -r "$DIR/requirements.txt"
fi
[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "sem tela gráfica (DISPLAY vazio). No WSL, use o WSLg; por ssh, use ssh -X." >&2; exit 1; }
exec "$DIR/.venv/bin/python" "$DIR/app.py" "$@"
