# Mapgen additions

Four flags extend Mapgen Valleys. `sea_level_rivers`, `mountains` and
`remove_floaters` are part of the default `mgvalleys_spflags` when
`IS_VOPI_ENGINE` is on, `carve_cliffs` is opt-in, and none of them exists in
a build without the option, which generates the upstream terrain unchanged.
The code lives in `src/mapgen/mapgen_valleys.cpp`.

## Climate corrections

Valleys uses `calcValleysClimate` for river humidity, altitude drying and
altitude cooling. Its height context is the river-bank level and the
integer-truncated 2D terrain surface of the column, computed from the same
column model in every generation pass and in point queries. Stone generated in
the pass, 3D relief and mountain bodies above the 2D surface do not change
the climate; where a column sits in the landscape is described by the biome
form bounds below instead. If all three correction flags are disabled, the
final correction call and climate-map writes are skipped; `vary_river_depth`
still runs independently.

With `IS_VOPI_ENGINE` enabled, `mgvalleys_altitude_chill` has an effective
minimum of **1**. A zero setting is normalized when mapgen parameters are read,
written or used to construct a generator. The shared climate helper enforces
the same minimum. This prevents division by zero in both altitude corrections
and the separate river-depth calculation. Use the `altitude_chill` and
`altitude_dry` flags to disable their effects.

Positive integer values keep their previous behavior. Worlds configured with
zero will generate new terrain using 1 instead of the previous nonfinite
calculation; already generated nodes are not regenerated. Raw settings may
still contain zero until normalized mapgen parameters are serialized.
Builds with `IS_VOPI_ENGINE` disabled retain upstream behavior.

## `sea_level_rivers`

Upstream Valleys puts the surface of river water one node below the river
banks, and the banks follow the terrain. A river therefore descends in
steps, with a cascade at every step. With this flag no river bank rises
above `water_level + mgvalleys_river_bank_height`: the height by which it
would is subtracted from
the terrain, fading out with distance from the river, so every river is
level with the sea and the terrain beyond the fade keeps its height. A
river through high ground runs at the bottom of a valley that deepens to
sea level instead of through a stepped channel.

`mgvalleys_river_valley_width` sets the fade as a fraction of the valley
profile: 1.0 lowers the whole valley to sea level, smaller values cut a
narrower canyon and leave the valley above it as upstream generates it.
The 3D relief noise keeps its upstream amplitude either way.

Channels whose banks were lowered carry river water below the water line;
lowland channels stay part of the sea, as upstream.

## `mountains`

A second solid joined to the terrain of Valleys: a 3D density in the manner
of Mapgen v7, stone where
`(n_mountain + cap) * gate - (y - surface_y) / height > 0`.

- `mgvalleys_np_mountain` is the 3D noise. Its offset keeps most of space
  below zero, so the body comes as separate lobes: pillars, walls, arches,
  and windows where two lobes miss each other.
- The gradient is anchored at the terrain surface of the column rather than
  at a fixed level, so the body rides on plateaus and valley walls.
- `mgvalleys_np_mountain_height` is a 2D noise, the vertical distance the
  gradient spans and so the height a body can reach. Small values give
  rocks, large ones mountains, and a value of 0 or less switches mountains
  off, which makes the noise a regional mask as well.
- The gate is the rise of the valley itself: 0 in the river channel and 1
  beyond `mgvalleys_mountain_river_width` of the valley profile, so
  mountains grow where the ground climbs out of the valley and never dam a
  river.
- The cap. Where the body touches the ground it has a foot, the noise at
  the surface. In the band up to `mgvalleys_mountain_cap_height` above the
  ground the threshold drops by `mgvalleys_mountain_cap` times the strongest
  foot within `mgvalleys_mountain_cap_reach`, most at half that height and
  tapering with the distance from the foot. A body widens over its foot into
  overhangs and mushroom shapes, two feet within reach of each other join
  into an arch, and every cap hangs from a foot. The terrain of the columns
  within that reach around the mapchunk is computed along with it, so a foot
  beyond the mapchunk casts its cap into it like any other.
- The body is evaluated down to the level the 3D relief cannot cut below,
  so it fills the relief cut under itself and stands on the ground. Lobes
  the noise pinches off above the ground are left to `remove_floaters`.

The 3D noise is computed only for mapchunks that can hold a body, and the
spawn search knows the body.

## `carve_cliffs`

