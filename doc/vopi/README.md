# VOPI Engine

This fork adds mobile platform support and a set of engine features on top of
upstream Luanti. This directory documents what it adds and where each piece
lives; the rest of `doc/` is upstream reference material and applies unchanged.

Everything here is gated behind the `IS_VOPI_ENGINE` CMake option, which
defaults to `OFF`. With the option off, the feature code is excluded and the
previous Luanti behaviour is preserved. This includes the day-cycle clock,
Lua APIs, network extension, persistence, rendering and clock formspec element.
See [build.md](build.md).

## Contents

| Document | About |
|---|---|
| [build.md](build.md) | `IS_VOPI_ENGINE`, engine revision, and the version string |
| [platform.md](platform.md) | iOS and Android support, the platform state channel, mobile lifecycle, memory and thermal caps |
| [content-vfs.md](content-vfs.md) | Reading game content from encrypted containers instead of the filesystem |
| [ui.md](ui.md) | Formspec and HUD extensions, touch input, font scaling, baked node icons |
| [mapgen.md](mapgen.md) | Mapgen Valleys additions: rivers level with the sea, a solid floor, cliff carving, floating piece removal |
| [day-cycle.md](day-cycle.md) | Configurable civil clock, visual phases, synchronization and world load information |
| [lua-api.md](lua-api.md) | Index of every Lua API addition, and where each one is documented |

## Where API reference lives

Additions that extend an existing upstream concept are documented **next to
their upstream peers**, not here: a new formspec element belongs in
`doc/lua_api.md` beside the other formspec elements, a new menu function in
`doc/menu_lua_api.md`. This directory documents subsystems that have no
upstream counterpart, and indexes the rest.

[lua-api.md](lua-api.md) is the index: every added function, which Lua
environment it lives in, and which file documents it.
