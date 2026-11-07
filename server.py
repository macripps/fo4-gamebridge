"""MCP server for a running Fallout 4: talks to the GameBridge F4SE plugin, and reads the Papyrus logs.

Log tools work with the game closed. Game tools need the game running with GameBridge.dll in Data/F4SE/Plugins.
Needs the mcp package (2.x): pip install mcp
"""
import json
import os
import re
import socket
import threading
import itertools

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError

HOST, PORT = "127.0.0.1", 47830
TIMEOUT = 20
LOGS = os.path.expanduser(r"~\Documents\My Games\Fallout4\Logs\Script")
# Workshop Framework's ThreadRunner01..50 are consecutive forms from 0x001737 in WorkshopFramework.esm. The load-order
# prefix differs per setup: set WSFW_PREFIX to WorkshopFramework.esm's console prefix (default 10).
FIRST_RUNNER = int(os.environ.get("WSFW_PREFIX", "10"), 16) << 24 | 0x001737
RUNNER_COUNT = 50
RUNNER_SCRIPT = "workshopframework:mainthreadrunner"

mcp = MCPServer("fallout4")
_lock = threading.Lock()
_ids = itertools.count(1)
_sock = None


def _call(*parts):
    """One request to the plugin. Raises with a plain message if the game isn't reachable."""
    global _sock
    request_id = str(next(_ids))
    line = "\t".join((request_id,) + parts) + "\n"
    with _lock:
        for attempt in (1, 2):  # the first send can fail on a connection left over from a previous game run
            try:
                if _sock is None:
                    _sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
                _sock.settimeout(TIMEOUT)
                _sock.sendall(line.encode("cp1252", "replace"))
                data = b""
                while not data.endswith(b"\n"):
                    chunk = _sock.recv(65536)
                    if not chunk:
                        raise ConnectionError("connection closed")
                    data += chunk
                break
            except OSError as e:
                if _sock is not None:
                    _sock.close()
                _sock = None
                if attempt == 2:
                    raise ToolError(f"can't reach the game on {HOST}:{PORT} ({e}). Is the game running with GameBridge loaded?")
    reply = json.loads(data.decode("ascii"))
    if not reply["ok"]:
        raise ToolError(reply["result"])
    return reply["result"]


def _log_path(name):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise ValueError("log name is a file name such as WSFWRunnerTrace.0.log or Papyrus.0.log")
    for folder in (os.path.join(LOGS, "User"), LOGS):
        path = os.path.join(folder, name)
        if os.path.exists(path):
            return path
    raise FileNotFoundError(name)


def _read_vars(form_id, script):
    out = {}
    for line in _call("vars", form_id, script).splitlines():
        key, _, value = line.partition(" = ")
        out[key] = value
    return out


@mcp.tool()
def game_status() -> str:
    """Check the game is running with the bridge loaded. Answers on the plugin's own thread, so it works even when
    Papyrus is jammed."""
    return _call("ping")


@mcp.tool()
def game_console(command: str) -> str:
    """Run one console command on the game's main thread. Commands act on the running game. Only the command's echo
    comes back, not its output: check the effect with read_log (e.g. a script's Debug.TraceUser log) or
    game_script_vars. Use "prid <ref>" first for commands that need a selected reference."""
    return _call("console", command)


@mcp.tool()
def game_script_vars(form_id: str, script: str) -> dict:
    """Read every variable of a script attached to a form, including private ones, e.g. form_id "10001761",
    script "workshopframework:mainthreadrunner". Values are live."""
    return _read_vars(form_id, script)


@mcp.tool()
def wsfw_runners() -> list:
    """State of WSFW's 50 thread runners. A runner with count above 0 and no running thread is stuck."""
    rows = []
    for i in range(RUNNER_COUNT):
        form_id = f"{FIRST_RUNNER + i:08X}"
        values = _read_vars(form_id, RUNNER_SCRIPT)
        count = int(values.get("iRunningThreads", "0"))
        running = values.get("kRunningThread", "None")
        pump = values.get("bQueuePumpTimerRunning", "False")
        if count or pump == "True":
            # index is 0-based (as in the bug report); WSFW's own names start at 01
            rows.append({"index": i, "name": f"WSFW_ThreadRunner{i + 1:02d}", "form": form_id, "count": count, "running_thread": running, "pump_armed": pump,
                         "stuck": count > 0 and running == "None"})
    return rows


@mcp.tool()
def papyrus_stacks() -> str:
    """Dump the running Papyrus stacks to Papyrus.0.log using the engine's own command, then return what it wrote."""
    path = _log_path("Papyrus.0.log")
    start = os.path.getsize(path)
    printed = _call("console", "DumpPapyrusStacks")
    with open(path, encoding="cp1252", errors="replace") as f:
        f.seek(start)
        return printed + f.read()


@mcp.tool()
def read_log(name: str, tail: int = 200, grep: str = "") -> str:
    """Last lines of a Papyrus log, optionally only lines matching a regex. Names are file names, e.g. Papyrus.0.log
    or WSFWRunnerTrace.0.log. Works with the game closed."""
    with open(_log_path(name), encoding="cp1252", errors="replace") as f:
        lines = f.readlines()
    if grep:
        pattern = re.compile(grep)
        lines = [line for line in lines if pattern.search(line)]
    return "".join(lines[-tail:])


if __name__ == "__main__":
    mcp.run()
