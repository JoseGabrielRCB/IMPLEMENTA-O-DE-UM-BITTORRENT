"""Ponte com o harness: roda cluster.sh / run.sh e lê a topologia.

A interface não sabe nada do sistema distribuído; tudo passa pelo cluster.sh
(que fala ssh com as máquinas) e pelos logs espelhados em cluster/logs/.
"""
import os
import re
import subprocess
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
CLUSTER_DIR = os.path.dirname(HERE)
CLUSTER_SH = os.environ.get("CLUSTER_SH", os.path.join(CLUSTER_DIR, "cluster.sh"))
RUN_SH = os.path.join(CLUSTER_DIR, "run.sh")
LOGS_DIR = os.path.join(CLUSTER_DIR, "logs")
OUT_DIR = os.path.join(CLUSTER_DIR, "out")
SCENARIOS_DIR = os.path.join(CLUSTER_DIR, "scenarios")
FILES_DIR = os.path.join(OUT_DIR, "files")
REPORT = os.path.join(OUT_DIR, "relatorio.md")

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def conf_path():
    """cluster.conf em uso (CLUSTER_CONF ou o que fica ao lado do cluster.sh)."""
    return os.environ.get("CLUSTER_CONF", os.path.join(CLUSTER_DIR, "cluster.conf"))


def run(args, timeout=60):
    """Roda cluster.sh <args> e devolve (rc, saída sem cores)."""
    try:
        p = subprocess.run(["bash", CLUSTER_SH, *args], cwd=CLUSTER_DIR, capture_output=True,
                           text=True, timeout=timeout, stdin=subprocess.DEVNULL)
        return p.returncode, ANSI.sub("", p.stdout + p.stderr)
    except subprocess.TimeoutExpired:
        return 124, f"tempo esgotado ({timeout}s): cluster.sh {' '.join(args)}\n"
    except OSError as e:
        return 127, f"não consegui rodar {CLUSTER_SH}: {e}\n"


class Job:
    """Processo em segundo plano com a saída entregue linha a linha.

    on_line(texto) e on_end(rc) são chamados na thread de leitura; a interface
    repassa para a thread do Tk por uma fila.
    """

    def __init__(self, argv, on_line, on_end, env=None):
        self.argv = argv
        self.proc = None
        self._on_line = on_line
        self._on_end = on_end
        self._env = env

    def start(self):
        env = dict(os.environ)
        if self._env:
            env.update(self._env)
        try:
            # setsid: um Parar mata o grupo inteiro (ssh, tail, filhos do cenário)
            self.proc = subprocess.Popen(self.argv, cwd=CLUSTER_DIR, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                         text=True, bufsize=1, env=env, start_new_session=True)
        except OSError as e:
            self._on_line(f"não consegui rodar {' '.join(self.argv)}: {e}")
            self._on_end(127)
            return self
        threading.Thread(target=self._pump, daemon=True).start()
        return self

    def _pump(self):
        for line in self.proc.stdout:
            self._on_line(ANSI.sub("", line.rstrip("\n")))
        self._on_end(self.proc.wait())

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.running():
            try:
                os.killpg(self.proc.pid, 15)
            except OSError:
                pass


def cluster_job(args, on_line, on_end, env=None):
    return Job(["bash", CLUSTER_SH, *args], on_line, on_end, env).start()


def scenario_job(name, on_line, on_end, keep=False):
    env = {"CLUSTER_KEEP": "1"} if keep else None
    return Job(["bash", RUN_SH, name], on_line, on_end, env).start()


def topology():
    """Super Peers e hosts do cluster.conf, sem ssh (cluster.sh list).

    Devolve (sps, hosts, erro): sps = [(nome, host, ip:porta)], hosts = [apelido].
    """
    rc, out = run(["list"], timeout=20)
    if rc != 0:
        return [], [], out.strip() or f"cluster.sh list saiu com {rc}"
    sps, hosts, sec = [], [], None
    for line in out.splitlines():
        if line.startswith("Hosts"):
            sec = "h"
        elif line.startswith("Super Peers"):
            sec = "s"
        elif line.startswith("Clientes") or not line.startswith("  "):
            sec = None
        elif sec == "h":
            hosts.append(line.split()[0])
        elif sec == "s":
            f = line.split()
            if len(f) >= 3:
                sps.append((f[0], f[1], f[2]))
    return sps, hosts, ""


def _table(out, first_col_re):
    rows = []
    for line in out.splitlines():
        f = line.split()
        if f and re.match(first_col_re, f[0]) and f[0] != "NOME":
            rows.append(f)
    return rows


def clients():
    """[(nome, host, estado, superpeer)] pelo cluster.sh client status (usa ssh)."""
    rc, out = run(["client", "status"], timeout=60)
    rows = [(f[0], f[1], f[2], f[4] if len(f) > 4 else "") for f in _table(out, r"^\S+$")
            if len(f) >= 4 and f[2] in ("alive", "dead")]
    return rows, ("" if rc == 0 else out.strip())


def nodes_status():
    """{SP: 'alive'|'dead'} pelo cluster.sh status (usa ssh)."""
    rc, out = run(["status"], timeout=60)
    st = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 4 and f[3] in ("alive", "dead") and f[0] != "NOME":
            st[f[0]] = f[3]
        if line.strip() == "":
            break  # a primeira tabela é a dos Super Peers
    return st


def scenarios():
    try:
        return sorted(n[:-3] for n in os.listdir(SCENARIOS_DIR) if n.endswith(".sh"))
    except OSError:
        return []


def test_files():
    """Arquivos de teste gerados pelo fixtures (out/files); nas máquinas ficam em run/files."""
    try:
        return sorted(os.listdir(FILES_DIR))
    except OSError:
        return []
