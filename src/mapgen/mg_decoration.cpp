// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2014-2018 kwolekr, Ryan Kwolek <kwolekr@minetest.net>
// Copyright (C) 2015-2018 paramat

#include "mg_decoration.h"
#include "mg_schematic.h"
#include "mg_biome.h"
#include "mapgen.h"
#include "noise.h"
#include "map.h"
#include <algorithm>
#include <vector>
#if IS_VOPI_ENGINE
#include <cstdlib>
#endif
#include "mapgen/treegen.h"


const FlagDesc flagdesc_deco[] = {
	{"place_center_x",  DECO_PLACE_CENTER_X},
	{"place_center_y",  DECO_PLACE_CENTER_Y},
	{"place_center_z",  DECO_PLACE_CENTER_Z},
	{"force_placement", DECO_FORCE_PLACEMENT},
	{"liquid_surface",  DECO_LIQUID_SURFACE},
	{"all_floors",      DECO_ALL_FLOORS},
	{"all_ceilings",    DECO_ALL_CEILINGS},
#if IS_VOPI_ENGINE
	{"biome_at_surface", DECO_BIOME_AT_SURFACE},
#endif
	{NULL,              0}
};


///////////////////////////////////////////////////////////////////////////////


DecorationManager::DecorationManager(IGameDef *gamedef) :
	ObjDefManager(gamedef, OBJDEF_DECORATION)
{
}


void DecorationManager::placeAllDecos(Mapgen *mg, u32 blockseed,
	v3s16 nmin, v3s16 nmax)
{
	for (size_t i = 0; i != m_objects.size(); i++) {
		Decoration *deco = (Decoration *)m_objects[i];
		if (!deco)
			continue;

		deco->placeDeco(mg, blockseed, nmin, nmax);
		blockseed++;
	}
}

DecorationManager *DecorationManager::clone() const
{
	auto mgr = new DecorationManager();
	ObjDefManager::cloneTo(mgr);
	return mgr;
}


///////////////////////////////////////////////////////////////////////////////


void Decoration::resolveNodeNames()
{
	getIdsFromNrBacklog(&c_place_on);
	getIdsFromNrBacklog(&c_spawnby);
}


#if IS_VOPI_ENGINE
// Whether the biome filter looks at the surface a decoration stands on
// rather than at the column's biomemap entry, the biome of the first stone
// surface from the top: the flag, a filter to apply, and a biome generator
// whose climate maps the generation pass filled for this mapchunk. With
// the mapgen's biomes off the maps hold nothing of this mapchunk, and the
// filter stays with the biomemap, which then names no biome, as upstream.
bool Decoration::filtersAtSurface(const Mapgen *mg) const
{
	return (flags & DECO_BIOME_AT_SURFACE) && !biomes.empty() &&
		mg->biomegen && (mg->flags & MG_BIOMES);
}


// One step of a stateless integer hash, in whole 64-bit arithmetic, so the
// same on every platform
static inline u64 lattice_mix(u64 h, u64 v)
{
	h ^= v;
	h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
	h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
	return h ^ (h >> 31);
}


DecoLattice::DecoLattice(s32 mapseed, u32 id, s32 cell, s32 apart) :
	m_cell(cell), m_apart(apart)
{
	u64 h = 0x9E3779B97F4A7C15ULL;
	h = lattice_mix(h, (u64)(s64)mapseed);
	h = lattice_mix(h, id);
	h = lattice_mix(h, (u64)cell);
	m_base = lattice_mix(h, (u64)apart);
}


u64 DecoLattice::hash(s32 cx, s32 cz, u32 n) const
{
	u64 h = lattice_mix(m_base, (u64)(s64)cx);
	h = lattice_mix(h, (u64)(s64)cz);
	return lattice_mix(h, n);
}


float DecoLattice::getRoll(s32 cx, s32 cz) const
{
	// Beyond the numbers of the points a cell tries
	return (hash(cx, cz, 0xFFFFFFFFU) & 0xFFFFFF) / 16777216.0f;
}