Carves the walls of high ground into alcoves, undercuts, arches and windows,
so the terrain keeps its silhouette and gains negative space. A node belongs
to a wall when it stands higher than the terrain surface of some column
within `mgvalleys_carve_reach`: flat tops and gentle slopes have no such
nodes and stay untouched, steep walls carry the whole carving, so steepness
gates it without any derivative. Inside a wall the 3D noise
`mgvalleys_np_carve` decides what goes, faded in with the height of the
ground above the river banks up to `mgvalleys_carve_zero_height`, and
biased by `mgvalleys_carve_undercut` towards the foot of the wall, which
turns alcoves into overhangs. Nothing is carved below the water line.
Pieces cut loose are taken away by `remove_floaters`.

## `remove_floaters`

The 3D relief noise of Valleys leaves pieces of stone hanging in the air
above steep ground, the mountain body pinches lobes off above the ground,
the cliff carving adds more, and tunnels near the surface cut further
pieces loose. For every column the terrain pass records
a floor: the level under which the base terrain is solid whatever the 3D
noise does. After the caves are carved, every column whose topmost run of
solid nodes ends above that floor seeds a flood fill over connected solid
nodes. A piece that reaches neither the floor nor the edge of the mapchunk
is removed, whatever its size; everything standing on the ground, overhangs
included, is kept. The pass runs before ores and decorations, so nothing is
placed in or on a removed piece.

## `mgvalleys_floor_y`

A setting rather than a flag. After the caves are carved, every void and
liquid at or below this Y becomes stone, so tunnels, randomwalk caves and
caverns end above it and nothing hollow lies under it; solid nodes are left
as they are. The fill covers the whole generation area, the border shared
with neighbouring mapchunks included: randomwalk caves reach one mapblock
past their mapchunk, and a neighbour generated later would otherwise reopen
a floor already laid. Ores and decorations run after it and may still place
into that stone. It must lie below the terrain; the default of -31000 is
below the map and does nothing.

## Biome `node_seabed`

A field of the biome definition rather than a flag, read by every mapgen
that uses the biome API. Upstream lays a biome's `node_top` and
`node_filler` on every solid surface inside the biome's Y range, whether
water or air stands above it, so a biome whose range spans both dry ground
and sea floor, a shore biome reaching from the shallows up onto the beach,
gives both the same node. With `node_seabed` and `depth_seabed` set, a
surface under sea water takes that layer instead, and surfaces under air
keep the top and filler layers. The seabed layer is `depth_seabed` nodes
deep and is followed by the biome's `node_stone`; it mirrors
`node_riverbed`, which upstream already applies under river water. A biome
without the field behaves as before. Caves are carved after the biome
layers, so cave floors are untouched by either field.

## Schematic decorations draw from the decoration's generator

Upstream rolls the placement probability of every schematic node, and of
every Y slice, with the process-wide random generator, so the trees of a
decoration differ from one run to the next even for the same seed. With
`IS_VOPI_ENGINE` the schematic blit of a decoration draws those rolls
from the decoration's own generator, the one that already chooses its
positions and rotation, and a seed reproduces the world node for node.
Schematics placed from Lua with `core.place_schematic` and
`core.place_schematic_on_vmanip` keep the upstream behaviour.

## Biome terrain constraints

With `IS_VOPI_ENGINE`, `core.register_biome` accepts four optional, inclusive
bounds. They select biomes for the existing terrain; they do not change terrain
noise, density, rivers or the generation order.

| Field | Unit | Default | Accepted explicit values |
|---|---|---|---|
| `slope_min` | degrees | 0 | finite number from 0 to 90 |
| `slope_max` | degrees | 90 | finite number from 0 to 90 |
| `relief_min` | nodes | 0 | finite nonnegative number representable as a float |
| `relief_max` | nodes | unlimited | finite nonnegative number representable as a float |

An inverted range, nonnumeric value, NaN or infinity is a registration error.
All bounds must pass, together with the existing position and Y bounds, before
weighted heat/humidity distance selects the winner. A terrain-rejected biome
cannot enter through `vertical_blend`. Keep an unconstrained fallback biome
for any climate/height that the constrained biomes do not cover; if no candidate
passes, the existing default biome is returned.

With `IS_VOPI_ENGINE`, a nonfinite weighted climate distance is recomputed in
double precision from the original float inputs. This keeps finite climate
centers and small positive finite weights selectable when float subtraction,
squaring or division overflows. Ordinary finite float distances retain their
rounding, registration-order ties and vertical blending rules. A distance
equal to `FLT_MAX` is also a valid candidate, rather than the no-candidate
sentinel. Builds without `IS_VOPI_ENGINE` retain the upstream selection path.

Only Valleys supplies terrain metrics. On other mapgens, a biome with an
operative terrain restriction is ineligible. Bounds equal to the unrestricted
defaults (`slope_min = 0`, `slope_max = 90`, `relief_min = 0`) do not impose a
restriction. Definitions without restrictions preserve the original selection
and do not sample terrain. A definition is copied with its bounds into emerge
threads.

