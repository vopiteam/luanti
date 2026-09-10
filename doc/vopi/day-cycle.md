# Configurable day cycle

This subsystem requires `IS_VOPI_ENGINE=ON`. The server owns one fractional
civil clock and a day counter. Within that build, a game can opt in to a
two-speed cycle and configurable visual phases. Without runtime opt-in,
ordinary `time_speed`, the standard light curve and standard celestial orbit
remain in use, with the precise clock and its pause fixes.

With `IS_VOPI_ENGINE=OFF`, the day-cycle module and tests are excluded from the
build. The new Lua APIs, network capability, saved metadata, backup setting,
clock formspec element and shader uniform are absent. The previous Luanti
clock implementation, light queries, protocol and rendering remain intact.
A disabled runtime profile in a VOPI build is distinct from this compile-time
choice.

## Definition

Call during mod loading, or replace the complete definition during play:

```lua
core.set_day_cycle({
    enabled = true,
    timing = {
        day_start = 6/24, night_start = 18/24,
        day_duration_seconds = 600, night_duration_seconds = 600,
    },
    visual = {
        dawn_start = 5/24, sunrise = 6/24, dawn_end = 7/24,
        dusk_start = 17/24, sunset = 18/24, dusk_end = 19/24,
        night_light = 0.175, day_light = 1,
    },
})
```

`enabled` defaults to true. All timing and visual fields are required when
enabled. This replaces a definition, rather than patching selected fields.
`core.set_day_cycle({enabled=false})` returns to the standard cycle without
changing the time or clearing the pause or the world's migration marker.

Positions are finite fractions in `[0,1)`. Timing intervals must each span at
least one game second. Durations are simulation seconds in `[1,31536000]`;
closed applications and paused simulation do not advance this clock. Visual
positions must follow the listed cyclic order, with at least one game second
between neighbours, including the final night interval. Light levels satisfy
`0 <= night_light < day_light <= 1`. Invalid definitions raise a Lua error and
leave the active definition unchanged.

The clock uses `timing` boundaries. The orbit crosses the horizon at `sunrise`
and `sunset`; its highest point is halfway between those crossings. Smoothstep
blends the light over dawn/dusk. Three palette weights interpolate night →
dawn → day and day → dawn → night; the existing `dawn_*` sky colours serve both
transitions. Moon direction stays opposite the sun. Sky palettes, clouds,
star visibility, horizon glow, tonemaps and shadow strength consume this state.
Per-player sky, star opacity, lighting and custom shadow-direction overrides
remain available. A custom shadow direction bypasses the automatic horizon
strength fade in controlled mode.

## World APIs

These mutations are server-only. `set_day_cycle` and `set_day_cycle_paused`
queue requests made during mod loading until world metadata is loaded.
Time edits require an initialized world; perform them from a server callback.

- `core.set_day_cycle_paused(boolean)` stops/resumes integration. It neither
  changes the date nor repeatedly sets the time. The pause is saved.
- `core.set_world_time({day=integer, timeofday=fraction})` atomically edits
  the date and/or time. Omitted fields retain their values. `day` is in
  `[0,4294967295]`; `timeofday` is in `[0,1)`. Editing time alone keeps the date.
- `core.advance_time(game_seconds)` explicitly moves forward, accounting for
  every crossed midnight, even while paused. Negative/non-finite values and
  day-counter overflow are rejected without changing the clock.
- `core.get_day_cycle_state([timeofday])` is available on server and CSM.
  It returns nil before an environment exists. An optional fraction forecasts
  the visual/timing state without mutating the world; date and pause still
  describe the current world. CSM reads the synchronized, locally predicted
  clock and does not have authority to change it.

State fields:

| Fields | Meaning |
|---|---|
| `enabled`, `paused` | Mode and saved clock pause |
| `day`, `timeofday` | Civil date and fractional clock |
| `revision`, `discontinuity` | Profile revision and explicit edit/pause marker |
| `is_day` | Timing bucket; may differ from the visual phase |
| `phase` | `dawn`, `day`, `dusk`, `night` |
| `daylight` | Normalized light blend, 0..1 |
| `day_night_ratio` | Effective profile light level, 0..1, before player override |
| `day_weight`, `dawn_weight`, `night_weight` | Palette weights summing to one |
| `orbit_time`, `shadow_factor` | Orbit phase and automatic shadow multiplier |
| `speed` | Game seconds per simulation second; zero while paused |
| `day_start`, `night_start` | Timing boundaries |
| `day_duration_seconds`, `night_duration_seconds` | Configured simulation durations |

In standard mode, visual phase/weights are derived from the legacy light curve;
there are no independently defined dawn/dusk intervals. Duration fields are the
stored definition defaults, not a measurement of legacy `time_speed`.

