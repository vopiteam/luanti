# Mobile platform layer

Upstream Luanti runs on Android; this fork adds iOS, unifies the two behind one
mobile code path, and gives the surrounding application a way to talk to the
engine at runtime.

## Where platform code lives

| Layer | Location |
|---|---|
| Shared porting layer | `src/porting.{cpp,h}` |
| State channel | `src/platform_state.{cpp,h}`, `src/script/lua_api/l_platform_state.{cpp,h}` |
| Application platform layer | **outside this repository**, compiled in via `VOPI_PLATFORM_SRC_DIR` |

> **`src/porting_android.{cpp,h}` as committed here is upstream's.** The mobile
> implementation is part of the application platform layer, and the embedding
> project substitutes it at build time. A checkout of this repository alone
> therefore does not build the Android target — it builds the desktop targets
> and the engine, which is what `IS_VOPI_ENGINE=OFF` is for.

The application layer — the iOS porting layer, per-platform default settings,
memory-cap policy, content key provision — is not part of the fork. `CMake`
takes its directory as `VOPI_PLATFORM_SRC_DIR` and compiles it alongside. The
engine defines the interfaces; the application implements them.

## Platform state channel

The application around the engine runs services whose state the main menu wants
to show — the progress of a background content download, for instance. Rather
than one C hook per value, the platform publishes an opaque JSON document per
**topic**.

```lua
core.get_platform_state(topic)              --> table | nil
core.platform_action(topic, action[, arg])  --> boolean
```

Topics, their fields and their actions are a contract between the application
and the menu scripts. **The engine carries them without interpreting
anything** — it parses the JSON into a Lua table and passes actions back down.
Adding a feature that needs new state therefore needs no engine change.

Reading is safe everywhere: a malformed document is logged and yields `nil`,
never a partially populated table. JSON `null` becomes `nil`, matching
`core.parse_json`.

`platform_action` returns whether the platform *accepted* the action, not
whether the work succeeded. Results come back as state.

### Change notification

The platform calls `porting_platform_state_changed(topic)` from any thread. The
main-menu loop drains the changed set once per frame and raises one menu event
per topic:

```
PlatformStateChange:<topic>
```

along the same path as `WindowInfoChange`. The event carries no payload: it is
a signal to re-read, so a coalesced or missed event cannot leave Lua holding
stale data it believes is fresh. Changes made while a world is running are
delivered on the first menu frame afterwards.

This is a channel for state snapshots and occasional commands — documents of a
few kilobytes, an event only when something actually changed. It is not a
streaming channel.

### Provider registration

The platform installs a `Provider` once at startup with `setProvider`. Until it
does, and in builds with no platform layer at all (desktop, unit tests), the
engine reports no state and declines every action.

A registered provider rather than weak symbols: weak definitions do not link
the same way on every toolchain — MSVC has none — and a registered provider
lets tests install a fake.

## Platform services

`main()` calls one more startup hook after the content pack hooks
(`content-vfs.md`, Encryption and keys), before the user data directory and
the logs are set up:

```c
extern "C" void vopi_install_platform_services();   // platform_state.h
```

A platform layer defines it to install what it supplies: the platform
network (`platform_network::setProvider`) and its Lua functions
(`platform_lua::setRegistrar`), both below. The hook only stores pointers:
`main()` may run again in one process on Android, and every installation
replaces the previous one. The default, in `platform_state.cpp`, installs
nothing; it is weak with GCC and Clang, and on MSVC it is a plain definition
that exists only in a build without a platform layer: a platform layer
built with MSVC must define the hook itself. The hook alone is a weak
symbol; what it installs are registered providers, as above.

## Platform network

Datagram sockets the platform supplies in place of the device's own UDP
sockets, for a world that others join through the platform and for a client
that joins a world through it (`src/netcode/platform_network.h`). What
carries the datagrams and how their peers are authenticated is the
platform's; the engine runs its usual connection on the sockets it is given.

**The socket.** `IDatagramSocket` (`src/netcode/socket.h`) has the
operations the connection uses: `Bind`, `Send`, `Receive`, `setTimeoutMs`
and `GetHandle`. `UDPDatagramSocket` is the default, a `UDPSocket` behind the
interface, so a connection without a supplied socket works as upstream's.
`con::createMTP` has an overload that takes the socket. Its contract: `Bind`
once from the send thread, throwing only on the serving side; `Send` from
the send thread, dropping silently what it cannot deliver; `Receive` from
the receive thread, waiting at most the last `setTimeoutMs` and returning -1
when nothing came. The packet size stays 512 bytes; a socket adds its own
framing outside it.

**The provider.** A platform installs one `platform_network::Provider`. The
main menu chooses the mode with the `platform_network` field of `gamedata`
(`doc/menu_lua_api.md`, Gamedata):

