# Build configuration

## `IS_VOPI_ENGINE`

```cmake
option(IS_VOPI_ENGINE "Enable VOPI Engine features" OFF)
```

The single switch for everything this fork adds. It defaults to `OFF`, so a
plain `cmake .. && cmake --build .` produces upstream Luanti.

When enabled it defines `IS_VOPI_ENGINE=1` for the compiler, renames the
project to `Luanti (VOPI Engine)`, appends the engine revision to the version
string, and pulls in the extra library and source directories the features
need — currently `lib/aes` for encrypted content packs, and the platform
source directory named by `VOPI_PLATFORM_SRC_DIR`.

Most feature code is guarded in place with `#if IS_VOPI_ENGINE`, so the two
configurations build from the same sources. Turning the option off is the
first diagnostic step when a bug might be ours rather than upstream's.

## Version string

```cmake
set(ENGINE_REVISION "ve1" CACHE STRING "VOPI Engine revision")
set(GAME_VERSION    ""    CACHE STRING "Game/application version string")
```

`ENGINE_REVISION` marks a revision of the fork's own modifications and is
appended to the upstream version when `IS_VOPI_ENGINE` is on:

```
5.16.1        upstream
5.16.1-ve1    this fork
```

Bump it when fork behaviour changes in a way a bug report needs to distinguish;
the upstream numbers stay owned by `util/bump_version.sh`.

`GAME_VERSION` is the marketing version of the application embedding the
engine. It is passed in by the platform build — Gradle on Android, Xcode on
iOS — and defines `GAME_VERSION` for the compiler when set. The engine only
reports it; nothing branches on it.

## Other CMake variables

| Variable | Purpose |
|---|---|
| `VOPI_PLATFORM_SRC_DIR` | Directory of platform C++ compiled into the engine — see below |
| `GETTEXT_PO_PATH` | Honoured for VOPI builds so the embedding project can supply its own translations instead of the ones in `po/` |

## The platform layer

`VOPI_PLATFORM_SRC_DIR` points at a directory of C++ that lives **outside this
repository** and is compiled alongside the engine. It is where an embedding
project puts what is specific to it: the content-pack key provider, the
per-platform default settings, the mobile porting layer.

The engine defines the interfaces and works without any of it:

| With a platform layer | Without one |
|---|---|
| encrypted content packs mount | only unencrypted packs mount |
| the embedding project's default game and fonts apply | no default game, stock fonts |
| the mobile targets build | desktop targets build; the mobile ones do not |

Leaving it unset is a supported configuration, not a broken one — it is what a
plain checkout of this repository builds, and what CI builds. CMake reports
which case you are in:

```
-- VOPI platform layer: /path/to/layer
-- VOPI platform layer: none (encrypted packs will not mount)
```

The engine picks up two files by name when the directory provides them:
`content_keys.cpp` (always) and `default_desktop_settings.cpp` (desktop
targets only). Everything else the layer contains is wired up by the embedding
project's own build — Gradle on Android, Xcode on iOS.