bool DecoLattice::getSlot(s32 cx, s32 cz, v2s32 *pos)
{
	const u64 key = ((u64)(u32)cx << 32) | (u32)cz;
	auto it = m_slots.find(key);
	if (it == m_slots.end())
		it = m_slots.emplace(key, findSlot(cx, cz)).first;
	if (it->second.taken)
		*pos = it->second.pos;
	return it->second.taken;
}


DecoLattice::Slot DecoLattice::findSlot(s32 cx, s32 cz)
{
	// The colour of a cell is the parity of its coordinates, 0 to 3. A cell
	// of colour 0 looks at no other, and each later colour at the earlier
	// ones alone, so a slot depends on the cells within three of its own.
	auto colour_of = [](s32 x, s32 z) { return (u32)(x & 1) | ((u32)(z & 1) << 1); };
	const u32 colour = colour_of(cx, cz);

	v2s32 earlier[8];
	u32 count = 0;
	for (s32 dz = -1; dz <= 1; dz++)
	for (s32 dx = -1; dx <= 1; dx++) {
		if (colour_of(cx + dx, cz + dz) < colour &&
				getSlot(cx + dx, cz + dz, &earlier[count]))
			count++;
	}

	for (u32 n = 0; n < TRIES; n++) {
		const u64 h = hash(cx, cz, n);
		const v2s32 p(cx * m_cell + (s32)((h & 0xFFFFFFFFU) % (u32)m_cell),
			cz * m_cell + (s32)((h >> 32) % (u32)m_cell));
		bool clear = true;
		for (u32 i = 0; i < count && clear; i++) {
			clear = std::abs(p.X - earlier[i].X) > m_apart ||
				std::abs(p.Y - earlier[i].Y) > m_apart;
		}
		if (clear)
			return {true, p};
	}
	return {false, v2s32()};
}


float DecoLattice::measureFill(s32 cell, s32 apart)
{
	// Every colour in equal number
	const s32 side = 128;
	DecoLattice lattice(0, 0, cell, apart);
	u32 taken = 0;
	v2s32 pos;
	for (s32 cz = 0; cz < side; cz++)
	for (s32 cx = 0; cx < side; cx++) {
		if (lattice.getSlot(cx, cz, &pos))
			taken++;
	}
	return (float)taken / (side * side);
}
#endif


bool Decoration::canPlaceDecoration(MMVManip *vm, v3s16 p)
{
	// Note that `p` refers to the node the decoration will be placed ontop of,
	// not to the decoration itself.

	// Check if the decoration can be placed on this node
	u32 vi = vm->m_area.index(p);
	if (!CONTAINS(c_place_on, vm->m_data[vi].getContent()))
		return false;

	// Don't continue if there are no spawnby constraints
	if (nspawnby == -1)
		return true;

	int nneighs = 0;
	static const v3s16 dirs[8] = {
		v3s16( 0, 1,  1),
		v3s16( 0, 1, -1),
		v3s16( 1, 1,  0),
		v3s16(-1, 1,  0),
		v3s16( 1, 1,  1),
		v3s16(-1, 1,  1),
		v3s16(-1, 1, -1),
		v3s16( 1, 1, -1)
	};


	// Check these 16 neighboring nodes for enough spawnby nodes
	for (size_t i = 0; i != ARRLEN(dirs); i++) {
		u32 index = vm->m_area.index(p + dirs[i]);
		if (!vm->m_area.contains(index))
			continue;

		if (CONTAINS(c_spawnby, vm->m_data[index].getContent()))
			nneighs++;
	}

	if (check_offset != 0) {
		const v3s16 dir_offset(0, check_offset,  0);

		for (size_t i = 0; i != ARRLEN(dirs); i++) {
			u32 index = vm->m_area.index(p + dirs[i] + dir_offset);
			if (!vm->m_area.contains(index))
				continue;

			if (CONTAINS(c_spawnby, vm->m_data[index].getContent()))
				nneighs++;
		}

	}
	if (nneighs < nspawnby)
		return false;

	return true;
}


