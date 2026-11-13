# GameBridge for Fallout 4

An F4SE plugin and an [MCP](https://modelcontextprotocol.io) server that let an AI assistant, or any local tool, work
with a running game: run console commands, read the live variables of any Papyrus script, and read the Papyrus logs.
It was built to debug Workshop Framework and Sim Settlements 2 scripts while the game runs.

> **Use it on a test profile, not your playthrough.** The plugin runs any console command it's sent. It listens on
> `127.0.0.1` only, but any program on your PC can connect to it.

## What's in it

| Part | What it does |
|---|---|
| `GameBridge.dll` (`src/`) | F4SE plugin. Listens on `127.0.0.1:47830` on its own thread, so `ping` answers even when Papyrus is jammed. Anything that touches game state runs on the main thread through F4SE's task queue. |
| `server.py` | MCP server that talks to the plugin. Its tools: `game_status`, `game_console`, `game_script_vars`, `game_player`, `game_ref`, `game_actor`, `game_refs_near`, `game_place`, `game_screenshot`, `game_perf`, `game_quest`, `game_quest_events`, `game_global`, `game_camera`, `game_camera_look`, `game_camera_fly`, `game_camera_orbit`, `game_camera_path`, `game_crosshair`, `game_move`, `game_wait_for_load`, `wsfw_runners`, `papyrus_stacks`, `read_log`, `latest_screenshots`. |

The protocol is one line each way: `id<TAB>op[<TAB>arg...]` in, `{"id":"...","ok":true,"result":"..."}` out. The
operations are `ping`, `console <command>`, `vars <formid hex> <script>`, `player`, `ref <formid hex>`, `actor <formid hex>`,
`near <radius> [<base form type> [<x> <y> <z>]]`, `perf [<seconds>]`, `place <formid hex> [<distance>]`,
`screenshot`, `quest <formid hex or editor ID>`, `questlog [<count>]`, `global <formid hex or editor ID>`,
`camera <subcommand> [<arg>...]` (`status`, `free`, `release`, `stop`, `look`, `fly`, `orbit`, `path`; a point is
`x,y,z` or a reference's FormID), `crosshair [<range>]`, `move <formid> <to|-> <offset|-> <rotation|->` and `loads`
(answered before game data is ready too).

- `game_player`: the player's cell, position and rotation (degrees).
- `game_ref`: one reference's base object and type, cell, position, rotation, disabled/deleted/3D state, and every
  linked ref (keyword → target), including links a script added at runtime. For an actor whose 3D isn't loaded, the
  position can be its editor position rather than where it is.
- `game_actor`: one actor's life state, whether it's talking to the player, the AI package it's running and that
  package's target, the furniture it's using or heading to (a sitting NPC is using a chair or sit marker), and its
  equipped items. It reads data members only: calling some engine functions by vtable slot or address crashed
  runtime 1.11.240.
- `game_place`: places a new reference of a base object in front of the player, upright and turned the way the
  player faces, and returns its FormID. Handy for looking at a new mesh in game.
- `game_screenshot`: takes a screenshot through the game's own screenshot function and returns it. Set
  `FO4_GAME_SCREENSHOTS` to the folder in the game's `sScreenShotBaseName` (`[Display]` in `Fallout4Prefs.ini` or
  your custom ini), with `bAllowScreenshot=1`.
- `game_refs_near`: references within a radius of the player or a point (in the interior cell, or across all loaded
  exterior cells), nearest first (up to 300), optionally only
  those whose base is a given form type (`LIGH`, `STAT`, `CONT`...). Finds objects scripts spawned (`FF...` FormIDs).
- `game_perf`: frame rate and frame times (mean, median, 99th percentile, worst) over 0.5 to 10 seconds, read from the
  engine's own frame timer. Compare runs from the same spot and facing, e.g. a room before and after a change. With
  vsync on, a cost shows first in the 99th percentile and worst frames.
- `game_quest`: one quest's run flags, current stage (the highest set), objectives with their live state and text,
  aliases with their flags and the reference each holds, and the stage events seen for it this session. Which earlier
  stages are done isn't readable.
- `game_quest_events`: the latest quest stage and start/stop events from the engine (up to 500 since the game
  started), with times. They come from the engine, not Papyrus, but a stage with no log entry sends none: it's set
  silently, and Papyrus `OnStageSet` doesn't fire for it either. Give every stage a script reacts to a log entry, even
  an empty one.
- `game_global`: a global variable's value.
- Camera tools, for looking round the world or filming: `game_camera` switches the free camera (as `tfc`) on and off
  and reports where it is; `game_camera_look` turns it to face a point or reference; `game_camera_fly` glides it to a
  point, optionally keeping something in view; `game_camera_orbit` circles a point or reference while facing it
  (radius, height, turns, aim height), easing onto the circle from wherever the camera is; `game_camera_path` flies a
  smooth curve through several points at an even speed. Moves are eased, run in game, and return at once, so take a
  `game_screenshot` along the way. The camera's position and rotation are written every frame (data members only, no
  engine calls by address); the player doesn't move, so outdoors only the cells already loaded are drawn. `tm` in
  the console hides the HUD.
