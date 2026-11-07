-- Built from Dear-Modding-FO4/commonlibf4-template (GPL-3.0). CommonLibF4 is the lib/commonlibf4 submodule:
--   git clone --recurse-submodules https://github.com/macripps/fo4-gamebridge
includes("lib/commonlibf4")

local plugin_name = "GameBridge"
local plugin_version = "1.0.0"
local plugin_version_major, plugin_version_minor, plugin_version_patch = plugin_version:match("^(%d+)%.(%d+)%.(%d+)$")

set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.release", "mode.releasedbg")

add_defines("COMMONLIB_RUNTIMECOUNT=3")

target(plugin_name)
    add_rules("commonlibf4.plugin", {
        name = plugin_name,
        author = "AgentIcarus",
        description = "Local bridge between a running game and an MCP server"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_syslinks("ws2_32")

    add_defines(
        'PLUGIN_NAME="' .. plugin_name .. '"',
        "PLUGIN_VERSION_MAJOR=" .. plugin_version_major,
        "PLUGIN_VERSION_MINOR=" .. plugin_version_minor,
        "PLUGIN_VERSION_PATCH=" .. plugin_version_patch
    )
