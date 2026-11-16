"""Métricas dos Super Peers a partir dos logs espelhados (cluster/logs/) e os gráficos.

As linhas lidas são as que o superpeer.c imprime (as mesmas do inspect.sh):
  NodeID: <hex>   Successor: <SP> (...)   COORDINATOR <SP>   highest NodeID wins: <SP>
  LOG #<n> + PUT <caminho>   LFU CACHE ... hits= misses= evictions=
  Gossip: membro <SP> (...) <ESTADO>   Gossip: <SP> <ESTADO> por <x>   Gossip: <SP> voltou
  SUSPECT|FAILED|REMOVED <SP> (...)   IST: READY (...)
Cada vida de um processo é um arquivo <nome>.<vida>.log; <nome>.log aponta para a atual.
"""
import glob
import math

from matplotlib.ticker import MaxNLocator
import os
import re

TS = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d{3}) ")
LIFE = re.compile(r"\.(\d+)\.log$")

# --- cores (paleta de referência do método de dataviz) ---------------------
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]  # categóricas 1..3: azul, laranja, aqua
STATUS = {"ALIVE": "#0ca30c", "SUSPECT": "#fab219", "FAILED": "#d03b3b", "REMOVED": "#8a2a2a"}
NEUTRAL = "#f0efec"
DEAD = "#b9b8b2"


def _secs(m):
    h, mi, s, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