- **Hosting** (`"host"`): `Game::createServer` asks `Provider::host` for a
  `Hosting` of the selected world and starts the server on its socket; no
  address of the device is resolved or bound. The server and this device's
  own client talk over an in-process channel whose two ends the hosting
  supplies; the platform presents them in `127.0.0.0/8`, and every remote
  peer at an address of its own outside it, which no other remote peer has
  while that one is connected. The local player plays under the singleplayer name
  (`default_player_name`, else `singleplayer`) and is the administrator
  (the `name` setting). The server is not in simple singleplayer mode, so
  it does not pause in the menus, and `core.is_singleplayer()` is false.
- **Joining** (`"join"`): `ClientLauncher` takes the `Joining` the
  platform has prepared, a socket, the server's address on it, and the name
  and password to log in with, which come from the platform and never pass
  through a script.

**Fails closed.** Without a provider, or when it refuses, a world started
for hosting does not start and a joining client does not connect: neither
ever falls back to a UDP socket. A joining client never resolves an address
and never takes the fallback address of an ordinary connection. No game on
the platform network asks to reconnect, and the launcher refuses a
`do_reconnect` start after one: a reconnection would need a new session,
which only the platform makes, and would otherwise host the world on the
device's own socket. A joining ends at once when the platform reports its
session ended (`Joining::session_ended`, polled every frame).

**A hosted server.** In a world hosted through the platform network the
server:

- knows its local player by the address of the in-process channel, and lets
  it log in under the singleplayer name with no password, as simple
  singleplayer mode does; its password is not checked;
- refuses any remote peer that presents the administrator's name (the
  `name` setting, which the launcher sets to the local player's name and
  which stays fixed while the world runs), and asks the hosting whether a
  remote peer at its address may log in under the name it presents
  (`Hosting::admitsPlayer`): a platform that authenticates its peers binds
  each one to one name; a remote peer may not set an empty password,
  whatever `disallow_empty_password` says (as the client declares it, the
  setting's own rule);
- applies no address ban to a remote peer and records none
  (`core.ban_player` records nothing and returns false): a remote peer's
  address is the platform's and says nothing of who it is, and its bans
  are the platform's;
- drops at once the remote peers whose sessions the platform ended
  (`Hosting::takeEndedPeers`, polled every server step);
- accepts only its own protocol version from a remote peer, answers no
  media request from one, names no remote media server, is never announced
  to a server list, and always records rollback.

**A client that does not trust its server.** A joining client
(`Client::setServerUntrusted`) accepts only its own protocol version; loads
every media file the server announces, dynamic media included, from the
mounted content packs by the file's SHA-1 (`PackMediaIndex`,
`content-vfs.md`) and ends the session on a file no pack holds or whose
content does not match (one that matches but fails to load is logged, as
on any server); ignores any media data the server sends; neither reads nor
writes the media cache; fetches nothing from a remote media server; saves
no map whatever `enable_local_map_saving` says; keeps the fog-of-war
canvas in memory, forgetting the tiles it evicts; and opens no URL during
the game, from a formspec, hypertext or the chat. Inline images of texture
modifiers (`[png:`) are still decoded. (A URL asked for before a game
starts, by an earlier game or a hypertext of the main menu, is dropped when
any game starts.)

## Platform Lua functions

A platform installs one registrar (`platform_lua::setRegistrar`,
`src/script/platform_lua.h`), which the engine calls where it registers its
own functions in three environments: the main menu's, the pause menu's and
the server's, with the `core` table at the given stack index. Without one,
nothing is registered. The functions are the platform's and are documented
with it.

## Main menu notifications

On Android and iOS the engine tells the application layer when the main menu
is on screen (`src/gui/guiEngine.cpp`): `porting::onMainMenuShown()` after the
first presented frame of each main menu session (at startup, and again after
leaving a world), and `porting::onMainMenuClosed()` when that session's loop
ends, before a world loads or the application quits. The pair lets the
application keep its own dialogs to the menu, off the loading screen and the
game.

## Mobile lifecycle

Mobile applications are suspended, resumed and re-entered in ways a desktop
process is not, and several fixes here exist because of it:

- **`main()` re-entry on Android.** The process can call `main()` again without
  having been torn down. Process globals and the `Settings` hierarchy are reset
  on re-entry, so a second run does not inherit the first one's state.
- **Background.** Load-screen rendering is skipped while the app is
  backgrounded, main-menu audio is paused, and texture creation is deferred
  while no rendering context exists.