- `game_crosshair`: what the view ray hits first, cast from the camera along the player's aim (or the free camera's
  view) through the physics world, as the game's own pick does. Reports the hit point, its distance, the 3D node and
  the reference that owns it, skipping the player's own collision. When something can't be activated, it shows
  whose collision is in front of it: building collision is simpler than the visible mesh. It calls `bhkPickData` and
  `TESObjectCELL::Pick` by address (non-virtual, tested on 1.11.240).
- `game_move`: moves and/or turns a reference: an absolute position, an offset in the reference's own frame
  (forward, right, up) and a rotation in degrees. It runs `<ref>.setpos` / `<ref>.setangle`, so it needs no console
  selection (a `prid` only takes effect a frame later), and reads the result back a few frames later. Like `setpos`,
  the change is kept in the save.
- `game_wait_for_load`: returns once a save is loaded, at once if one already is, or with `next=True` after the
  next load. It waits for the game to start if it isn't running. It reads F4SE's load messages (the save's name from
  `kPreLoadGame`, then `kPostLoadGame` / `kNewGame`).

Forms print as `FormID EditorID [Plugin.esm] "Display name"`: the editor ID only where the game keeps it at runtime
(keywords, quests), the defining plugin (none for forms created in game), and the display name if there is one.

## Requirements

- Fallout 4 1.11.240 (next-gen update). CommonLibF4 also supports 1.10.163 and 1.10.984, but only 1.11.240 is tested.
- [F4SE](https://f4se.silverlock.org/) and [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327).
- Python 3.10+ with `pip install mcp` (2.x) for the MCP server; `pip install pillow` too if you want
  `latest_screenshots` to shrink images.

## Building

You need the Visual Studio 2022 C++ build tools (the "Desktop development with C++" workload and a Windows SDK) and
[xmake](https://xmake.io) 3.x.

```
git clone --recurse-submodules https://github.com/macripps/fo4-gamebridge
cd fo4-gamebridge
xmake f -y -p windows -a x64 -m releasedbg
xmake
```

The plugin is built to `build/windows/x64/releasedbg/GameBridge.dll`. Install it as `Data/F4SE/Plugins/GameBridge.dll`,
for example as an MO2 mod with that layout. Copy `GameBridge.pdb` beside it too, so crash logs can name the line.

## Using the MCP server

Add it to your MCP client. For Claude Code, put this in `.mcp.json` in your project:

```json
{
  "mcpServers": {
    "fallout4": {
      "command": "C:/path/to/python.exe",
      "args": ["C:/path/to/fo4-gamebridge/server.py"]
    }
  }
}
```

`read_log` and `latest_screenshots` work with the game closed. The other tools need the game running with the plugin
loaded; `game_status` confirms the plugin is there.

`latest_screenshots` returns the newest game screenshots (1600 px wide with Pillow installed). Set the
`FO4_SCREENSHOTS` environment variable to the folder they're saved in, e.g. through an `"env"` entry in `.mcp.json`
(for Steam: `.../Steam/userdata/<your id>/760/remote/377160/screenshots`).

`wsfw_runners` reads Workshop Framework's 50 thread runners and flags any that are stuck. It needs WorkshopFramework.esm's
console prefix in the `WSFW_PREFIX` environment variable (default `10`).

## Useful console commands

- `ReloadScript "Some:Script:Name"` swaps in a recompiled `.pex` while the game runs. The quotes are required, and the
  game pauses briefly while it reloads. Existing instances pick up the new function bodies, so leave the variables
  unchanged.
- `cqf <quest EditorID> <function> <args>` calls a function on a quest's script. It needs the EditorID, not a FormID.
- `DumpPapyrusStacks` writes every running Papyrus stack to `Papyrus.0.log`, and the `papyrus_stacks` tool returns them.

## Known issues

- `game_console` returns only the command's echo, not what it printed. The command does run. Check its effect through
  `game_script_vars`, or have the script write to a user log (`Debug.OpenUserLog`, `Debug.TraceUser`) and read it with
  `read_log`.

## Licence

GPL-3.0 (see `LICENSE`), because it links [CommonLibF4](https://github.com/Dear-Modding-FO4/commonlibf4), which is
GPL-3.0. It carries the same modding and linking exceptions as the CommonLibF4 template (see `EXCEPTIONS`).