void Decoration::placeDeco(Mapgen *mg, u32 blockseed, v3s16 nmin, v3s16 nmax)
{
	// Skip if y ranges do not overlap
	if (nmax.Y < y_min || y_max < nmin.Y)
		return;

#if IS_VOPI_ENGINE
	const bool at_surface = filtersAtSurface(mg);
#endif
	PcgRandom ps(blockseed + 53);
#if IS_VOPI_ENGINE
	if (spacing.cell > 0) {
		placeSpaced(mg, &ps, nmin, nmax, at_surface);
		return;
	}
#endif
	int carea_size = nmax.X - nmin.X + 1;
	if (nmax.Z - nmin.Z + 1 != carea_size) {
		// TODO: this is a stupid restriction, which we should lift
		throw BaseException("Decoration::placeDeco requires a square area (XZ)");
	}

	// Divide area into parts
	// If chunksize is changed it may no longer be divisable by sidelen
	if (carea_size % sidelen != 0)
		sidelen = carea_size;

	int area = sidelen * sidelen;

	for (s16 z0 = 0; z0 < carea_size; z0 += sidelen)
	for (s16 x0 = 0; x0 < carea_size; x0 += sidelen) {
		v2s16 p2d_min(nmin.X + x0, nmin.Z + z0);
		v2s16 p2d_max(nmin.X + x0 + sidelen - 1, nmin.Z + z0 + sidelen - 1);

		bool cover = false;
		// Amount of decorations
		float nval = (flags & DECO_USE_NOISE) ?
			NoiseFractal2D(&np, p2d_min.X + sidelen / 2, p2d_min.Y + sidelen / 2, mapseed) :
			fill_ratio;
		u32 deco_count = 0;

		if (nval >= 10.0f) {
			// Complete coverage. Disable random placement to avoid
			// redundant multiple placements at one position.
			cover = true;
			deco_count = area;
		} else {
			float deco_count_f = (float)area * nval;
			if (deco_count_f >= 1.0f) {
				deco_count = deco_count_f;
			} else if (deco_count_f > 0.0f) {
				// For very low density calculate a chance for 1 decoration
				if (ps.next() <= deco_count_f * static_cast<float>(PcgRandom::RANDOM_RANGE))
					deco_count = 1;
			}
		}

		s16 x = p2d_min.X - 1;
		s16 z = p2d_min.Y;

		for (u32 i = 0; i < deco_count; i++) {
			if (!cover) {
				x = ps.range(p2d_min.X, p2d_max.X);
				z = ps.range(p2d_min.Y, p2d_max.Y);
			} else {
				x++;
				if (x == p2d_max.X + 1) {
					z++;
					x = p2d_min.X;
				}
			}
			int mapindex = carea_size * (z - nmin.Z) + (x - nmin.X);

			if ((flags & DECO_ALL_FLOORS) ||
					(flags & DECO_ALL_CEILINGS)) {
				// All-surfaces decorations
				// Check biome of column
#if IS_VOPI_ENGINE
				// With the biome at every surface the column cannot be
				// refused ahead of its surfaces
				if (!at_surface && mg->biomemap && !biomes.empty()) {
#else
				if (mg->biomemap && !biomes.empty()) {
#endif
					auto iter = biomes.find(mg->biomemap[mapindex]);
					if (iter == biomes.end())
						continue;
				}

				// Get all floors and ceilings in node column
				u16 size = (nmax.Y - nmin.Y + 1) / 2;
				std::vector<s16> floors;
				std::vector<s16> ceilings;
				floors.reserve(size);
				ceilings.reserve(size);

#if IS_VOPI_ENGINE
				// Only the surfaces the Y range admits. A floor shows at
				// its own Y against the node above it and a ceiling at its
				// own Y against the node below, so one node more each way
				// finds every surface of the range the whole scan would.
				const s16 scan_min = std::max<int>(nmin.Y, (int)y_min - 1);
				const s16 scan_max = std::min<int>(nmax.Y, (int)y_max + 1);
				mg->getSurfaces(v2s16(x, z), scan_min, scan_max, floors, ceilings);

				// The biome at a surface, selected as the biome pass
				// selects it: once for a run of surfaces between two Y
				// limits of any biome, at the first surface of the run.
				// Both lists run from the top down, each on its own.
				const Biome *surface_biome = nullptr;
				s16 biome_y_next = 0;
				auto surface_allowed = [&](s16 y) {
					if (!at_surface)
						return true;
					if (!surface_biome || y <= biome_y_next) {
						surface_biome = mg->biomegen->getBiomeAtIndex(
							mapindex, v3s16(x, y, z));
						biome_y_next = mg->biomegen->getNextTransitionY(y);
					}
					return surface_biome &&
						biomes.find(surface_biome->index) != biomes.end();
				};
#else
				mg->getSurfaces(v2s16(x, z), nmin.Y, nmax.Y, floors, ceilings);
#endif

				if (flags & DECO_ALL_FLOORS) {
					// Floor decorations
					for (const s16 y : floors) {
						if (y < y_min || y > y_max)
							continue;

#if IS_VOPI_ENGINE
						if (!surface_allowed(y))
							continue;
#endif
						v3s16 pos(x, y, z);
						if (generate(mg->vm, &ps, pos, false))
							mg->gennotify.addDecorationEvent(pos, index);
					}
				}

				if (flags & DECO_ALL_CEILINGS) {
					// Ceiling decorations
#if IS_VOPI_ENGINE
					surface_biome = nullptr;
#endif
					for (const s16 y : ceilings) {
						if (y < y_min || y > y_max)
							continue;

#if IS_VOPI_ENGINE
						if (!surface_allowed(y))
							continue;
#endif
						v3s16 pos(x, y, z);
						if (generate(mg->vm, &ps, pos, true))
							mg->gennotify.addDecorationEvent(pos, index);
					}
				}
			} else { // Heightmap decorations
#if IS_VOPI_ENGINE
				placeOnHeightmap(mg, &ps, nmin, nmax, x, z, mapindex, at_surface);
#else
				s16 y = -MAX_MAP_GENERATION_LIMIT;
				if (flags & DECO_LIQUID_SURFACE)
					y = mg->findLiquidSurface(v2s16(x, z), nmin.Y, nmax.Y);
				else if (mg->heightmap)
					y = mg->heightmap[mapindex];
				else
					y = mg->findGroundLevel(v2s16(x, z), nmin.Y, nmax.Y);

				if (y < y_min || y > y_max || y < nmin.Y || y > nmax.Y)
					continue;

				if (mg->biomemap && !biomes.empty()) {
					auto iter = biomes.find(mg->biomemap[mapindex]);
					if (iter == biomes.end())
						continue;
				}

				v3s16 pos(x, y, z);
				if (generate(mg->vm, &ps, pos, false))
					mg->gennotify.addDecorationEvent(pos, index);
#endif
			}
		}
	}

	return;
}


#if IS_VOPI_ENGINE
// One decoration on the surface of the column at X and Z, where the Y range
// and the biome filter admit it: the heightmap placement, for a point of the
// scatter and for a slot of the lattice alike.
void Decoration::placeOnHeightmap(Mapgen *mg, PcgRandom *ps, v3s16 nmin,
	v3s16 nmax, s16 x, s16 z, int mapindex, bool at_surface)
{
	s16 y = -MAX_MAP_GENERATION_LIMIT;
	if (flags & DECO_LIQUID_SURFACE)
		y = mg->findLiquidSurface(v2s16(x, z), nmin.Y, nmax.Y);
	else if (mg->heightmap)
		y = mg->heightmap[mapindex];
	else
		y = mg->findGroundLevel(v2s16(x, z), nmin.Y, nmax.Y);

	if (y < y_min || y > y_max || y < nmin.Y || y > nmax.Y)
		return;

	if (at_surface) {
		// The top walkable node of the column, or the liquid
		const Biome *biome = mg->biomegen->getBiomeAtIndex(
			mapindex, v3s16(x, y, z));
		if (!biome || biomes.find(biome->index) == biomes.end())
			return;
	} else if (mg->biomemap && !biomes.empty()) {
		auto iter = biomes.find(mg->biomemap[mapindex]);
		if (iter == biomes.end())
			return;
	}

	v3s16 pos(x, y, z);
	if (generate(mg->vm, ps, pos, false))
		mg->gennotify.addDecorationEvent(pos, index);
}


// The decorations of a lattice: the slots of the cells the area overlaps
// that lie inside it and whose roll falls in this decoration's part. The
// cells are walked in one order, so the decoration's generator, which rolls
// a schematic's rotation and probabilities, gives every slot the same
// numbers whenever the area generates. A slot the ground or the biome
// refuses stays empty.
void Decoration::placeSpaced(Mapgen *mg, PcgRandom *ps, v3s16 nmin, v3s16 nmax,
	bool at_surface)
{
	const int width = nmax.X - nmin.X + 1;
	DecoLattice lattice(mg->seed, spacing.seed, spacing.cell, spacing.apart);
	const s32 cx_max = DecoLattice::cellOf(nmax.X, spacing.cell);
	const s32 cz_max = DecoLattice::cellOf(nmax.Z, spacing.cell);

	for (s32 cz = DecoLattice::cellOf(nmin.Z, spacing.cell); cz <= cz_max; cz++)
	for (s32 cx = DecoLattice::cellOf(nmin.X, spacing.cell); cx <= cx_max; cx++) {
		const float roll = lattice.getRoll(cx, cz);
		if (roll < spacing.from || roll >= spacing.to)
			continue;

		v2s32 slot;
		if (!lattice.getSlot(cx, cz, &slot))
			continue;
		// A cell on the edge of the area may hold its slot outside it
		if (slot.X < nmin.X || slot.X > nmax.X ||
				slot.Y < nmin.Z || slot.Y > nmax.Z)
			continue;

		placeOnHeightmap(mg, ps, nmin, nmax, slot.X, slot.Y,
			width * (slot.Y - nmin.Z) + (slot.X - nmin.X), at_surface);
	}
}
#endif


void Decoration::cloneTo(Decoration *def) const
{
	ObjDef::cloneTo(def);
	def->flags = flags;
	def->mapseed = mapseed;
	def->c_place_on = c_place_on;
	def->check_offset = check_offset;
	def->sidelen = sidelen;
	def->y_min = y_min;
	def->y_max = y_max;
	def->fill_ratio = fill_ratio;
	def->np = np;
	def->c_spawnby = c_spawnby;
	def->nspawnby = nspawnby;
	def->place_offset_y = place_offset_y;
	def->biomes = biomes;
#if IS_VOPI_ENGINE
	def->spacing = spacing;
#endif
}


///////////////////////////////////////////////////////////////////////////////


ObjDef *DecoSimple::clone() const
{
	auto def = new DecoSimple();
	Decoration::cloneTo(def);

	def->c_decos = c_decos;
	def->deco_height = deco_height;
	def->deco_height_max = deco_height_max;
	def->deco_param2 = deco_param2;
	def->deco_param2_max = deco_param2_max;

	return def;
}


void DecoSimple::resolveNodeNames()
{
	Decoration::resolveNodeNames();
	getIdsFromNrBacklog(&c_decos);
}


size_t DecoSimple::generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling)
{
	// Don't bother if there aren't any decorations to place
	if (c_decos.empty())
		return 0;

	if (!canPlaceDecoration(vm, p))
		return 0;

	// Check for placement outside the voxelmanip volume
	if (ceiling) {
		// Ceiling decorations
		// 'place offset y' is inverted
		if (p.Y - place_offset_y - std::max(deco_height, deco_height_max) <
				vm->m_area.MinEdge.Y)
			return 0;

		if (p.Y - 1 - place_offset_y > vm->m_area.MaxEdge.Y)
			return 0;

	} else { // Heightmap and floor decorations
		if (p.Y + place_offset_y + std::max(deco_height, deco_height_max) >
				vm->m_area.MaxEdge.Y)
			return 0;

		if (p.Y + 1 + place_offset_y < vm->m_area.MinEdge.Y)
			return 0;
	}

	content_t c_place = c_decos[pr->range(0, c_decos.size() - 1)];
	s16 height = (deco_height_max > 0) ?
		pr->range(deco_height, deco_height_max) : deco_height;
	u8 param2 = (deco_param2_max > 0) ?
		pr->range(deco_param2, deco_param2_max) : deco_param2;
	bool force_placement = (flags & DECO_FORCE_PLACEMENT);

	const v3s32 &em = vm->m_area.getExtent();
	u32 vi = vm->m_area.index(p);

	if (ceiling) {
		// Ceiling decorations
		// 'place offset y' is inverted
		VoxelArea::add_y(em, vi, -place_offset_y);

		for (int i = 0; i < height; i++) {
			VoxelArea::add_y(em, vi, -1);
			content_t c = vm->m_data[vi].getContent();
			if (c != CONTENT_AIR && c != CONTENT_IGNORE && !force_placement)
				break;

			vm->m_data[vi] = MapNode(c_place, 0, param2);
		}
	} else { // Heightmap and floor decorations
		VoxelArea::add_y(em, vi, place_offset_y);

		for (int i = 0; i < height; i++) {
			VoxelArea::add_y(em, vi, 1);
			content_t c = vm->m_data[vi].getContent();
			if (c != CONTENT_AIR && c != CONTENT_IGNORE && !force_placement)
				break;

			vm->m_data[vi] = MapNode(c_place, 0, param2);
		}
	}

	return 1;
}


