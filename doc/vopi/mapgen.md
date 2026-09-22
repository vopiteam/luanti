# Mapgen additions

Three flags extend Mapgen Valleys: `sea_level_rivers`, `mountains` and
`remove_floaters`, all part of the default `mgvalleys_spflags` when
`IS_VOPI_ENGINE` is on. None of them exists in a build without the option,
which generates the upstream terrain unchanged.
The code lives in `src/mapgen/mapgen_valleys.cpp`.

## Climate corrections

Valleys uses `calcValleysClimate` for river humidity, altitude drying and
altitude cooling. Its height context is the river-bank level and the
integer-truncated 2D terrain surface of the column, computed from the same
column model in every generation pass and in point queries. Stone generated in
the pass, 3D relief and mountain bodies above the 2D surface do not change
the climate; where a column sits in the landscape is described by the biome
form bounds below instead. The generation pass writes this climate into the
heat and humidity maps of every column, so the biomes of a mapchunk and a
point query see one climate; with all three correction flags disabled it is
the scalar raw climate and the column model is not consulted for it.
`vary_river_depth` still runs on the bulk noise maps of the pass.

The 2D column model is one function, `calcValleysColumn`, shared by the
generator and the biome terrain sampler: the generator feeds it the bulk
noise of the generation area, the sampler the scalar noise of one column.

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

## `remove_floaters`

The 3D relief noise of Valleys leaves pieces of stone hanging in the air
above steep ground, the mountain body pinches lobes off above the ground,
and tunnels near the surface cut further pieces loose. For every column
the terrain pass records a floor: the level under which the base terrain
is solid whatever the 3D noise does. After the caves are carved, every column whose topmost run of
solid nodes ends above that floor seeds a flood fill over connected solid
nodes. A piece that reaches neither the floor nor the edge of the mapchunk
is removed, whatever its size; everything standing on the ground, overhangs
included, is kept. The pass runs before ores and decorations, so nothing is
placed in or on a removed piece.

The biome pass runs earlier still: the caves need the biomemap for the
floors of their entrances, and the removal needs the caves. That pass
records, per column, the biome selected at the first stone surface met from
the top, so where that surface went since, eaten by a cave or removed as a
piece, the record would name the biome of a height the column no longer
has, while the ground under it was laid with the biome selected at its own
surface, which can be another one when the surface that went stood in a
higher Y band. After the caves, the solid floor and the removal, whichever
of them the flags enable, the heightmap is rebuilt, and every column whose
top moved since the biome pass has its biomemap entry selected again at
the surface it has now, as that pass selects it: at the ground, under the
lids the pass lays on water, such as ice, or, with no ground left in the
mapchunk, at the liquid surface, or none. The nodes stay as laid;
decorations and dust, which go by the biomemap, follow the surface that is
there.

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

## Biome climate and form bounds

With `IS_VOPI_ENGINE`, `core.register_biome` accepts inclusive bounds on the
climate the selector receives and on the terrain form of the column. They
select among the existing terrain and change neither noise nor generation
order. All are optional; an omitted bound is unrestricted.

| Field | Meaning | Accepted explicit values |
|---|---|---|
| `heat_min`, `heat_max` | heat as the selector sees it (effective in Valleys) | finite float |
| `humidity_min`, `humidity_max` | humidity as the selector sees it | finite float |
| `variant_min`, `variant_max` | the variant axis: the noise `mg_biome_np_variant` at the column | finite float |
| `base_min`, `base_max` | region level: `terrain_height + valley_depth²` before the river-bank clamp, in nodes | finite float |
| `valley_depth_min`, `valley_depth_max` | valley depth amplitude `valley_depth²`, in nodes | finite float ≥ 0 |
| `valley_pos_min`, `valley_pos_max` | position in the valley profile, 0 at the river edge, 1 on the ridge | 0 to 1 |
| `mountain_min`, `mountain_max` | mountain mask `max(mountain_height, 0) · gate`, 0 where no body can rise | finite float ≥ 0 |
| `body_min`, `body_max` | the mountain body: the height it reaches over the terrain at the column, from its density sampled up the column, within a few nodes of the modeled surface, 0 where no body stands on the column. The mask says how tall a body can be in the region, the body whether and how far one rises here | finite float ≥ 0 |
| `priority` | among the biomes passing every bound, only the highest priority competes by distance | whole number in the s16 range, default 0 |

An inverted range, nonnumeric value, NaN, infinity, a value outside the float
range or a nonzero value rounding to float zero is a registration error, and
so is a priority that is not a whole number in range.

Selection order: Y and position bounds, climate bounds, variant bounds,
form bounds, then
priority, then weighted heat/humidity distance among the survivors, with
the usual registration-order ties and vertical blending. A biome rejected by
a bound cannot enter through `vertical_blend`; if no candidate passes, the
default biome is returned, so keep an unbounded fallback for every climate a
bounded set does not cover.
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
Valleys; elsewhere a biome with an operative form bound is ineligible. The
form is the modeled 2D column, independent of chunks, generated nodes and
player edits; it is sampled at most once per selection and cached per column.
Definitions without bounds preserve the original selection and never sample
the column. A definition is copied with its bounds and priority into the
emerge threads.

The variant axis is a third climate noise beside heat and humidity,
`mg_biome_np_variant`, sampled per column like the form. It takes no part
in the distance: two biomes on one climate point, one with
`variant_max = 0` and the other with `variant_min = 0`, alternate along
the zero line of the noise, so that one climate cell holds two biome
variants. The default noise has scale 0, so the
axis stays at its offset and unbounded definitions never notice it. The
query below reports the value as `variant`.

