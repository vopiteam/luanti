# Configurable day cycle

This subsystem requires `IS_VOPI_ENGINE=ON`. The server owns one fractional
civil clock and a day counter. Within that build, a game can opt in to a
two-speed cycle and configurable visual phases. Without runtime opt-in,
ordinary `time_speed`, the standard light curve and standard celestial orbit
remain in use, with the precise clock and its pause fixes.

With `IS_VOPI_ENGINE=OFF`, the day-cycle module and tests are excluded from the
build. The new Lua APIs, network capability, saved metadata,
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
changing the time or clearing the pause.

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

## Sky exposure and fog

For regular sky with an enabled profile, cave exposure follows elapsed time
with a 0.4-second exponential time constant. Cave-side brightness is retained
while exiting, so the first sunlight-visible frame does not jump directly to
full outdoor brightness. Current clock weights and supplied biome palettes
remain current; this filter smooths exposure, not outdoor clock edits.

`set_sky` fog colors use the same sky/cave brightness. Their alpha composites
the modulated RGB over the automatic background: zero selects automatic fog,
255 selects the complete override. Intermediate alpha can fade a game-supplied
fog into or out of automatic fog. This changes the legacy interpretation of
nonzero-alpha fog, so games must not pre-darken RGB a second time to compensate
for the old absolute override.

Effective fog distance and start follow their resolved targets with a
0.2-second exponential time constant. Range is always capped by the current
client limit. A negative distance remains client-controlled; negative fog
start resolves the client's setting when the sky packet arrives. A zero range
has a finite shader denominator. These are exponential time constants, not
fixed-duration transitions. Player getters expose requested server settings,
not the client's current interpolated result.

Disabled profiles and non-regular skies retain the legacy fog color/range
behavior; OFF builds retain the previous implementation. These changes add
no fields to sky packets. Games still own transitions between sky palettes.

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
  It returns nil before an environment exists. An optional fraction in
  `[0,1]`, where 1 wraps to midnight, forecasts the visual/timing state
  without mutating the world; date and pause still describe the current world. CSM reads the synchronized, locally predicted
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
performed on resume: a server iteration that measures more than five seconds of
wall time, for example after the process was suspended, is skipped rather than
simulated as one step. ABM/LBM timers and `get_gametime` retain their own semantics.

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
instead of silently discarding the saved clock. Any other `day_cycle_*` field
is ignored and dropped by the next save. Loading validates saved values before
publishing them. Invalid metadata produces a handled startup error; an
incompletely initialized world does not run ordinary shutdown callbacks or
save over the original environment metadata.

The saved profile stays active when a world is later opened by a game that
does not configure one. `core.set_day_cycle({enabled=false})` returns such a
world to the standard cycle.

### World load information

`core.get_world_load_info()` is a server-only, read-only API. It returns nil
before successful environment metadata loading, including during mod loading
and `register_on_mods_loaded`, without a deprecation warning or error even with
`deprecated_lua_api_handling = error`. Use it from a later server callback, such
as the first globalstep. Each call returns a fresh table of facts from that load:

| Field | Meaning |
|---|---|
| `has_metadata` | An existing `env_meta.txt` was read successfully; false when default metadata was used |
| `has_day_cycle_state` | The file contained a valid native clock snapshot |
| `day_cycle_enabled` | Enabled flag in the saved snapshot; false without a native snapshot |

These facts do not change when a queued startup profile, a time edit or a
runtime profile replacement changes the active clock. Editing the returned
Lua table does not modify native state. The API is not synchronized to clients
or available in CSM. It exposes no file paths or mutable metadata writer.

A game can combine these facts with its own version in mod storage to perform
a world upgrade. The engine does not classify a world as needing an upgrade,
choose its new hour, or write a migration marker. Existing hours and dates are
retained during profile configuration; missing metadata uses `world_start_time`.
An explicit `core.set_world_time({timeofday=...})` from Lua keeps the date.

`migration` and `legacy_timeofday` are no longer accepted in `set_day_cycle`,
either at startup or at runtime. Passing either field raises an error instead
of silently ignoring an old game's request. Deploy the updated game and engine
together. Backup directories left by previous versions are not deleted; no
new backup or special startup metadata save is performed.

Clock state and mod storage use their ordinary independent save mechanisms.
A game-owned version marker and a time edit do not form a shared transaction:
forced termination can persist either component alone. A game must choose
appropriate retry/skip semantics. Format validation and the existing clock's
safe metadata replacement remain engine responsibilities. Unrelated shutdown
I/O failures retain ordinary engine handling.

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
weights/orbit, validation, snapshot parsing and clock formatting.
`--test-module TestSkyAppearance` additionally covers fog compositing,
initialization, cave transitions and reversals at 30/60/120 FPS, current-clock
palette changes, effective fog bounds and the zero-distance denominator.
These headless tests do not replace device validation of shaders and formspecs.