For example, the following fields may be added to a complete biome definition:

```lua
    y_min = 2,
    y_max = 80,
    slope_max = 12,
    relief_max = 20,
```

These are illustrative thresholds, not universal values for a biome type.
Y remains the position being classified, while slope and relief describe the
natural surface above that X/Z, including for an underground biome query.

## Biome climate and form bounds

With `IS_VOPI_ENGINE`, `core.register_biome` also accepts inclusive bounds on
the climate the selector receives and on the terrain form of the column. Like
the terrain constraints, they select among the existing terrain and change
neither noise nor generation order. All are optional; an omitted bound is
unrestricted.

| Field | Meaning | Accepted explicit values |
|---|---|---|
| `heat_min`, `heat_max` | heat as the selector sees it (effective in Valleys) | finite float |
| `humidity_min`, `humidity_max` | humidity as the selector sees it | finite float |
| `base_min`, `base_max` | region level: `terrain_height + valley_depth²` before the river-bank clamp, in nodes | finite float |
| `valley_depth_min`, `valley_depth_max` | valley depth amplitude `valley_depth²`, in nodes | finite float ≥ 0 |
| `valley_pos_min`, `valley_pos_max` | position in the valley profile, 0 at the river edge, 1 on the ridge | 0 to 1 |
| `mountain_min`, `mountain_max` | mountain mask `max(mountain_height, 0) · gate`, 0 where no body can rise | finite float ≥ 0 |
| `priority` | among the biomes passing every bound, only the highest priority competes by distance | whole number in the s16 range, default 0 |

An inverted range, nonnumeric value, NaN, infinity, a value outside the float
range or a nonzero value rounding to float zero is a registration error, and
so is a priority that is not a whole number in range.

Selection order: Y and position bounds, climate bounds, form bounds, terrain
constraints, then priority, then weighted heat/humidity distance among the
survivors, with the usual registration-order ties and vertical blending.
Several biomes may share one climate point when their bounds keep them
apart. Priority is for a region that one box cannot carve out of the others:
a mountain body is `mountain_min = 10` between `y_min = 20` and `y_max = 59`,
and without priority every other biome would need the three-part complement
of that box in its own bounds. With `priority = 1` on the mountain biome the
others keep their bounds and lose only where the mountain is eligible. A
biome above its `y_max`, inside its `vertical_blend`, dithers into the biome
in range only when its priority is at least as high; disjoint biomes of
equal priority blend exactly as before. Climate bounds
work on every mapgen. Form bounds need a mapgen with a column model, which is
Valleys; elsewhere a biome with an operative form bound is ineligible, so keep
an unbounded fallback for every climate a bounded set does not cover. The
form is the modeled 2D column, independent of chunks, generated nodes and
player edits; it is sampled at most once per selection and cached per column.

The form values describe the same column model as
`get_effective_biome_data`, so a query and generation agree on them.
Combined with `y_min`/`y_max`, they let a definition say where a biome lives:

```lua
    valley_pos_max = 0.05,        -- valley floor beside the river
    valley_depth_max = 3,         -- in flat country
    heat_min = 28, heat_max = 47, -- temperate
```

```lua
    mountain_min = 10, y_min = 20, y_max = 59, priority = 1, -- a mountain body, ahead of the biomes it overlaps
```

## `core.get_biome_terrain(pos)`

Available in server and emerge Lua environments. Returns
`{height = number, slope = number, relief = number}` for Valleys, or `nil` when
no supported terrain sampler is available (including before mapgen setup).
Only X/Z select the surface; Y does not select a cave floor or a vertical chunk.
No mapblocks need to be loaded or generated first.

The query and biome filter use the same deterministic modeled natural surface.
It includes the base 3D density, mountain bodies, mountain caps, solid floor,
`carve_cliffs` and removal of natural floating components. The latter two use
canonical chunk bounds, including components retained at chunk boundaries,
the supporting floor, the native component-size limit and the native seeding
rule: only a column's topmost solid node starts a fill, and not one whose run
of solid nodes reaches the floor or the chunk bottom.

Cave carving and changes to biome materials are excluded: they depend on the
already selected biome, so including their complete effects before selection
would create a dependency cycle. Decorations and player edits are excluded
as well. The query describes the landscape for classification, not the exact
final visible voxel surface. Water does not replace the ground below it.

Surface heights are sampled on a world-aligned lattice every **8 nodes**.
At each lattice point:

- `height` is the highest modeled solid node, in node coordinates.
- `slope` is `atan(g)` in degrees. For each axis, take the larger absolute
  height difference to either adjacent sample, divided by 8. Combine these
  X/Z derivatives with `hypot`; `g` is the larger of that result and each
  absolute diagonal height difference divided by `8 * sqrt(2)`. Opposite
  flanks of a ridge or valley therefore cannot cancel each other.