Two blend noises, `mg_biome_np_base_blend` and
`mg_biome_np_valley_depth_blend`, are added to the region level and the
valley depth of the form before the bounds are compared, in nodes, the way
the heat and humidity blend noises are added to the climate; the valley
depth never falls below zero. A bound on one of these slow fields then cuts
a ragged border rather than the smooth contour of the terrain noise. Both
default to scale 0 and offset 0, which leaves the form as the mapgen
models it.

A shift noise, `mg_biome_np_shift`, displaces the point at which the
climate, the form and the variant of a column are read: the noise at the
column gives the X displacement in nodes, the same noise under another
seed the Z displacement, and heat and humidity are read at the displaced
point, the form and the variant at the nearest column there. Every border
between biomes then moves by the noise wherever it runs, in the shape of
its octaves, while the column's own bank height, its climate corrections
and its modeled surface stay in place. The displacement is meant to be a
few nodes: a form bound on a regional field, the region level, the valley
depth or the mountain mask, does not notice it, whereas a bound on the
position in the valley would put a bank biome a few nodes off the water.
A blend noise adds to a field, so it moves a border by its amplitude
divided by the slope of the field, far where a field crosses a bound
slowly, and scatters islands of the neighbouring biome there; the shift
moves a border by its own amplitude everywhere and leaves no islands. The
displacement of every column of a mapchunk is found once, when the chunk's
climate noise is calculated. The default has scale 0 and offset 0, which
reads every column at its own position.

The form values describe the same column model as
`get_effective_biome_data`, so a query and generation agree on them, blend
and shift included.
Combined with `y_min`/`y_max`, they let a definition say where a biome lives:

```lua
    valley_pos_max = 0.05,        -- valley floor beside the river
    valley_depth_max = 3,         -- in flat country
    heat_min = 28, heat_max = 47, -- temperate
```

```lua
    mountain_min = 10, y_min = 20, y_max = 59, priority = 1, -- a mountain body, ahead of the biomes it overlaps
```

### Distance precision

With `IS_VOPI_ENGINE`, a nonfinite weighted climate distance is recomputed in
double precision from the original float inputs. This keeps finite climate
centers and small positive finite weights selectable when float subtraction,
squaring or division overflows. Ordinary finite float distances retain their
rounding, registration-order ties and vertical blending rules. A distance
equal to `FLT_MAX` is also a valid candidate, rather than the no-candidate
sentinel. Builds without `IS_VOPI_ENGINE` retain the upstream selection path.

## `core.get_biome_terrain(pos)`

Available in server and emerge Lua environments. Returns `{height = number}`
for Valleys, or `nil` when no column model is available (including before
mapgen setup). `pos` is read like the position of `get_effective_biome_data`
below; only X/Z select the column, Y does not select a cave floor or a
vertical chunk. No mapblocks need to be loaded or generated first.

`height` is the highest node of the modeled natural surface of the column:
the base 3D density, mountain bodies, mountain caps, the solid floor and
the removal of natural floating components. The removal uses canonical
chunk bounds, including components retained at chunk
boundaries, the supporting floor, the native component-size limit and the
native seeding rule: only a column's topmost solid node starts a fill, and
not one whose run of solid nodes reaches the floor or the chunk bottom.

Cave carving and changes to biome materials are excluded: they depend on the
already selected biome, so including their complete effects before selection
would create a dependency cycle. Decorations and player edits are excluded
as well. The query describes the landscape, not the exact final visible
voxel surface. Water does not replace the ground below it.

Biome selection never uses this height: a biome lives where its Y bands and
form bounds say. The query is for tools that describe the world without
generating it, such as a map of the modeled surface. The 2D column is the
generator's own model; the 3D surface is modeled a second time per column,
and the unit tests compare it with generated mapchunks column by column, so
the two cannot drift apart unnoticed. Its caches are bounded
and owned by each generator; every cached value depends on the mapgen
parameters and the world position alone, so they are kept across mapchunks
and discarded only when full, without affecting results.

`core.get_biome_data(pos)` retains its raw heat/humidity semantics. Valleys
generates with the effective climate; this API therefore still does not
promise the same biome ID as the generated biomemap.

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
| `variant` | The variant axis at the column, `mg_biome_np_variant`. |
| `valley_pos` | Position in the valley profile, 0 at the river edge, 1 on the ridge. |
| `mountain` | Mountain mask, 0 where no mountain body can rise. |
| `body` | The mountain body of the column: the height it reaches over the terrain, 0 where none stands. |

The climate context and the form depend on exact X/Z and the world's frozen
mapgen parameters, independently of query Y, generated chunks, player edits
and generation order. The column height is the integer-truncated 2D surface,
clamped to the engine's global generation limits; the climate reference is
the maximum of that height and the river-bank level. 3D relief, mountain
bodies, floater removal, the solid floor and caves do not enter the
climate. This is a separate model from the natural surface
`get_biome_terrain().height`. The four form fields are the values the biome
form bounds are compared with.

Query Y selects the vertical biome band; it does not replace the climate
reference height. Selection uses the native weights, position, climate and
form bounds, priority, registration-order tie breaking and vertical
blending. This is
a model classification, not a lookup of the historical material of a node.
In particular, a chunk's 2D biomemap can record a selection made at another Y,
or reuse a selection from a water surface; where the caves or the floating
piece removal moved the surface it is selected again at the one left, see
`remove_floaters`. Arbitrary-Y biome IDs therefore need not equal that
biomemap. Check actual nodes separately when ground, water or occupancy
matters.

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
