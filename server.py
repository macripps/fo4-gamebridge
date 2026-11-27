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
def _documents():
    """The user's Documents folder, wherever it has been moved to (the game follows a moved Documents too)."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders") as key:
            return os.path.expandvars(winreg.QueryValueEx(key, "Personal")[0])
    except OSError:
        return os.path.expanduser(r"~\Documents")


LOGS = os.path.join(_documents(), r"My Games\Fallout4\Logs\Script")
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


def _read_vars(form_id, script, variable=""):
    out = {}
    for line in _call("vars", form_id, script, *([variable] if variable else [])).splitlines():
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
def game_script_vars(form_id: str, script: str, variable: str = "") -> dict:
    """Read every variable of a script attached to a form, including private ones, e.g. form_id "10001761",
    script "workshopframework:mainthreadrunner". Values are live; a form shows as [script form=FormID]. With variable
    (its name, or a property's name for "::Name_var"), only that one, and an array's elements as name[0], name[1]..."""
    return _read_vars(form_id, script, variable)


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
def game_ref_holds(form_id: str) -> str:
    """What keeps a reference alive (a deleted reference is only freed once nothing persists it): form flags (deleted,
    persistent, disabled) and engine refcount; the Papyrus persist count on its handle (a script variable, array,
    event registration or alias holding it); each script bound to it with the script object's raw refcount word (the
    VM's own table holds one); the quest aliases holding it; every reference linking to it (keyword <- ref); and the
    type of each extra data on it. form_id: load-order FormID in hex, e.g. "FF01E7FE"."""
    return _call("holds", form_id)


@mcp.tool()
def game_ref_holds_near(radius: float, deleted_only: bool = True, form_type: str = "", save_to: str = "") -> str:
    """game_ref_holds for every reference within radius of the player (default: deleted ones only), one tab-separated
    line each: FormID, base, form flags, engine refs, persist count, scripts (name:refcount word), aliases, linked-from
    (count:FormIDs), extra data types. For finding what blocks a settlement's deleted leftovers. save_to: write the
    rows to a file and return a summary of persist counts, scripts, aliases and links."""
    text = _call("holdsnear", str(radius), "1" if deleted_only else "0", form_type)
    if not save_to:
        return text
    with open(save_to, "w", encoding="utf-8") as f:
        f.write(text)
    rows = [r.split("\t") for r in text.splitlines()[1:]]
    def count(i, empty):
        return sum(1 for r in rows if len(r) > i and r[i].split(" ", 1)[1] != empty)
    return (f"{len(rows)} references; wrote to {save_to}. With a persist count: {count(4, '-')}, with scripts: "
            f"{count(5, '-')}, in an alias: {count(6, '-')}, linked from another ref: {count(7, '-')}")


@mcp.tool()
def game_ref_demote(form_id: str, owner: str = "") -> str:
    """Dev-only fix prototype: removes one promoter from a reference with the engine's
    BGSDynamicPersistenceManager::DemoteReference, the reference itself unless owner (a FormID in hex) is given.
    Reports the holds before and after. For SS2 leftovers that are deleted and promoted only by themselves. Changes
    the running game: use on a test profile, and don't save unless that's the point."""
    return _call("demote", form_id, *([owner] if owner else []))


@mcp.tool()
def game_ref_demote_near(radius: float) -> str:
    """Dev-only fix prototype: game_ref_demote (owner = itself) on every deleted reference within radius of the player
    that lists itself as a promoter. Returns one line per reference with its state afterwards."""
    return _call("demotenear", str(radius))


@mcp.tool()
def game_ref_demote_all() -> str:
    """Dev-only fix prototype: game_ref_demote (owner = itself) on every deleted, self-promoted reference in memory,
    the whole save (persistent references are always loaded), not just nearby cells. Reports how many were released
    and the bases released most. Changes the running game: test profile; save under a new name to measure."""
    return _call("demoteall")


@mcp.tool()
def game_promote_hook(action: str = "read", count: int = 200, all_calls: bool = False) -> str:
    """Dev-only: a detour on the engine's PromoteReference that records who promotes references to persistent.
    action "on" installs it (once per game run; it checks the function's first bytes and refuses if they differ) and
    starts recording self-promotions (a reference promoted by itself), or every promotion with all_calls; "off" stops
    recording; "clear" empties the buffer (last 3000 kept); "read" returns the last count records: time, thread, ref,
    base, owner, then a native backtrace as module+offset (map with tools/promotetrace.py)."""
    if action == "on":
        return _call("promotehook", "on", *(["all"] if all_calls else []))
    if action == "read":
        return _call("promotehook", "read", str(count))
    return _call("promotehook", action)


@mcp.tool()
def game_actor(form_id: str) -> str:
    """One actor, live: life state, disabled, talking to the player, the AI package it is running (with the
    package's target), the furniture it is using or heading to (sitting = using a chair or sit marker), and the items
    it has equipped. form_id is the load-order FormID of the actor reference (not its base NPC). An actor whose 3D
    isn't loaded may show a stale position, and far-away actors have no furniture data."""
    return _call("actor", form_id)


@mcp.tool()
def game_refs_near(radius: float, form_type: str = "", x: float | None = None, y: float | None = None,
                   z: float | None = None, enabled_only: bool = False, save_to: str = "") -> str:
    """References within radius of a point in the player's cell, nearest first (up to 2000). The point defaults to
    the player. form_type filters by the base's type, e.g. "LIGH", "STAT", "CONT", "ACTI", "FURN". enabled_only skips
    disabled and deleted references (old plot stages leave many disabled ones). Each row gives position, rotation
    (degrees), scale and the base's bounds (OBND: min x y z, max x y z, object space, unscaled), so footprints can be
    drawn. Finds objects a script spawned (FF... FormIDs); follow up with game_ref. save_to: a file path; the rows go
    there (appended) and only a summary comes back, for big sweeps."""
    parts = ["near", str(radius), form_type]
    if x is not None and y is not None and z is not None:
        parts += [str(x), str(y), str(z)]
    else:
        parts += ["", "", ""]
    parts.append("1" if enabled_only else "0")
    text = _call(*parts)
    if not save_to:
        return text
    lines = text.splitlines()
    with open(save_to, "a", encoding="utf-8") as f:
        f.write("\n".join(lines[1:]) + "\n")
    rows = lines[1:]
    return (f"{lines[0] if lines else '0 references'}; wrote {len(rows)} rows to {save_to} "
            f"({sum(1 for r in rows if 'DISABLED' in r)} disabled, {sum(1 for r in rows if 'DELETED' in r)} deleted)")


@mcp.tool()
def game_scripted_refs(script: str, variables: str, radius: float = 6000.0, x: float | None = None,
                       y: float | None = None, z: float | None = None, enabled_only: bool = True,
                       save_to: str = "") -> str:
    """Every reference within radius of a point (default: the player) carrying a Papyrus script, nearest first (up to
    500), each with the script variables named in variables (comma-separated; "X" also matches a property's
    "::X_var"). Form values print as the form's name ("0A2B3C4D EditorID [Plugin] "Name""). One call replaces a
    game_refs_near plus a game_script_vars per reference. save_to: write the rows to a file, return a summary."""
    parts = ["scripted", str(radius), script, variables]
    parts += [str(x), str(y), str(z)] if x is not None and y is not None and z is not None else ["", "", ""]
    parts.append("1" if enabled_only else "0")
    text = _call(*parts)
    if not save_to:
        return text
    with open(save_to, "w", encoding="utf-8") as f:
        f.write(text)
    return text.splitlines()[0] + f"; wrote to {save_to}"


def _batch_lines(path):
    with open(path, encoding="utf-8") as f:
        return sum(1 for line in f if line.strip() and not line.startswith("#"))


def _batch(op, list_path, record_path, chunk=100):
    total, out = _batch_lines(list_path), []
    for start in range(0, total, chunk):
        out.append(_call(op, list_path, record_path, str(start), str(chunk)).strip())
    return f"{total} lines\n" + "\n".join(out)


@mcp.tool()
def game_place_batch(list_path: str, record_path: str) -> str:
    """Place many objects at once, for mock-ups. list_path: one line per object, "FORMID x y z rx ry rz scale"
    (load-order FormID in hex, angles in degrees; blank lines and # comments skipped). Placed in the player's
    worldspace (or interior) as plain references, 100 per game frame. Each new reference is appended to record_path
    as "placed <ref>", so game_undo_batch removes exactly those. Place statics, not scripted objects (no workshop link)."""
    return _batch("placebatch", list_path, record_path)


@mcp.tool()
def game_disable_batch(list_path: str, record_path: str) -> str:
    """Disable existing references listed one per line (load-order FormID first), appending "disabled <ref>" to
    record_path so game_undo_batch enables them again. For a mock-up's scrap list. Precombined references stay drawn
    while their precombine is intact."""
    return _batch("disablebatch", list_path, record_path)


@mcp.tool()
def game_undo_batch(record_path: str) -> str:
    """Undo a record from game_place_batch / game_disable_batch: placed references are disabled and marked for delete,
    disabled ones enabled again."""
    total, out = _batch_lines(record_path), []
    for pass_ in ("0", "1"):  # disable/enable, then delete: one game frame each
        for start in range(0, total, 200):
            out.append(_call("undobatch", record_path, str(start), "200", pass_).strip())
    return f"{total} records\n" + "\n".join(out)


SS2_PLOT_SCRIPT ="SimSettlementsV2:ObjectReferences:SimPlot"
SS2_PLOT_VARS = "ParentBuildingPlan,CurrentLevelPlan,iCurrentLevel,iOccupants,bIsPlotActive,bUnderConstruction"


@mcp.tool()
def game_plots(radius: float = 6000.0, x: float | None = None, y: float | None = None, z: float | None = None,
               save_to: str = "") -> str:
    """Sim Settlements 2 plots round a point (default: the player), enabled ones only: for each, its plot type and
    position, building plan, current level plan (None = nothing built yet), level, assigned occupants, and whether it's
    active or under construction. game_scripted_refs with SS2's SimPlot script; works only with SS2 loaded."""
    return game_scripted_refs(SS2_PLOT_SCRIPT, SS2_PLOT_VARS, radius, x, y, z, True, save_to)


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
def game_av(target: str, actor_values: list[str]) -> str:
    """Live actor values on a reference, not only actors: workbenches keep their resources in them (food, water,
    defense). Sim Settlements 2 keeps its virtual resources and supplies on each settlement's data store instead
    (SettlementManager's SettlementDataStores array, indexed by workshop ID: list it with game_script_vars).
    target: a reference's FormID in hex, or "workshops" for every workbench (with its location). actor_values:
    FormIDs in hex or editor IDs, e.g. ["1104A581"]."""
    if not actor_values:
        raise ToolError("give at least one actor value")
    return _call("av", target, *actor_values)


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
    folder in the game's sScreenShotBaseName (Fallout4Prefs.ini, [Display]), and bAllowScreenshot=1. Several
    folders (one per MO2 profile) can be given, separated by ";"."""
    folders = [f for f in os.environ.get("FO4_GAME_SCREENSHOTS", "").split(";") if f and os.path.isdir(f)]
    if not folders:
        raise ToolError("set FO4_GAME_SCREENSHOTS to the folder in the game's sScreenShotBaseName")
    folder = "; ".join(folders)
    pattern = [os.path.join(f, ext) for f in folders for ext in ("*.png", "*.jpg", "*.bmp")]
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
