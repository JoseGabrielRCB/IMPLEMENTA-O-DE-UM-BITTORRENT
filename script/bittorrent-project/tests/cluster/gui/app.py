"""Interface gráfica do harness distribuído (tkinter + matplotlib).

Abas:
  Cliente   upload, download, lookup, state, ping/join/leave e comando livre em um cliente (C1...)
  Cluster   preparo (check, deploy...), subir/derrubar, status e injeção de falha nos Super Peers
  Métricas  anel Chord, membership, LogIndex, LFU e linha do tempo, atualizando sozinhos
  Cenários  roda scenarios/*.sh e mostra o relatório
  Logs      acompanha o log espelhado de um processo, com filtro por categoria

Tudo passa pelo cluster.sh (cluster.py); os gráficos leem os logs em cluster/logs (metrics.py).
"""
import os
import queue
import re
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

import matplotlib

matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg  # noqa: E402
from matplotlib.figure import Figure  # noqa: E402

import cluster  # noqa: E402
import metrics  # noqa: E402

MONO = ("DejaVu Sans Mono", 9)
PAD = {"padx": 6, "pady": 4}

# categorias do Logs (as mesmas do cluster.sh events)
LOG_FILTERS = {
    "tudo": "",
    "sem ruído": None,  # tratado à parte: esconde heartbeat, RX e finger table
    "membership": r"^(SUSPECT|FAILED|REMOVED|Gossip: membro|Gossip: \S+ (voltou|ALIVE|SUSPECT|FAILED|REMOVED)|JOIN|LEAVE|detect failure)",
    "eleição": r"^(start election|ELECTION|OK <-|COORDINATOR|highest NodeID|Coordenador|sem COORDINATOR|Bully|resume operations)",
    "chord": r"^(Chord|Successor|Predecessor|Lookup)",
    "2pc": r"^2PC",
    "ist": r"^IST",
    "lfu": r"^LFU",
    "log SMR": r"^(LOG #|PUT |SMR)",
    "chunks": r"^(CHUNK_STORE|DOWNLOAD_REP)",
    "cliente": r"^(\[done|\[ready|\[bye|Erro|SHA-256|Download|Compression|Chunks:|Owner:)",
}
NOISE = re.compile(r"^(Gossip: heartbeat|RX |  finger\[|Finger table)")
TS = re.compile(r"^\d\d:\d\d:\d\d\.\d{3} ")


class Console(ttk.Frame):
    """Área de texto só de leitura, com rolagem e limite de linhas."""

    def __init__(self, master, height=12, max_lines=5000):
        super().__init__(master)
        self.max_lines = max_lines
        self.text = tk.Text(self, height=height, font=MONO, wrap="none", state="disabled",
                            background="#fcfcfb", foreground="#0b0b0b", relief="flat",
                            borderwidth=1, highlightthickness=1, highlightbackground="#d8d7d2")
        ys = ttk.Scrollbar(self, orient="vertical", command=self.text.yview)
        xs = ttk.Scrollbar(self, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=ys.set, xscrollcommand=xs.set)
        self.text.tag_configure("cmd", foreground="#2a78d6", font=MONO + ("bold",))
        self.text.tag_configure("err", foreground="#d03b3b")
        self.text.tag_configure("ok", foreground="#008300")
        self.text.grid(row=0, column=0, sticky="nsew")
        ys.grid(row=0, column=1, sticky="ns")
        xs.grid(row=1, column=0, sticky="ew")
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)

    def append(self, line, tag=None):
        at_end = self.text.yview()[1] >= 0.999
        self.text.configure(state="normal")
        if tag is None:
            if re.search(r"\bFAIL|FALHA|[Ee]rro|falhou|rc=[1-9]", line):
                tag = "err"
            elif re.search(r"\[PASS\]|rc=0\b|\bok\b", line):
                tag = "ok"
        self.text.insert("end", line + "\n", tag or ())
        extra = int(self.text.index("end-1c").split(".")[0]) - self.max_lines
        if extra > 0:
            self.text.delete("1.0", f"{extra + 1}.0")
        self.text.configure(state="disabled")
        if at_end:
            self.text.see("end")

    def set(self, content):
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.insert("end", content)
        self.text.configure(state="disabled")
        self.text.see("end")

    def clear(self):
        self.set("")