- `relief` is the highest minus the lowest sampled height in a square with
  radius **16 nodes** (5 by 5 samples).

Between lattice points, all three metrics are bilinearly interpolated.
Negative coordinates use floor alignment. Sampling extends beyond the active
chunk and is independent of its Y range, generation order and saved mapblocks.
These smoothed metrics cannot detect every feature narrower than 8 nodes.
Caches are bounded and owned by each generator. Every cached value depends on
the mapgen parameters and the world position alone, so they are kept across
mapchunks and discarded only when full, without affecting results. Restrictive
candidates and explicit API queries compute metrics on demand; without either,
no heights are sampled.

`core.get_biome_data(pos)` applies terrain restrictions but retains its existing
raw heat/humidity semantics. Valleys adjusts the climate during generation;
this API therefore still does not promise the same biome ID as the generated
biomemap. The new terrain query does not change that older climate distinction.

## `core.get_effective_biome_data(pos)`

Available only in VOPI builds, in the server and mapgen Lua environments.
Check for the function before calling it. It returns `nil` before mapgen
initialization, for unsupported mapgens, or when the modeled climate cannot
be safely used by the biome selector. A returned biome ID of 0 is a valid
fallback, not an unavailable result.

`pos` must contain numeric, finite `x`, `y` and `z`. Coordinates are rounded to
the nearest node, using the existing position APIs' signed half-node addition
followed by truncation. Exact halfway values round away from zero; floating-point
addition at adjacent representable values follows the same `doubleToInt` behavior.
Rounded values
must be within the engine generation bound, -31007 through 31007.
Invalid inputs raise an error. This global coordinate bound is independent of
the world's possibly smaller configured `mapgen_limit`; accepting a query is
not permission to generate a chunk there.

For Valleys, the result contains:

| Field | Meaning |
|---|---|
| `biome` | Native biome selector's ID at the rounded query position. |
| `heat`, `humidity` | Effective climate after the configured Valleys corrections. |
| `raw_heat`, `raw_humidity` | Scalar climate noise before those corrections. |
| `climate_reference_height` | Canonical height used by the climate corrections. |
| `river_bank_height` | Modeled river-bank level, in absolute node coordinates. |
| `base` | Region level before the river-bank clamp, in nodes. |
| `valley_depth` | Valley depth amplitude, in nodes. |
| `valley_pos` | Position in the valley profile, 0 at the river edge, 1 on the ridge. |
| `mountain` | Mountain mask, 0 where no mountain body can rise. |

The climate context and the form depend on exact X/Z and the world's frozen
mapgen parameters, independently of query Y, generated chunks, player edits
and generation order. The column height is the integer-truncated 2D surface,
clamped to the engine's global generation limits; the climate reference is
the maximum of that height and the river-bank level. 3D relief, mountain
bodies, cliff carving, floater removal, the solid floor and caves do not enter
the climate. This is a separate model from the smoothed, post-carving
`get_biome_terrain().height`. The four form fields are the values the biome
form bounds are compared with.

Query Y selects the vertical biome band; it does not replace the climate
reference height. Selection uses the native weights, position and terrain
restrictions, registration-order tie breaking and vertical blending. This is
a model classification, not a lookup of the historical material of a node.
In particular, a chunk's 2D biomemap can record a selection made at another Y,
or reuse a selection from a water surface. Arbitrary-Y biome IDs therefore
need not equal that biomemap. Check actual nodes separately when ground,
water or occupancy matters.

The existing `get_heat`, `get_humidity` and `get_biome_data` APIs retain their
raw-climate semantics. This function does not silently substitute their
answers when the effective model is unavailable.

### Generation and availability

VOPI Valleys uses this deterministic climate for both generation and queries.
No climate-version setting or separate activation is required. Other mapgens
keep their existing climate path; a world's saved `mg_name` selects its mapgen.

When all three climate corrections are disabled, generation only computes
the scalar raw climate; a biome with form bounds still samples the column
model once per selection. A full API query always returns the modeled heights
and the form.

Effective climate must be finite and permit the selector's signed integer
seed conversion throughout the supported Y range. An invalid climate makes
this query return `nil`. If encountered during generation, it causes a
reported mapgen error and the incomplete chunk is cancelled; it is not
replaced with another climate. The existing raw APIs retain their semantics.

The deterministic model can differ from older Valleys generation. Continuing
an older Valleys world may produce biome transitions at newly generated
regions; existing nodes are not regenerated. This does not change V7 into
Valleys or migrate worlds.
