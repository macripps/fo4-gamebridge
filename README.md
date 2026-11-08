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
| `server.py` | MCP server that talks to the plugin. Its tools: `game_status`, `game_console`, `game_script_vars`, `game_player`, `game_ref`, `game_refs_near`, `wsfw_runners`, `papyrus_stacks`, `read_log`, `latest_screenshots`. |

The protocol is one line each way: `id<TAB>op[<TAB>arg...]` in, `{"id":"...","ok":true,"result":"..."}` out. The
operations are `ping`, `console <command>`, `vars <formid hex> <script>`, `player`, `ref <formid hex>` and
`near <radius> [<base form type> [<x> <y> <z>]]`.

- `game_player`: the player's cell, position and rotation (degrees).
- `game_ref`: one reference's base object and type, cell, position, rotation, disabled/deleted/3D state, and every
  linked ref (keyword → target), including links a script added at runtime.
- `game_refs_near`: references within a radius of the player or a point, nearest first (up to 300), optionally only
  those whose base is a given form type (`LIGH`, `STAT`, `CONT`...). Finds objects scripts spawned (`FF...` FormIDs).

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
