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


def _loads():
    """(load count, reply text) from the plugin, or None if the game isn't reachable."""
    try:
        text = _call("loads")
    except ToolError:
        return None
    return int(text.splitlines()[0].split()[1]), text


@mcp.tool()
def game_wait_for_load(timeout: float = 900.0, next: bool = False) -> str:
    """Wait until a save is loaded, then return the save's name and time. If one is already loaded, returns at once;
    next=True waits for the next load instead (e.g. after asking for a different save). Waits for the game to start
    first if it isn't running. Use it instead of asking the user to say when they've loaded. Polls every 2 s, up to
    timeout seconds."""
    start = _loads()
    if start and start[0] > 0 and not next:
        return start[1] + "\n(already loaded)"
    baseline = start[0] if start else 0
    deadline = time.monotonic() + timeout
    was_up = start is not None
    while time.monotonic() < deadline:
        time.sleep(2)
        now = _loads()
        if now is None:
            if was_up:
                baseline, was_up = 0, False  # the game quit: after a restart the count starts again at 0
            continue
        was_up = True
        if now[0] > baseline:
            return now[1]
    raise ToolError(f"no save loaded within {timeout:.0f} s")


@mcp.tool()
def game_move(form_id: str, to: str = "", offset: str = "", rotation: str = "") -> str:
    """Move and/or turn a reference, as the console's prid + setpos + setangle in one step (the selection can't change
    in between; 3D and collision update). to: "x,y,z" absolute. offset: "forward,right,up" in the reference's own
    frame (by its rotation Z), applied after to, e.g. "0,0,5" raises it 5, "-3,0,0" moves it 3 back. rotation: "x,y,z"
    degrees. Returns the position and rotation before and after. Like setpos, the change is kept in the save."""
    return _call("move", form_id, to or "-", offset or "-", rotation or "-")


@mcp.tool()
def game_crosshair(range: float = 1000.0) -> str:
    """What the view ray hits first, as the game's own pick would: casts from the camera (the free camera in free
    mode, otherwise the player's aim) up to range units through the physics world, skipping the player's own
    collision. Returns the hit point, its distance, the 3D node hit and the reference that owns it. Use it when something can't be activated: if the hit isn't the activator, that reference's
    collision is in front of it. Third person: the ray starts at the camera but follows the player's aim."""
    return _call("crosshair", str(range))


@mcp.tool()
def game_camera(action: str = "status") -> str:
    """The free camera (as the console's tfc). action: "status" (camera state; in free mode its position, yaw and
    pitch in degrees, and the move running), "free" (detach the camera from the player; needed before any move),
    "stop" (end the move running, the camera stays put), "release" (camera back to the player). The player doesn't
    move: in exteriors only cells already loaded around the player are drawn. Points in the other camera tools are
    "x,y,z" or a reference's load-order FormID. Moves return at once and run in game; follow with game_screenshot."""
    return _call("camera", action)


@mcp.tool()
def game_camera_fly(to: str, seconds: float, look_at: str = "") -> str:
    """Glide the free camera to a point over seconds, eased at both ends. look_at (a point or reference) keeps that in
    view along the way; without it the view keeps its direction."""
    parts = ["camera", "fly", to, str(seconds)]
    if look_at:
        parts.append(look_at)
    return _call(*parts)


@mcp.tool()
def game_camera_look(at: str, seconds: float = 0.0) -> str:
    """Turn the free camera to face a point or reference, at once or smoothly over seconds."""
    return _call("camera", "look", at, str(seconds))


@mcp.tool()
def game_camera_orbit(center: str, radius: float, height: float, seconds: float, turns: float = 1.0,
                      aim_height: float = 0.0, start_angle: float | None = None) -> str:
    """Circle the free camera round a point or reference, always facing it: radius and height (above the centre) in
    game units, turns (negative = anticlockwise) over seconds, looking at the centre raised by aim_height (e.g. half
    a statue's height). It starts from the camera's own bearing (or start_angle, degrees from north) and eases onto
    the circle, so there's no jump."""
    parts = ["camera", "orbit", center, str(radius), str(height), str(seconds), str(turns), str(aim_height)]
    if start_angle is not None:
        parts.append(str(start_angle))
    return _call(*parts)


@mcp.tool()
def game_camera_path(points: list[str], seconds: float, look_at: str = "") -> str:
    """Fly the free camera along a smooth curve from where it is through points (each "x,y,z" or a reference) over
    seconds, at an even speed, eased at both ends. It faces along the path, or towards look_at if given."""
    return _call("camera", "path", str(seconds), look_at or "-", *points)


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
