# Mobile platform layer

Upstream Luanti runs on Android; this fork adds iOS, unifies the two behind one
mobile code path, and gives the surrounding application a way to talk to the
engine at runtime.

## Where platform code lives

| Layer | Location |
|---|---|
| Shared porting layer | `src/porting.{cpp,h}` |
| Android specifics | `src/porting_android.{cpp,h}` |
| State channel | `src/platform_state.{cpp,h}`, `src/script/lua_api/l_platform_state.{cpp,h}` |
| Application platform layer | **outside this repository**, compiled in via `VOPI_PLATFORM_SRC_DIR` |

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

## Mobile lifecycle

Mobile applications are suspended, resumed and re-entered in ways a desktop
process is not, and several fixes here exist because of it:

- **`main()` re-entry on Android.** The process can call `main()` again without
  having been torn down. Process globals and the `Settings` hierarchy are reset
  on re-entry, so a second run does not inherit the first one's state.
- **Background.** Load-screen rendering is skipped while the app is
  backgrounded, main-menu audio is paused, and texture creation is deferred
  while no rendering context exists.
- **Focus regain.** The game pauses when focus returns, which resets touch
  input state that system UI (notification shade, control centre) leaves
  inconsistent.
- **Formspec after a background cycle on iOS** would come back invisible; the
  fix is in the same area.

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
