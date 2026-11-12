"""MCP server for a running Fallout 4: talks to the GameBridge F4SE plugin, and reads the Papyrus logs.

Log and screenshot tools work with the game closed. Game tools need the game running with GameBridge.dll in
Data/F4SE/Plugins. Needs the mcp package (2.x): pip install mcp. Pillow (optional) shrinks screenshots.
Environment: WSFW_PREFIX (see below), FO4_SCREENSHOTS (folder the game's screenshots land in, e.g. Steam's
userdata/<id>/760/remote/377160/screenshots), FO4_GAME_SCREENSHOTS (the folder in the game's own sScreenShotBaseName,
for game_screenshot).
"""
import glob
import io
import json
import os
import re
import socket
import threading
import itertools
import time

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

HOST, PORT = "127.0.0.1", 47830
TIMEOUT = 20
LOGS = os.path.expanduser(r"~\Documents\My Games\Fallout4\Logs\Script")
# Workshop Framework's ThreadRunner01..50 are consecutive forms from 0x001737 in WorkshopFramework.esm. The load-order
# prefix differs per setup: set WSFW_PREFIX to WorkshopFramework.esm's console prefix (default 10).
FIRST_RUNNER = int(os.environ.get("WSFW_PREFIX", "10"), 16) << 24 | 0x001737
RUNNER_COUNT = 50
RUNNER_SCRIPT = "workshopframework:mainthreadrunner"
SCREENSHOTS = os.environ.get("FO4_SCREENSHOTS", "")
SCREENSHOT_WIDTH = 1600

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
def game_player() -> str:
    """The player's cell, position (x y z) and rotation in degrees, live."""
    return _call("player")


@mcp.tool()
def game_ref(form_id: str) -> str:
    """One reference, live: base object and its form type, cell, position, rotation, disabled/deleted/3D-loaded, and
    every linked ref (keyword -> target), including links a script added at runtime. form_id is the load-order
    FormID in hex, e.g. "15004365" or a spawned "FF0020A3". Editor IDs show only where the game keeps them."""
    return _call("ref", form_id)


@mcp.tool()
def game_actor(form_id: str) -> str:
    """One actor, live: life state, disabled, talking to the player, the AI package it is running (with the
    package's target), the furniture it is using or heading to (sitting = using a chair or sit marker), and the items
    it has equipped. form_id is the load-order FormID of the actor reference (not its base NPC). An actor whose 3D
    isn't loaded may show a stale position, and far-away actors have no furniture data."""
    return _call("actor", form_id)


@mcp.tool()
def game_refs_near(radius: float, form_type: str = "", x: float | None = None, y: float | None = None,
                   z: float | None = None) -> str:
    """References within radius of a point in the player's cell, nearest first (up to 300). The point defaults to
    the player. form_type filters by the base's type, e.g. "LIGH", "STAT", "CONT", "ACTI", "FURN". Finds objects a
    script spawned (FF... FormIDs); follow up with game_ref."""
    parts = ["near", str(radius), form_type]
    if x is not None and y is not None and z is not None:
        parts += [str(x), str(y), str(z)]
    return _call(*parts)


@mcp.tool()
def game_place(form_id: str, distance: float = 200.0) -> str:
    """Place a new reference of a base object (load-order FormID in hex, e.g. "16000801") distance units in front of
    the player, upright and turned the way the player faces. Returns the new reference (an FF... FormID) and where it
    is; move or delete it with game_console ("prid <id>", "setpos z ...", "markfordelete")."""
    return _call("place", form_id, str(distance))


@mcp.tool()
def game_quest(quest: str) -> str:
    """One quest, live: running/completed/failed flags, the current stage (the last one set), every objective with
    its state (dormant, displayed, completed...) and text, every alias with its flags and the reference it holds, and
    the stage events GameBridge has seen for it this session. quest is the editor ID (e.g. "AIMUS_MoSS_Unlock") or the
    load-order FormID in hex. Which earlier stages are done isn't readable; see game_quest_events."""
    return _call("quest", quest)


