# VOPI Engine

This fork adds mobile platform support and a set of engine features on top of
upstream Luanti. This directory documents what it adds and where each piece
lives; the rest of `doc/` is upstream reference material and applies unchanged.

Everything here is gated behind the `IS_VOPI_ENGINE` CMake option, which
defaults to `OFF`. With the option off, the build is upstream Luanti — the
feature code either compiles out or is not compiled at all. See
[build.md](build.md).

## Contents

| Document | About |
|---|---|
| [build.md](build.md) | `IS_VOPI_ENGINE`, engine revision, version string, and the patch workflow |
| [platform.md](platform.md) | iOS and Android support, the platform state channel, mobile lifecycle, memory and thermal caps |
| [content-vfs.md](content-vfs.md) | Reading game content from encrypted containers instead of the filesystem |
| [ui.md](ui.md) | Formspec and HUD extensions, touch input, font scaling, baked node icons |
| [lua-api.md](lua-api.md) | Index of every Lua API addition, and where each one is documented |

## Where API reference lives

Additions that extend an existing upstream concept are documented **next to
their upstream peers**, not here: a new formspec element belongs in
`doc/lua_api.md` beside the other formspec elements, a new menu function in
`doc/menu_lua_api.md`. This directory documents subsystems that have no
upstream counterpart, and indexes the rest.

[lua-api.md](lua-api.md) is the index: every added function, which Lua
environment it lives in, and which file documents it.

## What is not here

Some engine entry points used by the mobile products are not part of this
repository. They are applied as patches at build time from the product tree,
touching `l_mainmenu`, `l_util`, `s_player`, `server`, `game` and
`clientlauncher`. They are not fork commits, they do not exist in a plain
checkout, and they are not documented here — see [build.md](build.md) for how
the patch workflow keeps them out of the history.

If you are reading a function name in a build and cannot find it in this
repository, that is the reason.
