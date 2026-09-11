# Mapgen additions

Four flags extend Mapgen Valleys. `sea_level_rivers`, `mountains` and
`remove_floaters` are part of the default `mgvalleys_spflags` when
`IS_VOPI_ENGINE` is on, `carve_cliffs` is opt-in, and none of them exists in
a build without the option, which generates the upstream terrain unchanged.
The code lives in `src/mapgen/mapgen_valleys.cpp`.

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