@mcp.tool()
def game_quest_events(count: int = 50) -> str:
    """The latest quest events from the engine (up to 500 since the game started, across loads), oldest first, with
    the time: "stage N (log entry M)", "started", "stopped". They come from the engine, not Papyrus, so they show stages
    set even when no script hears them; a stage with no log entry may send no event at all."""
    return _call("questlog", str(count))


@mcp.tool()
def game_global(name: str) -> str:
    """A global variable's current value, by editor ID (e.g. "AIMUS_MoSS_ActionAvailable_CleanRoom_3rdFloorSmallOffice")
    or load-order FormID in hex. Set one with game_console ("set <name> to <value>")."""
    return _call("global", name)


@mcp.tool()
def game_screenshot() -> list:
    """Take a game screenshot (as the screenshot key does) and return it. Needs FO4_GAME_SCREENSHOTS set to the
    folder in the game's sScreenShotBaseName (Fallout4Prefs.ini, [Display]), and bAllowScreenshot=1."""
    folder = os.environ.get("FO4_GAME_SCREENSHOTS", "")
    if not folder or not os.path.isdir(folder):
        raise ToolError("set FO4_GAME_SCREENSHOTS to the folder in the game's sScreenShotBaseName")
    pattern = [os.path.join(folder, "*.png"), os.path.join(folder, "*.jpg"), os.path.join(folder, "*.bmp")]
    before = {p: os.path.getmtime(p) for g in pattern for p in glob.glob(g)}
    _call("screenshot")
    deadline = time.time() + 10
    while time.time() < deadline:
        time.sleep(0.5)
        new = [p for g in pattern for p in glob.glob(g) if before.get(p) != os.path.getmtime(p)]
        if new:
            path = max(new, key=os.path.getmtime)
            time.sleep(0.5)  # let the game finish writing it
            return [os.path.basename(path), _image(path)]
    raise ToolError(f"no new screenshot in {folder} after 10 s (is the game window in front?)")


@mcp.tool()
def game_perf(seconds: float = 3.0) -> str:
    """Frame rate and frame times (mean, median, 99th percentile, worst, in ms) over a few seconds (0.5 to 10) of
    play, from the engine's own frame timer. Stand still and keep the view steady while it samples, and compare runs
    from the same spot and facing (e.g. a room before and after its design is built)."""
    return _call("perf", str(seconds))


@mcp.tool()
def latest_screenshots(count: int = 1) -> list:
    """The newest game screenshots (from FO4_SCREENSHOTS), newest first, shrunk to 1600 px wide when Pillow is
    installed. Works with the game closed."""
    if not SCREENSHOTS or not os.path.isdir(SCREENSHOTS):
        raise ToolError("set FO4_SCREENSHOTS to the folder the game's screenshots are saved in")
    files = sorted(glob.glob(os.path.join(SCREENSHOTS, "*.jpg")) + glob.glob(os.path.join(SCREENSHOTS, "*.png")),
                   key=os.path.getmtime, reverse=True)[:max(1, min(count, 6))]
    if not files:
        raise ToolError(f"no screenshots in {SCREENSHOTS}")
    out = []
    for path in files:
        out += [os.path.basename(path), _image(path)]
    return out


def _image(path):
    """An image for the client, shrunk to SCREENSHOT_WIDTH as JPEG when Pillow is installed."""
    try:
        from PIL import Image as PILImage
    except ImportError:
        return Image(path=path)
    with PILImage.open(path) as im:
        if im.width > SCREENSHOT_WIDTH:
            im = im.resize((SCREENSHOT_WIDTH, round(im.height * SCREENSHOT_WIDTH / im.width)))
        buffer = io.BytesIO()
        im.convert("RGB").save(buffer, "JPEG", quality=85)
    return Image(data=buffer.getvalue(), format="jpeg")


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