- **Background on Android.** The process runs on there (SDL's block on
  pause stays off, for multiplayer), and the system kills a cached process
  that uses too much CPU. While the app is in the background:
  - the audio output device is paused (`ALC_SOFT_pause_device`, followed
    in `RenderingEngine::run`), so no mixer runs and the system does not
    see an app playing audio, which it would not freeze; the sound thread
    steps once a second instead of sixty times, and a one-shot sound
    started meanwhile (a multiplayer game goes on) is dropped as ended;
    a device that fails to restart is reopened (`ALC_SOFT_reopen_device`);
  - a singleplayer game is paused, as the suspended iOS app stands still;
    left from play, it stays paused until the pause menu comes with the
    focus, and left with a formspec or the chat open, it goes on when the
    window shows;
  - the frame rate, from a second in, and a singleplayer server's step
    rate are capped at `FpsControl::BACKGROUND_FPS` (4): the first frame
    after the return comes within a quarter of a second. A server hosting
    others keeps its rate, and a client session its connection; neither
    is held under the system's limit.
- **Audio interruptions on iOS.** An interruption of the audio session (a
  call, an alarm, Siri) stops OpenAL Soft's CoreAudio output, and nothing in
  OpenAL restarts it. The application platform layer pauses the output when
  the interruption begins and resumes it once the session is active again,
  through `SoundManagerSingleton::setOutputPaused`, the pause of the
  background on Android: a one-shot sound started meanwhile is dropped as
  ended, the sound thread steps once a second, and a device that fails to
  restart is reopened. For it `al_helpers.h` includes OpenAL Soft's
  `alext.h` on iOS, which links OpenAL Soft rather than the system's OpenAL.
- **Focus regain.** The game pauses when focus returns, which resets touch
  input state that system UI (notification shade, control centre) leaves
  inconsistent.
- **Formspec after a background cycle on iOS** would come back invisible; the
  fix is in the same area.
- **Emergency world save.** When the OS is about to suspend the process, the
  platform layer asks the running server for an immediate save; every server
  step polls `porting::consumeEmergencyWorldSaveRequest()` and saves the
  map, the players, the environment metadata and the mod storage on that
  step instead of waiting for `server_map_save_interval`. It polls before
  the pause early-return, so a paused game (the pause menu, the background
  on Android) saves too.

## Transient caps

Two mechanisms let the platform reduce load while the OS reports pressure. Both
are `std::atomic<int>` in `porting.h`, and both are **deliberately not
settings**:

| Variable | Bounds | Read by |
|---|---|---|
| `thermal_fps_cap` | frame rate | `FpsControl`, every frame |
| `memory_view_range_cap` | effective viewing range, in nodes | render loop |
| `memory_mapblock_cap` | client mapblock cache limit, in blocks | client map |

`0` means no cap.

The reason they are not settings is worth stating, because writing them as
settings is the obvious implementation and it is wrong: the configuration is
persisted whenever the player leaves a game or the main menu. A temporary
reduction would become permanent, and repeated reductions would compound
across launches until the game looked broken with no way for the player to
tell why.

## Apple workarounds

Present because the platform requires them, not by preference:

- `SIGPIPE` is ignored on Apple platforms.
- `mystof` avoids the Apple libc `strtod` fast path, which raised
  `EXC_BAD_ACCESS`.
- Vertex buffers are not re-uploaded in the pattern that stalls the CPU on
  Apple GL (iOS only).
- Irrlicht dropdowns open through Irrlicht on iOS rather than a native hook
  that never fires.
- The GL framebuffer is restored correctly after a texture readback.
- On iOS the engine builds with SDL2 or SDL3 (`_IRR_USE_SDL3_`, which the
  iOS project defines for every source file, `main.cpp` included):
  `main.cpp` includes the matching `SDL_main.h`, and the view controller
  comes from `SDL_GetWindowWMInfo` or from the window's
  `SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER`. An app built with the iOS 27 SDK
  needs SDL3, whose UIKit delegate adopts the scene life cycle that iOS
  then requires.
- On iOS `main()` returns right after `porting::initializeIOSPlatform()`
  when that returns false: the application keeps a unit-test host from
  starting the engine, since SDL3 calls `main()` from inside XCTest's wait
  for its session.

## SDL3 on mobile

Upstream's SDL3 backend is written for the desktop. Two SDL3 behaviours
differ from SDL2's where mobile depends on them, and the SDL device
(`irr/src/CIrrDeviceSDL.cpp`) handles both in fork builds only:

- **A touch the system takes away** arrives as `SDL_EVENT_FINGER_CANCELED`
  (on iOS whenever a view is presented over the touch, such as a text input
  dialog opened on the press). It is handled as a release: otherwise the
  pointer stays counted, and every later tap goes in as a second finger.
- **The application lifecycle events** (entering the background or the
  foreground) are not queued but handed to event watchers. The device
  registers one, `AppLifecycleWatch`, which sets the same state as the
  queued events under SDL2: otherwise the engine never learns it is in the
  background and goes on submitting GPU work there, which iOS refuses.
  An event sent before the watcher exists reaches no one, such as the
  application sent to the background while it starts, so on iOS the device
  starts from the application's state (anything but active counts as
  background, as the events make it), and a device created in the
  background has its driver defer texture creation from the start.