def read(path):
    """[(segundos_desde_meia_noite | None, texto)] do log espelhado."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            rows = []
            for line in f:
                line = line.rstrip("\n")
                m = TS.match(line)
                rows.append((_secs(m), line[m.end():]) if m else (None, line))
            return rows
    except OSError:
        return []


def _basename(path):
    return path.rsplit("/", 1)[-1]


class SPState:
    def __init__(self, name):
        self.name = name
        self.node_id = ""
        self.succ = ""
        self.pred = ""
        self.coord = ""
        self.log_index = 0
        self.objects = set()
        self.hits = 0
        self.misses = 0
        self.evictions = 0
        self.members = {}
        self.has_log = False


_MEM_ADD = re.compile(r"^Gossip: membro (\S+) \([^)]*\) ([A-Z]+)$")
_MEM_BACK = re.compile(r"^Gossip: (\S+) voltou ")
_MEM_BY = re.compile(r"^Gossip: (\S+) (ALIVE|SUSPECT|FAILED|REMOVED) por ")
_MEM_LOCAL = re.compile(r"^(SUSPECT|FAILED|REMOVED) (\S+) \(")
_NUM = {k: re.compile(k + r"=(\d+)") for k in ("hits", "misses", "evictions")}


def sp_state(name, logs_dir):
    """Estado do Super Peer pela vida atual (logs/<nome>.log)."""
    st = SPState(name)
    rows = read(os.path.join(logs_dir, name + ".log"))
    st.has_log = bool(rows)
    for _, t in rows:
        if t.startswith("NodeID: "):
            st.node_id = t.split()[1]
        elif t.startswith("Successor: "):
            st.succ = t.split()[1]
        elif t.startswith("Predecessor: "):
            st.pred = t.split()[1]
        elif t.startswith("COORDINATOR "):
            st.coord = t.split()[1]
        elif t.startswith("LOG #") and t[5:].isdigit():
            st.log_index = int(t[5:])
        elif t.startswith("PUT "):
            st.objects.add(_basename(t[4:]))
        elif t.startswith("DELETE "):
            st.objects.discard(_basename(t[7:]))  # remoção lógica
        elif t.startswith("LFU CACHE") or t.startswith("LFU Cache stats"):
            for k, rx in _NUM.items():
                m = rx.search(t)
                if m:
                    setattr(st, k, int(m.group(1)))
        else:
            m = _MEM_ADD.match(t)
            if m:
                st.members[m.group(1)] = m.group(2)
                continue
            m = _MEM_BACK.match(t)
            if m:
                st.members[m.group(1)] = "ALIVE"
                continue
            m = _MEM_BY.match(t)
            if m:
                st.members[m.group(1)] = m.group(2)
                continue
            m = _MEM_LOCAL.match(t)
            if m:
                st.members[m.group(2)] = m.group(1)
    return st


def timeline(names, logs_dir):
    """Vidas de cada SP e eventos marcantes, com o tempo em segundos.

    Devolve (lives, events): lives = [(sp, início, fim)], events = [(t, sp, tipo, texto)],
    tipo em SUSPECT, FAILED, WIN, IST.
    """
    lives, events = [], []
    seen_detect = set()
    for sp in names:
        files = sorted(glob.glob(os.path.join(logs_dir, sp + ".*.log")),
                       key=lambda p: int(LIFE.search(p).group(1)) if LIFE.search(p) else 0)
        for path in files:
            rows = [r for r in read(path) if r[0] is not None]
            if not rows:
                continue
            lives.append((sp, rows[0][0], rows[-1][0]))
            for t, txt in rows:
                if txt.startswith("highest NodeID wins: "):
                    events.append((t, txt.split()[-1], "WIN", "coordenador"))
                elif txt.startswith("IST: READY"):
                    events.append((t, sp, "IST", "IST pronto"))
                else:
                    m = _MEM_LOCAL.match(txt)
                    if m and m.group(1) in ("SUSPECT", "FAILED"):
                        key = (m.group(1), m.group(2), int(t) // 30)  # 1ª detecção por janela
                        if key not in seen_detect:
                            seen_detect.add(key)
                            events.append((t, m.group(2), m.group(1), f"{m.group(1)} por {sp}"))
    return lives, events


# --- gráficos ---------------------------------------------------------------

def _style(ax, title):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", fontsize=10, color=INK, pad=8)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8, length=0)


def draw_ring(ax, states, alive):
    """Anel Chord: posição pelo NodeID, seta para o sucessor que cada nó reporta."""
    _style(ax, "Anel Chord (posição = NodeID; seta = sucessor)")
    ax.set_aspect("equal")
    ax.axis("off")
    circle = [(math.cos(a), math.sin(a)) for a in [i * 2 * math.pi / 200 for i in range(201)]]
    ax.plot([p[0] for p in circle], [p[1] for p in circle], color=GRID, lw=2, zorder=1)
    pos, fracs = {}, {}
    for st in states:
        if not st.node_id:
            continue
        frac = int(st.node_id, 16) / float(1 << 256)
        a = math.pi / 2 - 2 * math.pi * frac  # 0 no topo, sentido horário
        pos[st.name] = (math.cos(a), math.sin(a))
        fracs[st.name] = frac
    if not pos:
        ax.text(0, 0, "sem NodeID nos logs ainda", ha="center", va="center", color=INK2, fontsize=9)
        return
    coord = _agreed_coordinator(states, alive)
    for st in states:
        if st.name in pos and st.succ in pos and st.succ != st.name and alive.get(st.name) != "dead":
            ax.annotate("", xy=pos[st.succ], xytext=pos[st.name], zorder=2,
                        arrowprops=dict(arrowstyle="-|>", color=SERIES[0], lw=1.5,
                                        shrinkA=11, shrinkB=11, connectionstyle="arc3,rad=-0.25"))
    # nós fora do ar primeiro (contorno tracejado), vivos por cima: um nó caído que
    # ocupa quase o mesmo ponto de outro continua visível
    ordered = sorted((st for st in states if st.name in pos), key=lambda st: alive.get(st.name) != "dead")
    for st in ordered:
        x, y = pos[st.name]
        if alive.get(st.name) == "dead":
            ax.scatter([x], [y], s=240, facecolors=SURFACE, edgecolors=DEAD, linewidths=2,
                       linestyles=(0, (2, 1.5)), zorder=3)
        else:
            ax.scatter([x], [y], s=150, color=STATUS["ALIVE"], edgecolors=SURFACE, linewidths=2, zorder=4)
            if st.name == coord:
                ax.scatter([x], [y], s=380, facecolors="none", edgecolors=INK, linewidths=1.5, zorder=4)

    # rótulos espalhados: NodeIDs próximos ficariam com os textos um em cima do outro
    for st, la in _spread_labels(states, fracs):
        x, y = pos[st.name]
        dead = alive.get(st.name) == "dead"
        lx, ly = math.cos(la), math.sin(la)
        ax.plot([x * 1.08, lx * 1.24], [y * 1.08, ly * 1.24], color=GRID if not dead else DEAD, lw=0.8, zorder=2)
        label = st.name + (" · coordenador" if st.name == coord and not dead else "") + (" · fora" if dead else "")
        ha = "left" if lx > 0.25 else "right" if lx < -0.25 else "center"
        ax.text(lx * 1.3, ly * 1.3, f"{label}\n{st.node_id[:8]}", ha=ha, va="center",
                fontsize=8, color=INK2 if dead else INK, linespacing=1.3, zorder=5)
    ax.set_xlim(-2.1, 2.1)
    ax.set_ylim(-1.6, 1.6)


def _spread_labels(states, fracs, sep=0.09):
    """Ângulo de cada rótulo: o do nó, afastado dos vizinhos em pelo menos sep (fração do anel)."""
    items = sorted((fracs[st.name], st) for st in states if st.name in fracs)
    if not items:
        return []
    lab = [f for f, _ in items]
    for i in range(1, len(lab)):
        lab[i] = max(lab[i], lab[i - 1] + sep)
    # recentra cada grupo empurrado em volta da posição média dos seus nós
    groups, cur = [], [0]
    for i in range(1, len(lab)):
        if abs(lab[i] - lab[i - 1] - sep) < 1e-9 and lab[i] != items[i][0]:
            cur.append(i)
        else:
            groups.append(cur)
            cur = [i]
    groups.append(cur)
    for g in groups:
        shift = sum(lab[i] - items[i][0] for i in g) / len(g)
        for i in g:
            lab[i] -= shift
    return [(st, math.pi / 2 - 2 * math.pi * lf) for lf, (_, st) in zip(lab, items)]


def _agreed_coordinator(states, alive):
    votes = [st.coord for st in states if st.coord and alive.get(st.name) != "dead"]
    return max(set(votes), key=votes.count) if votes else ""


def draw_logindex(ax, states, alive):
    _style(ax, "LogIndex do SMR por Super Peer")
    names = [st.name for st in states]
    vals = [st.log_index for st in states]
    colors = [DEAD if alive.get(n) == "dead" else SERIES[0] for n in names]
    bars = ax.bar(range(len(names)), vals, color=colors, width=0.55, edgecolor=SURFACE, linewidth=2)
    ax.set_xticks(range(len(names)))
    ax.set_xticklabels([n + ("\n(fora)" if alive.get(n) == "dead" else "") for n in names])
    ax.grid(axis="y", color=GRID, lw=0.8)
    ax.set_axisbelow(True)
    top = max(vals + [1])
    ax.set_ylim(0, top * 1.25)
    ax.yaxis.set_major_locator(MaxNLocator(integer=True))
    for b, st in zip(bars, states):
        ax.text(b.get_x() + b.get_width() / 2, b.get_height() + top * 0.03,
                f"#{st.log_index} · {len(st.objects)} obj", ha="center", fontsize=8, color=INK)
    if len(set(v for v, n in zip(vals, names) if alive.get(n) != "dead")) > 1:
        ax.text(0.99, 0.97, "divergência", transform=ax.transAxes, ha="right", va="top",
                fontsize=8, color=STATUS["FAILED"])


def draw_lfu(ax, states):
    _style(ax, "Cache LFU (hits e misses acumulados)")
    names = [st.name for st in states]
    xs = range(len(names))
    w = 0.36
    ax.bar([x - w / 2 for x in xs], [st.hits for st in states], w, color=SERIES[0], label="hits",
           edgecolor=SURFACE, linewidth=2)
    ax.bar([x + w / 2 for x in xs], [st.misses for st in states], w, color=SERIES[1], label="misses",
           edgecolor=SURFACE, linewidth=2)
    ax.set_xticks(list(xs))
    ax.set_xticklabels(names)
    ax.grid(axis="y", color=GRID, lw=0.8)
    ax.set_axisbelow(True)
    ax.legend(frameon=False, fontsize=8, loc="upper right", labelcolor=INK)
    top = max([st.hits for st in states] + [st.misses for st in states] + [1])
    ax.set_ylim(0, top * 1.25)
    ax.yaxis.set_major_locator(MaxNLocator(integer=True))


def draw_members(ax, states):
    _style(ax, "Membership (linha = quem observa, coluna = membro)")
    names = [st.name for st in states]
    order = {"eu": 0, "-": 1, "ALIVE": 2, "SUSPECT": 3, "FAILED": 4, "REMOVED": 5}
    colors = [NEUTRAL, SURFACE, STATUS["ALIVE"], STATUS["SUSPECT"], STATUS["FAILED"], STATUS["REMOVED"]]
    for i, obs in enumerate(states):
        for j, m in enumerate(names):
            v = "eu" if m == obs.name else obs.members.get(m, "-")
            ax.add_patch(_rect(j, i, colors[order.get(v, 1)]))
            ink = SURFACE if v in ("ALIVE", "FAILED", "REMOVED") else INK
            ax.text(j + 0.5, i + 0.5, v if obs.has_log else "sem log", ha="center", va="center",
                    fontsize=7.5, color=ink)
    ax.set_xlim(0, len(names))
    ax.set_ylim(len(names), 0)
    ax.set_xticks([j + 0.5 for j in range(len(names))])
    ax.set_xticklabels(names)
    ax.set_yticks([i + 0.5 for i in range(len(names))])
    ax.set_yticklabels(names)
    ax.xaxis.tick_top()
    for s in ax.spines.values():
        s.set_visible(False)


def _rect(x, y, color):
    from matplotlib.patches import Rectangle
    return Rectangle((x + 0.03, y + 0.03), 0.94, 0.94, facecolor=color, edgecolor="none")


def draw_timeline(ax, names, logs_dir):
    _style(ax, "Linha do tempo (barra = processo no ar; marcas = eventos)")
    lives, events = timeline(names, logs_dir)
    if not lives:
        ax.text(0.5, 0.5, "sem logs", transform=ax.transAxes, ha="center", color=INK2, fontsize=9)
        ax.set_yticks([])
        return
    t0 = min(s for _, s, _ in lives)
    row = {n: i for i, n in enumerate(names)}
    for sp, s, e in lives:
        ax.barh(row[sp], max(e - s, 0.3), left=s - t0, height=0.42, color=SERIES[0], alpha=0.9)
    marks = {"SUSPECT": ("v", STATUS["SUSPECT"], "SUSPECT"), "FAILED": ("X", STATUS["FAILED"], "FAILED"),
             "WIN": ("*", INK, "venceu a eleição"), "IST": ("D", SERIES[2], "IST pronto")}
    used = set()
    for t, sp, kind, _ in events:
        if sp not in row:
            continue
        mk, col, lab = marks[kind]
        ax.scatter([t - t0], [row[sp]], marker=mk, s=70 if kind == "WIN" else 46, color=col,
                   edgecolors=SURFACE, linewidths=1, zorder=3, label=None if kind in used else lab)
        used.add(kind)
    ax.set_yticks(list(row.values()))
    ax.set_yticklabels(list(row.keys()))
    ax.set_ylim(len(names) - 0.4, -0.6)
    ax.set_xlabel("segundos desde o primeiro log", fontsize=8, color=INK2)
    ax.grid(axis="x", color=GRID, lw=0.8)
    ax.set_axisbelow(True)
    if used:
        ax.legend(frameon=False, fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.24),
                  ncol=4, labelcolor=INK, handletextpad=0.3, columnspacing=1.4)


def draw_all(fig, names, logs_dir, alive):
    """Redesenha os cinco painéis. alive = {SP: 'alive'|'dead'} (vazio = desconhecido)."""
    fig.clear()
    fig.set_facecolor(SURFACE)
    states = [sp_state(n, logs_dir) for n in names]
    gs = fig.add_gridspec(2, 3, height_ratios=[1.15, 1], hspace=0.5, wspace=0.3,
                          left=0.05, right=0.98, top=0.93, bottom=0.16)
    draw_ring(fig.add_subplot(gs[0, 0]), states, alive)
    draw_members(fig.add_subplot(gs[0, 1]), states)
    draw_logindex(fig.add_subplot(gs[0, 2]), states, alive)
    draw_lfu(fig.add_subplot(gs[1, 0]), states)
    draw_timeline(fig.add_subplot(gs[1, 1:]), names, logs_dir)
    return states
