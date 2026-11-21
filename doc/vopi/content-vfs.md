# ContentVFS

A read-only overlay filesystem that lets the engine read game content out of
container files instead of a directory tree. Content ships as `.kpk` packs,
optionally encrypted, and is read straight into memory — it is never unpacked
to disk. The one exception is a world pack, a world template copied out into a
new world (see [World packs](#world-packs)).

Source: `src/content_vfs.{cpp,h}`, `src/client/mod_vfs.{cpp,h}`,
`src/script/lua_api/l_content_packs.{cpp,h}`. Unit tests:
`src/unittest/test_content_vfs.cpp`.

## How it hooks in

The central `fs::` functions — `ReadFile`, `PathExists`, `IsFile`, `IsDir`,
`GetDirListing`, `AbsolutePath` — consult the overlay. Every piece of path
arithmetic already in the engine therefore works unchanged on packed content;
no call site had to learn about packs.

A pack declares a mount prefix such as `share:/games/foo`, resolved against
`porting::path_share` or `path_user` when it is mounted.

## Resolution rule

> **The real filesystem wins.**

A loose file at the same path shadows the pack entry. Pack entries only fill
gaps the real tree misses.

This ordering is the load-bearing decision. It makes development overlays work
— drop plain files over a mounted pack and they take effect — and it makes
every fallthrough trivially safe, because the packed path can only ever be
reached when nothing real is there.

## Sources and priority

Packs are mounted from two places:

| Source | Directory | May contain |
|---|---|---|
| `Bundled` | `<path_share>/packs`, shipped with the application | anything, including bytecode |
| `Installed` | set by the platform layer, downloaded content | `content` packs, mounted; `world` packs, listed and never mounted |

**Code ships only with the application.** Bytecode lives in `base` packs, and
`base` packs are never mounted from the installed directory. Downloaded content
cannot introduce executable code.

When two mounted packs share an id, the higher version wins and the other is
ignored. When different packs provide the same path, lookup order is installed
before bundled, then higher version, then id — deterministic, but such a
collision is a packaging bug and is logged as one.

## Encryption and keys

The reader supports format v1, plain and AES-256-CTR.

**No key material lives in this repository.** An encrypted pack names a
`key_version`; the reader asks an application-installed `ContentKeyProvider`
for that key:

```c
typedef bool (*ContentKeyProvider)(u32 key_version, unsigned char key_out[32]);
```

Without a provider, or when the provider does not know the version, the mount
is refused. The provider is installed at startup by the platform layer.

Two `extern "C"` startup hooks carry this, with do-nothing definitions in
`content_vfs.cpp` so the public engine builds and runs with neither keys nor
platform paths:

| Hook | Provides |
|---|---|
| `vopi_install_content_key_provider` | the `ContentKeyProvider` |
| `vopi_configure_content_vfs` | installed-packs directory and application version |

`main()` calls them on every platform, with no per-OS wiring. With GCC and
Clang the defaults are weak, and the platform layer's definitions replace
them. MSVC has no weak symbols: a build without a
platform layer gets the defaults as plain definitions, and a platform layer
built with MSVC must define every hook.

The encryption is a barrier, not a security guarantee: anything a client
renders can be recovered by someone determined enough. Publishing the mechanism
here costs nothing under Kerckhoffs's principle — only the key is secret, and
the key is not in this repository.

## Precompiled chunks

Lua in a trusted pack may be shipped as LuaJIT bytecode rather than source. A
pack records the `luajit_version` it was compiled for; a mismatch is refused
rather than loaded. Bytecode is accepted only from `base` packs — finding it in
a `content` or `world` pack is a packer bug, and is reported as one.

## Media

Pack-served local media skips the media cache. The cache exists to avoid
re-downloading from a server; a local pack read is already cheap, and caching it
would duplicate the content on disk in exactly the form the container exists to
avoid.

A client that does not trust its server (`platform.md`, Platform network)
loads each medium the server announces from the mounted packs by the SHA-1
of its content, which every pack index records: `PackMediaIndex`
(`src/client/clientmedia.h`) indexes the mounts visible when the
announcement arrives, in lookup-priority order.

## Thread safety

The mount table is an immutable snapshot behind a `shared_ptr`. Readers take
their own reference and can never observe a partially updated table. Writers —
startup mount and `syncInstalledPacks` — replace the snapshot atomically, and
only run on the main thread while no world is active.

Entry reads are stateless (`pread`), so no locks are needed on POSIX. A mutex
guards reads on Windows, where `pread` is unavailable.

The world pack list (below) has a lock of its own and may be read from any
thread.

## World packs

A pack of type `world` is a world template: the files of a world
(`world.mt`, `map_meta.txt`, the map, the player, auth and mod storage
databases) to copy into a new world. It is never mounted, from either
directory; a main menu lists the templates and makes worlds from them
through the Lua API below.

**The list** (`ContentVFS::listWorldPacks`) holds the world packs of the
bundled and the installed directory: for each id the highest version, an
installed copy on equal versions, leaving out a pack whose `minAppVersion` is
newer than the application and a pack with code (a `.lua` entry, or anything
in `worldmods/` or `game/`, the folders a world loads code from, all matched
in any case: code ships only with the application), sorted by id. A pack file is opened once and again
only when it changes; the file of a mounted pack is skipped. For world packs
only, the reader also reads the preview block's `name` and `description`, the
texts a list shows.

**A world from a pack** (`createWorldFromPack`):

1. The pack's files are written, decrypted, into a staging folder, each
   checked against the SHA-1 of the pack's index: neither the encryption nor
   a zstd frame catches damage.
2. Its `world.mt` gets the caller's keys, then `world_name`, `gameid`,
   `world_pack` (the pack's id) and `world_pack_version`, which the caller
   cannot replace.
3. The folder moves into the worlds directory under the name
   `core.create_world` would give the world: `sanitizeDirName(name, "world_")`,
   then the first free `_<n>`.

An empty folder name, a `world.mt` value that `Settings` refuses or whose
trim is `{` or `"""` (it would read back as a group or the start of a
multiline value, swallowing the rest of the file), and a pack with code are
refused. One creation runs at a time. What a creation cut short left in the
staging folder is removed by the next. The world on disk is plain, since the engine
writes into it; only the template stays encrypted.

## Lua API

Available in the menu, server, emerge and async environments:

```lua
core.is_content_pack_mounted(id)     --> boolean
core.get_content_pack_info(id)       --> table | nil
core.get_mounted_content_packs()     --> table
```

They are thin wrappers over the mount table. See [lua-api.md](lua-api.md).

Available in the main menu and its async environment only:

```lua
core.get_world_packs()                                    --> table
core.create_world_from_pack(id, name, gameid[, settings]) --> path | nil, error
```

- `core.get_world_packs()` returns the [world pack](#world-packs) list as an
  array of `{id, version, source, name, description}`; `source` is
  `"bundled"` or `"installed"`, and `name` and `description` are absent when
  the pack's preview has none. A menu reads it again on the
  `ContentPacksChange` event, raised on the first frame of each menu
  session and when the platform asked for a resync of the installed
  directory (`ContentVFS::requestResync`).
- `core.create_world_from_pack(id, name, gameid[, settings])` makes a world
  from the listed pack `id`, under `<path_user>/worlds`, and returns its path.
  `settings` is a table of `world.mt` keys, string keys with string, number
  or boolean values. On failure it returns `nil` and one of:
  - `"unknown_pack"`: no listed pack has the id (a pack with code is not
    listed);
  - `"no_space"`: the device ran out of space;
  - `"damaged"`: an entry of the pack cannot be read or fails its hash;
  - `"error"`: anything else, a game id that is not installed, an empty
    name and invalid `settings` included; the log says what.

  A non-string `id`, `name` or `gameid` raises, as any argument error of the
  API does.

  It blocks while it writes, so a menu calls it from `core.handle_async`. A
  job there returns one value to its callback, so it returns path and error
  in one table, e.g. `{path = path, err = err}`. Its staging folder is
  `<path_user>/world_packs.staging`.

## Container format

The `.kpk` format itself is specified by the tooling that produces it, not by
this reader. The reader implements v1.
