# Mapgen additions

Three flags extend Mapgen Valleys. `sea_level_rivers` is part of the default
`mgvalleys_spflags` when `IS_VOPI_ENGINE` is on, the other two are opt-in,
and none of them exists in a build without the option, which generates the
upstream terrain unchanged.
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
above steep ground, the cliff carving adds more, and tunnels near the
surface cut further pieces loose. For every column the terrain pass records
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