///////////////////////////////////////////////////////////////////////////////


DecoSchematic::~DecoSchematic()
{
	if (was_cloned)
		delete schematic;
}


ObjDef *DecoSchematic::clone() const
{
	auto def = new DecoSchematic();
	Decoration::cloneTo(def);
	NodeResolver::cloneTo(def);

	def->rotation = rotation;
	/* FIXME: We do not own this schematic, yet we only have a pointer to it
	 * and not a handle. We are left with no option but to clone it ourselves.
	 * This is a waste of memory and should be replaced with an alternative
	 * approach sometime. */
	def->schematic = dynamic_cast<Schematic*>(schematic->clone());
	def->was_cloned = true;

	return def;
}


size_t DecoSchematic::generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling)
{
	// Schematic could have been unloaded but not the decoration
	// In this case generate() does nothing (but doesn't *fail*)
	if (schematic == NULL)
		return 0;

	if (!canPlaceDecoration(vm, p))
		return 0;

	if (flags & DECO_PLACE_CENTER_Y) {
		p.Y -= (schematic->size.Y - 1) / 2;
	} else {
		// Only apply 'place offset y' if not 'deco place center y'
		if (ceiling)
			// Shift down so that schematic top layer is level with ceiling
			// 'place offset y' is inverted
			p.Y -= (place_offset_y + schematic->size.Y - 1);
		else
			p.Y += place_offset_y;
	}

	// Check schematic top and base are in voxelmanip
	if (p.Y + schematic->size.Y - 1 > vm->m_area.MaxEdge.Y)
		return 0;

	if (p.Y < vm->m_area.MinEdge.Y)
		return 0;

	Rotation rot = (rotation == ROTATE_RAND) ?
		(Rotation)pr->range(ROTATE_0, ROTATE_270) : rotation;

	if (flags & DECO_PLACE_CENTER_X) {
		if (rot == ROTATE_0 || rot == ROTATE_180)
			p.X -= (schematic->size.X - 1) / 2;
		else
			p.Z -= (schematic->size.X - 1) / 2;
	}
	if (flags & DECO_PLACE_CENTER_Z) {
		if (rot == ROTATE_0 || rot == ROTATE_180)
			p.Z -= (schematic->size.Z - 1) / 2;
		else
			p.X -= (schematic->size.Z - 1) / 2;
	}

	bool force_placement = (flags & DECO_FORCE_PLACEMENT);

#if IS_VOPI_ENGINE
	// The decoration's own generator also rolls the schematic's probabilities,
	// so the same seed places the same trees.
	schematic->blitToVManip(vm, p, rot, force_placement, pr);
#else
	schematic->blitToVManip(vm, p, rot, force_placement);
#endif

	return 1;
}

///////////////////////////////////////////////////////////////////////////////
ObjDef *DecoLSystem::clone() const
{
	auto def = new DecoLSystem();
	Decoration::cloneTo(def);

	def->tree_def = tree_def;
	return def;
}


size_t DecoLSystem::generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling)
{
	if (!canPlaceDecoration(vm, p))
		return 0;

	// Make sure that tree_def can't be modified, since it is shared.
	const auto &ref = *tree_def;
	return treegen::make_ltree(*vm, p, ref);
}