`get_timeofday` and `get_day_count` expose the same world clock. The existing
`set_timeofday` retains its legacy backwards-edit-means-next-day behaviour and
24000-step input precision; use `set_world_time` for explicit calendar edits.
`get_node_light` and `get_natural_light`, including forecasts, use the active
profile. The standalone `time_to_day_night_ratio` utility remains a legacy
curve usable without a world; use the new state API for controlled worlds.
`override_day_night_ratio` retains precedence for rendered lighting.

Changing `time_speed` while the controlled cycle is enabled has no effect and
logs a warning when its value changes. No automatic wall-clock catch-up is
performed on resume. ABM/LBM timers and `get_gametime` retain their own semantics.

## Synchronization and persistence

An optional six-byte capability suffix follows the language in `TOSERVER_INIT2`:
`u32 0x44435943` (DCYC), `u16 1`. A capable client receives the usual
`TOCLIENT_TIME_OF_DAY` header followed by that signature/version and a
length-prefixed, versioned snapshot string. The bounded snapshot carries the
complete definition, double-precision time, date, pause, revision and
explicit-edit counter. No new opcode or upstream protocol number is needed.

The server sends edits on the next server step and periodically synchronizes
at the ordinary time-send interval. Both peers integrate using the same phase
function. Small corrections are smoothed by elapsed frame time; explicit edits
snap to the new state. This does not compensate for arbitrary network latency.

A controlled server rejects clients without the capability before activating
the world. Standard worlds accept legacy clients; new clients accept legacy
servers and their ordinary time packets. Enabling a controlled profile while
an incompatible client is already connected fails atomically.

`env_meta.txt` stores `day_cycle_state` (version 1, 17-digit numbers), retaining
legacy time/day fields for tools. Unknown or malformed snapshots fail to load
instead of silently discarding the saved clock. `day_cycle_configured` persists
the clock basis independently of the enabled flag. Loading validates the
saved values before publishing them. Invalid metadata produces a handled
startup error; an incompletely initialized world does not run ordinary
shutdown callbacks or save over the original environment metadata.

### Explicit migration

A game that formerly remapped the engine clock may add this to its initial
`set_day_cycle` call:

```lua
migration = {id="civil_clock_v1", source_sunrise=6/24, source_sunset=18/24}
```

The ID is 1..64 ASCII letters, digits, underscores or hyphens. Source boundaries
must describe the game's previous piecewise day/night mapping. The first load
of an unconfigured existing world maps its phase position to the target
timing boundaries, correcting the date when the mapped civil midnight differs.
The engine cannot infer old Lua mappings. A fresh world needs no migration.
Runtime profile replacements do not perform migrations. A disabled startup
profile also leaves migration pending, preserving the saved time and existing
markers. A later enabled startup performs the first conversion.

Enable `day_cycle_migration_backup = true` before startup. The option is
registered by VOPI-only C++ defaults and documented here, rather than in the
shared, non-preprocessed `builtin/settingtypes.txt`. Before opening world
databases or loading mods, the server copies an eligible local world directory
to its sibling `.day-cycle-backups/<world>-<unique suffix>`. Migration refuses
to proceed without a successful backup. The built-in directory copy does not snapshot remote database services; deployments
using such storage must arrange a consistent external backup as well.

Migration saves the original clock snapshot, ID and backup path alongside the
new state. The persistent configured marker prevents repeated conversion,
including after temporarily disabling and re-enabling the profile. Existing
player progress is not reset. Already corrupted historical day counters cannot
be reconstructed from time alone. Restore the saved directory while the server
is stopped to roll back the conversion; retain the corresponding game version.

## UI and assets

`clock[x,y;w,h;12h|24h;alignment]` is documented beside labels in
[the Lua reference](../lua_api.md). It updates existing text each minute without
rebuilding the form. Text and minute-change detection share the same
boundary calculation, including a small floating-point tolerance for exact
minute inputs. It needs a connected client environment.

The VOPI shader generator passes `IS_VOPI_ENGINE=1` into GLSL. Node and object
vertex shaders then consume `f_day_cycle_shadow`; the `#else` branches retain
the original `f_timeofday` formulas. C++ guards alone cannot select GLSL code.
Deploy these shader resources together with the engine binary. C++ still supplies the old
time uniform for older resource bundles, but their legacy phase thresholds
cannot correctly represent a controlled profile.

## Tests

With `IS_VOPI_ENGINE=ON`, `--run-unittests --test-module TestDayCycle` covers phase integration, arbitrary
step partitioning, freeze/resume, calendar jumps, legacy light samples, visual
weights/orbit, validation, snapshot parsing, migration and clock formatting.
These headless tests do not replace device validation of shaders and formspecs.