class JobRunner:
    """Um processo por vez numa aba, com a saída indo para um Console."""

    def __init__(self, app, console, on_done=None):
        self.app = app
        self.console = console
        self.on_done = on_done
        self.job = None

    def busy(self):
        return self.job is not None and self.job.running()

    def start(self, kind, args, title, **kw):
        if self.busy():
            messagebox.showinfo("Aguarde", "Ainda há um comando rodando nesta aba (use Parar).")
            return False
        self.console.append(f"$ {title}", "cmd")
        t0 = time.time()

        def line(s):
            self.app.ui(self.console.append, s)

        def end(rc):
            def fin():
                tag = "ok" if rc == 0 else "err"
                self.console.append(f"[terminou com rc={rc} em {time.time() - t0:.1f} s]", tag)
                if self.on_done:
                    self.on_done(rc)
            self.app.ui(fin)

        if kind == "scenario":
            self.job = cluster.scenario_job(args, line, end, **kw)
        else:
            self.job = cluster.cluster_job(args, line, end, **kw)
        return True

    def stop(self):
        if self.busy():
            self.job.stop()
            self.console.append("[Parar: sinal enviado]", "err")


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("BitTorrent CP6 — harness distribuído")
        self.geometry("1280x820")
        self.minsize(980, 640)
        self.configure(background="#fcfcfb")
        style = ttk.Style(self)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("TNotebook.Tab", padding=(14, 6))
        style.configure("Hint.TLabel", foreground="#52514e")

        self._q = queue.Queue()
        self.sps, self.hosts, self.client_rows = [], [], []
        self.runners = []

        top = ttk.Frame(self)
        top.pack(fill="x", **PAD)
        ttk.Label(top, text=f"cluster.conf: {cluster.conf_path()}", style="Hint.TLabel").pack(side="left")
        self.topo_label = ttk.Label(top, text="lendo topologia…", style="Hint.TLabel")
        self.topo_label.pack(side="right")

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=6, pady=(0, 6))
        self.nb = nb
        self._build_client(nb)
        self._build_cluster(nb)
        self._build_metrics(nb)
        self._build_scenarios(nb)
        self._build_logs(nb)

        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(80, self._drain)
        self.reload_topology()

    # --- infraestrutura -----------------------------------------------------

    def ui(self, fn, *args):
        """Agenda fn(*args) na thread do Tk (seguro a partir de outras threads)."""
        self._q.put((fn, args))

    def _drain(self):
        try:
            for _ in range(500):
                fn, args = self._q.get_nowait()
                fn(*args)
        except queue.Empty:
            pass
        self.after(80, self._drain)

    def bg(self, fn, done):
        """Roda fn() numa thread e entrega o resultado a done() na thread do Tk."""
        threading.Thread(target=lambda: self.ui(done, fn()), daemon=True).start()

    def runner(self, console, on_done=None):
        r = JobRunner(self, console, on_done)
        self.runners.append(r)
        return r

    def _close(self):
        if any(r.busy() for r in self.runners):
            if not messagebox.askyesno("Sair", "Há comandos rodando. Parar todos e sair?"):
                return
            for r in self.runners:
                r.stop()
        self.destroy()

    def reload_topology(self):
        def done(res):
            sps, hosts, err = res
            self.sps, self.hosts = sps, hosts
            names = [s[0] for s in sps]
            if err:
                self.topo_label.configure(text="cluster.conf com problema (veja a aba Cluster)")
                self.cluster_console.append(err, "err")
            else:
                self.topo_label.configure(text=f"{len(hosts)} host(s) · {len(sps)} Super Peer(s)")
            self.sp_combo.configure(values=names)
            if names and not self.sp_combo.get():
                self.sp_combo.set(names[0])
            self._refresh_log_choices()
            self.refresh_metrics()
        self.bg(cluster.topology, done)

    # --- aba Cliente ----------------------------------------------------------

    def _build_client(self, nb):
        f = ttk.Frame(nb)
        nb.add(f, text="Cliente")

        row = ttk.Frame(f)
        row.pack(fill="x", **PAD)
        ttk.Label(row, text="Cliente:").pack(side="left")
        self.client_combo = ttk.Combobox(row, width=10, state="readonly")
        self.client_combo.pack(side="left", padx=4)
        self.client_combo.bind("<<ComboboxSelected>>", lambda e: self._client_info())
        ttk.Button(row, text="Atualizar lista", command=self.reload_clients).pack(side="left", padx=4)
        self.client_info = ttk.Label(row, text="clique em Atualizar lista (consulta as máquinas por ssh)",
                                     style="Hint.TLabel")
        self.client_info.pack(side="left", padx=8)

        ops = ttk.LabelFrame(f, text="Operações")
        ops.pack(fill="x", **PAD)
        ops.columnconfigure(1, weight=1)

        ttk.Label(ops, text="Upload").grid(row=0, column=0, sticky="w", **PAD)
        self.up_file = ttk.Combobox(ops)
        self.up_file.grid(row=0, column=1, sticky="ew", **PAD)
        self.up_file.bind("<Button-1>", lambda e: self.up_file.configure(values=cluster.test_files()))
        ttk.Button(ops, text="Enviar", command=self._upload).grid(row=0, column=2, **PAD)
        ttk.Label(ops, text="arquivo de out/files (nas máquinas: ../files/) ou caminho no host do cliente",
                  style="Hint.TLabel").grid(row=0, column=3, sticky="w", **PAD)

        ttk.Label(ops, text="Download").grid(row=1, column=0, sticky="w", **PAD)
        dl = ttk.Frame(ops)
        dl.grid(row=1, column=1, sticky="ew", **PAD)
        self.dl_name = ttk.Entry(dl)
        self.dl_name.pack(side="left", fill="x", expand=True)
        ttk.Label(dl, text=" saída:").pack(side="left")
        self.dl_out = ttk.Entry(dl, width=22)
        self.dl_out.pack(side="left")
        ttk.Button(ops, text="Baixar", command=self._download).grid(row=1, column=2, **PAD)
        ttk.Label(ops, text="nome do arquivo; a saída fica em run/<cliente>/ (vazia = <nome>.out)",
                  style="Hint.TLabel").grid(row=1, column=3, sticky="w", **PAD)

        ttk.Label(ops, text="Lookup").grid(row=2, column=0, sticky="w", **PAD)
        self.lk = ttk.Entry(ops)
        self.lk.grid(row=2, column=1, sticky="ew", **PAD)
        ttk.Button(ops, text="Buscar", command=lambda: self._client_cmd("lookup", self.lk.get().strip())
                   ).grid(row=2, column=2, **PAD)
        ttk.Label(ops, text="nome (metadado + LFU) ou ObjectID de 64 hex (caminho Chord)",
                  style="Hint.TLabel").grid(row=2, column=3, sticky="w", **PAD)

        ttk.Label(ops, text="Comando").grid(row=3, column=0, sticky="w", **PAD)
        self.free = ttk.Entry(ops)
        self.free.grid(row=3, column=1, sticky="ew", **PAD)
        self.free.bind("<Return>", lambda e: self._client_cmd(*self.free.get().split()))
        ttk.Button(ops, text="Enviar", command=lambda: self._client_cmd(*self.free.get().split())
                   ).grid(row=3, column=2, **PAD)
        ttk.Label(ops, text="qualquer comando da sessão (help lista)", style="Hint.TLabel"
                  ).grid(row=3, column=3, sticky="w", **PAD)

        quick = ttk.Frame(f)
        quick.pack(fill="x", **PAD)
        for label, cmd in (("state (topologia)", "state"), ("ping", "ping"), ("join", "join"),
                           ("leave", "leave"), ("help", "help")):
            ttk.Button(quick, text=label, command=lambda c=cmd: self._client_cmd(c)).pack(side="left", padx=3)
        ttk.Button(quick, text="Limpar", command=lambda: self.client_console.clear()).pack(side="right", padx=3)

        self.client_console = Console(f, height=18)
        self.client_console.pack(fill="both", expand=True, **PAD)
        self.client_runner = self.runner(self.client_console)

    def reload_clients(self):
        self.client_info.configure(text="consultando…")

        def done(res):
            rows, err = res
            self.client_rows = rows
            names = [r[0] for r in rows]
            self.client_combo.configure(values=names)
            if names and self.client_combo.get() not in names:
                self.client_combo.set(names[0])
            if err and not rows:
                self.client_info.configure(text="não consegui listar (o cluster está no ar?)")
                self.client_console.append(err, "err")
            else:
                self._client_info()
            self._refresh_log_choices()
        self.bg(cluster.clients, done)

    def _client_info(self):
        name = self.client_combo.get()
        for r in self.client_rows:
            if r[0] == name:
                txt = f"{r[0]} em {r[1]} · {'aberto' if r[2] == 'alive' else 'fechado'} · entra por {r[3]}"
                if r[2] != "alive":
                    txt += "  (abra os clientes na aba Cluster)"
                self.client_info.configure(text=txt)
                return
        self.client_info.configure(text="")

    def _client_cmd(self, *cmd):
        cmd = [c for c in cmd if c]
        name = self.client_combo.get()
        if not name:
            messagebox.showinfo("Cliente", "Escolha um cliente (Atualizar lista).")
            return
        if not cmd:
            return
        self.client_runner.start("cluster", ["client", "do", name, *cmd], f"{name}> {' '.join(cmd)}")

    def _upload(self):
        f = self.up_file.get().strip()
        if not f:
            return
        self._client_cmd("upload", f if "/" in f else f"../files/{f}")

    def _download(self):
        name = self.dl_name.get().strip()
        if not name:
            return
        out = self.dl_out.get().strip() or f"{name.rsplit('/', 1)[-1]}.out"
        self._client_cmd("download", name, out)

    # --- aba Cluster ----------------------------------------------------------

    def _build_cluster(self, nb):
        f = ttk.Frame(nb)
        nb.add(f, text="Cluster")

        groups = ttk.Frame(f)
        groups.pack(fill="x", **PAD)

        def group(title, buttons):
            g = ttk.LabelFrame(groups, text=title)
            g.pack(side="left", fill="y", padx=4)
            for i, (label, args) in enumerate(buttons):
                ttk.Button(g, text=label, width=16,
                           command=lambda a=args, t=label: self._cluster_cmd(a)
                           ).grid(row=i // 2, column=i % 2, padx=3, pady=3)

        group("Preparo", [("list", ["list"]), ("check", ["check"]), ("net", ["net"]),
                          ("deploy", ["deploy"]), ("fixtures", ["fixtures"]), ("clean", ["clean"])])
        group("Execução", [("up", ["up"]), ("up + clientes", ["up", "--clients"]),
                           ("down", ["down"]), ("status", ["status", "--clients"]),
                           ("abrir clientes", ["client", "start"]), ("fechar clientes", ["client", "stop"])])

        fail = ttk.LabelFrame(groups, text="Falhas em Super Peer")
        fail.pack(side="left", fill="y", padx=4)
        self.sp_combo = ttk.Combobox(fail, width=10, state="readonly")
        self.sp_combo.grid(row=0, column=0, columnspan=2, padx=3, pady=3, sticky="ew")
        for i, (label, op) in enumerate((("kill -9", "kill"), ("stop", "stop"),
                                         ("start", "start"), ("restart", "restart"))):
            ttk.Button(fail, text=label, width=10,
                       command=lambda o=op: self._sp_op(o)).grid(row=1 + i // 2, column=i % 2, padx=3, pady=3)

        side = ttk.Frame(groups)
        side.pack(side="right", fill="y", padx=4)
        ttk.Button(side, text="Parar", command=lambda: self.cluster_runner.stop()).pack(fill="x", pady=3)
        ttk.Button(side, text="Recarregar conf", command=self.reload_topology).pack(fill="x", pady=3)
        ttk.Button(side, text="Limpar", command=lambda: self.cluster_console.clear()).pack(fill="x", pady=3)

        self.cluster_console = Console(f, height=24)
        self.cluster_console.pack(fill="both", expand=True, **PAD)
        self.cluster_runner = self.runner(self.cluster_console, on_done=lambda rc: self.refresh_metrics())

    def _cluster_cmd(self, args):
        if args and args[0] == "down" and not messagebox.askyesno("down", "Derrubar o cluster inteiro?"):
            return
        self.cluster_runner.start("cluster", args, "cluster.sh " + " ".join(args))

    def _sp_op(self, op):
        sp = self.sp_combo.get()
        if not sp:
            return
        self.cluster_runner.start("cluster", [op, sp], f"cluster.sh {op} {sp}")

    # --- aba Métricas ---------------------------------------------------------

    def _build_metrics(self, nb):
        f = ttk.Frame(nb)
        nb.add(f, text="Métricas")

        bar = ttk.Frame(f)
        bar.pack(fill="x", **PAD)
        ttk.Button(bar, text="Atualizar", command=self.refresh_metrics).pack(side="left")
        self.auto = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="automático a cada", variable=self.auto).pack(side="left", padx=(12, 2))
        self.interval = tk.IntVar(value=5)
        ttk.Spinbox(bar, from_=2, to=60, width=4, textvariable=self.interval).pack(side="left")
        ttk.Label(bar, text="s").pack(side="left", padx=(2, 12))
        self.use_ssh = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="consultar processos vivos (ssh)", variable=self.use_ssh).pack(side="left")
        self.metrics_label = ttk.Label(bar, text="", style="Hint.TLabel")
        self.metrics_label.pack(side="right")

        pw = ttk.PanedWindow(f, orient="vertical")
        pw.pack(fill="both", expand=True, **PAD)
        holder = ttk.Frame(pw)
        self.fig = Figure(figsize=(12, 6.4), dpi=90)
        self.canvas = FigureCanvasTkAgg(self.fig, master=holder)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)
        pw.add(holder, weight=4)

        insp = ttk.Frame(pw)
        ib = ttk.Frame(insp)
        ib.pack(fill="x")
        ttk.Label(ib, text="Inspeção em texto:").pack(side="left", padx=(0, 6))
        for label, args in (("overview", ["overview"]), ("members", ["members"]),
                            ("chord + fingers", ["chord", "--fingers"]), ("coordinator", ["coordinator"]),
                            ("state + arquivos", ["state", "--files"]), ("metadata", ["metadata"])):
            ttk.Button(ib, text=label, command=lambda a=args: self.inspect_runner.start(
                "cluster", a, "cluster.sh " + " ".join(a))).pack(side="left", padx=2)
        self.inspect_console = Console(insp, height=8)
        self.inspect_console.pack(fill="both", expand=True, pady=(4, 0))
        self.inspect_runner = self.runner(self.inspect_console)
        pw.add(insp, weight=1)

        self._metrics_busy = False
        self.after(1000, self._metrics_tick)

    def _metrics_tick(self):
        try:
            every = max(2, int(self.interval.get()))
        except (tk.TclError, ValueError):
            every = 5
        if self.auto.get() and str(self.nb.select()) == str(self.nb.tabs()[2]):
            self.refresh_metrics()
        self.after(every * 1000, self._metrics_tick)

    def refresh_metrics(self):
        if self._metrics_busy or not self.sps:
            return
        self._metrics_busy = True
        names = [s[0] for s in self.sps]
        ssh = self.use_ssh.get()

        def done(alive):
            try:
                metrics.draw_all(self.fig, names, cluster.LOGS_DIR, alive)
                self.canvas.draw_idle()
                src = "logs + ssh" if ssh else "só logs"
                self.metrics_label.configure(text=f"atualizado às {time.strftime('%H:%M:%S')} ({src})")
            except Exception as e:  # um log estranho não pode derrubar a interface
                self.metrics_label.configure(text=f"erro ao desenhar: {e}")
            finally:
                self._metrics_busy = False
        self.bg(lambda: cluster.nodes_status() if ssh else {}, done)

    # --- aba Cenários ---------------------------------------------------------

    def _build_scenarios(self, nb):
        f = ttk.Frame(nb)
        nb.add(f, text="Cenários")

        bar = ttk.Frame(f)
        bar.pack(fill="x", **PAD)
        ttk.Label(bar, text="Cenário:").pack(side="left")
        names = cluster.scenarios()
        self.scn = ttk.Combobox(bar, values=names, width=14, state="readonly")
        if "smoke" in names:
            self.scn.set("smoke")
        self.scn.pack(side="left", padx=4)
        self.keep = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="deixar o cluster de pé no fim (CLUSTER_KEEP)",
                        variable=self.keep).pack(side="left", padx=8)
        ttk.Label(bar, text="WAVE (carga):").pack(side="left")
        self.wave = ttk.Entry(bar, width=4)
        self.wave.insert(0, "10")
        self.wave.pack(side="left", padx=4)
        ttk.Button(bar, text="Rodar", command=self._run_scenario).pack(side="left", padx=8)
        ttk.Button(bar, text="Parar", command=lambda: self.scn_runner.stop()).pack(side="left")
        ttk.Button(bar, text="Ver relatório", command=self._load_report).pack(side="right")

        pw = ttk.PanedWindow(f, orient="horizontal")
        pw.pack(fill="both", expand=True, **PAD)
        left = ttk.Frame(pw)
        ttk.Label(left, text="Saída").pack(anchor="w")
        self.scn_console = Console(left, height=30)
        self.scn_console.pack(fill="both", expand=True)
        pw.add(left, weight=3)
        right = ttk.Frame(pw)
        ttk.Label(right, text="out/relatorio.md").pack(anchor="w")
        self.report = Console(right, height=30)
        self.report.pack(fill="both", expand=True)
        pw.add(right, weight=2)
        self.scn_runner = self.runner(self.scn_console, on_done=lambda rc: (self._load_report(),
                                                                            self.refresh_metrics()))

    def _run_scenario(self):
        name = self.scn.get()
        if not name:
            return
        env = {"WAVE": self.wave.get().strip() or "10"}
        if self.keep.get():
            env["CLUSTER_KEEP"] = "1"
        self.scn_console.clear()
        # o cenário mexe no cluster inteiro: os gráficos acompanham pela aba Métricas
        if self.scn_runner.busy():
            messagebox.showinfo("Aguarde", "Já há um cenário rodando.")
            return
        self.scn_console.append(f"$ run.sh {name}", "cmd")
        t0 = time.time()

        def line(s):
            self.ui(self.scn_console.append, s)

        def end(rc):
            def fin():
                self.scn_console.append(f"[cenário {name} terminou com rc={rc} em {time.time() - t0:.0f} s]",
                                        "ok" if rc == 0 else "err")
                self._load_report()
                self.refresh_metrics()
            self.ui(fin)
        self.scn_runner.job = cluster.Job(["bash", cluster.RUN_SH, name], line, end, env).start()

    def _load_report(self):
        try:
            with open(cluster.REPORT, encoding="utf-8") as fh:
                self.report.set(fh.read())
        except OSError:
            self.report.set("(ainda não há out/relatorio.md)")

    # --- aba Logs -------------------------------------------------------------

    def _build_logs(self, nb):
        f = ttk.Frame(nb)
        nb.add(f, text="Logs")

        bar = ttk.Frame(f)
        bar.pack(fill="x", **PAD)
        ttk.Label(bar, text="Processo:").pack(side="left")
        self.log_proc = ttk.Combobox(bar, width=10, state="readonly")
        self.log_proc.pack(side="left", padx=4)
        self.log_proc.bind("<<ComboboxSelected>>", lambda e: self._log_reload())
        ttk.Label(bar, text="Filtro:").pack(side="left", padx=(10, 0))
        self.log_filter = ttk.Combobox(bar, values=list(LOG_FILTERS), width=12)
        self.log_filter.set("sem ruído")
        self.log_filter.pack(side="left", padx=4)
        self.log_filter.bind("<<ComboboxSelected>>", lambda e: self._log_reload())
        self.log_filter.bind("<Return>", lambda e: self._log_reload())
        ttk.Label(bar, text="(ou digite uma regex e Enter)", style="Hint.TLabel").pack(side="left")
        self.follow = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="acompanhar", variable=self.follow).pack(side="left", padx=10)
        ttk.Button(bar, text="Recarregar", command=self._log_reload).pack(side="right")

        self.log_console = Console(f, height=30, max_lines=20000)
        self.log_console.pack(fill="both", expand=True, **PAD)
        self._log_state = {"path": None, "real": None, "pos": 0, "rx": None, "noise": False}
        self.after(1000, self._log_tick)

    def _refresh_log_choices(self):
        names = [s[0] for s in self.sps] + [r[0] for r in self.client_rows]
        try:
            for fn in sorted(os.listdir(cluster.LOGS_DIR)):
                m = re.fullmatch(r"([^.]+)\.log", fn)
                if m and m.group(1) not in names:
                    names.append(m.group(1))
        except OSError:
            pass
        self.log_proc.configure(values=names)
        if names and not self.log_proc.get():
            self.log_proc.set(names[0])
            self._log_reload()

    def _log_reload(self):
        name = self.log_proc.get()
        if not name:
            return
        flt = self.log_filter.get()
        st = self._log_state
        st["path"] = os.path.join(cluster.LOGS_DIR, name + ".log")
        st["real"] = None
        st["pos"] = 0
        st["noise"] = flt == "sem ruído"
        pattern = LOG_FILTERS.get(flt, flt)
        try:
            st["rx"] = re.compile(pattern) if pattern else None
        except re.error as e:
            messagebox.showerror("Filtro", f"regex inválida: {e}")
            st["rx"] = None
        self.log_console.clear()
        self._log_read()

    def _log_read(self):
        st = self._log_state
        path = st["path"]
        if not path:
            return
        real = os.path.realpath(path)
        if real != st["real"]:  # nova vida do processo: o link aponta para outro arquivo
            if st["real"] is not None:
                self.log_console.append(f"--- nova vida: {os.path.basename(real)} ---", "cmd")
            st["real"], st["pos"] = real, 0
        try:
            with open(real, encoding="utf-8", errors="replace") as fh:
                fh.seek(st["pos"])
                chunk = fh.read()
                st["pos"] = fh.tell()
        except OSError:
            return
        for line in chunk.splitlines():
            body = TS.sub("", line)
            if st["noise"] and NOISE.match(body):
                continue
            if st["rx"] is not None and not st["rx"].search(body):
                continue
            self.log_console.append(line, "")

    def _log_tick(self):
        if self.follow.get():
            self._log_read()
        self.after(1000, self._log_tick)


def main():
    App().mainloop()


if __name__ == "__main__":
    main()
