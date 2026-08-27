# ContentVFS

A read-only overlay filesystem that lets the engine read game content out of
container files instead of a directory tree. Content ships as `.kpk` packs,
optionally encrypted, and is read straight into memory — it is never unpacked
to disk.

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
| `Installed` | set by the platform layer, downloaded content | `content` packs only |

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

Two `extern "C"` startup hooks carry this, with weak do-nothing definitions in
`content_vfs.cpp` so the public engine builds and runs with neither keys nor
platform paths:

| Hook | Provides |
|---|---|
| `vopi_install_content_key_provider` | the `ContentKeyProvider` |
| `vopi_configure_content_vfs` | installed-packs directory and application version |

The same mechanism on every platform, so `main()` needs no per-OS wiring.

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

## Thread safety

The mount table is an immutable snapshot behind a `shared_ptr`. Readers take
their own reference and can never observe a partially updated table. Writers —
startup mount and `syncInstalledPacks` — replace the snapshot atomically, and
only run on the main thread while no world is active.

Entry reads are stateless (`pread`), so no locks are needed on POSIX. A mutex
guards reads on Windows, where `pread` is unavailable.

## Lua API

Available in the menu, server, emerge and async environments:

```lua
core.is_content_pack_mounted(id)     --> boolean
core.get_content_pack_info(id)       --> table | nil
core.get_mounted_content_packs()     --> table
```

They are thin wrappers over the mount table. See [lua-api.md](lua-api.md).

## Container format

The `.kpk` format itself is specified by the tooling that produces it, not by
this reader. The reader implements v1.
