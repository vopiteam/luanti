// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2014-2018 kwolekr, Ryan Kwolek <kwolekr@minetest.net>
// Copyright (C) 2015-2018 paramat

#pragma once

#include <unordered_set>
#if IS_VOPI_ENGINE
#include <unordered_map>
#include "irr_v2d.h"
#endif
#include "objdef.h"
#include "noise.h"
#include "nodedef.h"

typedef u16 biome_t;  // copy from mg_biome.h to avoid an unnecessary include

class Mapgen;
class MMVManip;
class PcgRandom;
class Schematic;
namespace treegen { struct TreeDef; }

enum DecorationType {
	DECO_SIMPLE,
	DECO_SCHEMATIC,
	DECO_LSYSTEM
};

#define DECO_PLACE_CENTER_X  0x01
#define DECO_PLACE_CENTER_Y  0x02
#define DECO_PLACE_CENTER_Z  0x04
#define DECO_USE_NOISE       0x08
#define DECO_FORCE_PLACEMENT 0x10
#define DECO_LIQUID_SURFACE  0x20
#define DECO_ALL_FLOORS      0x40
#define DECO_ALL_CEILINGS    0x80
#if IS_VOPI_ENGINE
// The biome filter looks at the surface the decoration stands on instead
// of the column's biomemap entry
#define DECO_BIOME_AT_SURFACE 0x100
#endif

extern const FlagDesc flagdesc_deco[];

#if IS_VOPI_ENGINE
// A lattice of decoration slots in world coordinates. The XZ plane is cut
// into cells of `cell` nodes a side and a cell holds one slot at most. The
// cells are coloured two by two, so the eight cells around a cell all have
// another colour than it has; a cell takes the first of a few hashed points
// inside itself that lies further than `apart` nodes, along one axis at
// least, from the slot of every cell around it of an earlier colour, and
// stays empty when none does. Two cells that do not touch are a whole cell
// apart, so no two slots lie within `apart` nodes of each other along both
// axes. A slot is a function of the map seed, the lattice and the cell's
// coordinates alone, so the distance holds across mapchunks whatever order
// they generate in.
class DecoLattice {
public:
	// The points a cell tries before it stays empty. Part of what a world
	// is: another number moves every slot.
	static constexpr u32 TRIES = 16;

	static constexpr s32 CELL_MAX = 32767;

	// Needs 1 <= apart < cell <= CELL_MAX
	DecoLattice(s32 mapseed, u32 id, s32 cell, s32 apart);

	// The cell a coordinate lies in
	static s32 cellOf(s32 coord, s32 cell)
	{
		return coord >= 0 ? coord / cell : -((-coord - 1) / cell) - 1;
	}

	// The slot of a cell as X and Z, false for a cell that holds none
	bool getSlot(s32 cx, s32 cz, v2s32 *pos);

	// The roll of a cell, in [0, 1): which decoration its slot belongs to
	float getRoll(s32 cx, s32 cz) const;

	// The share of the cells that hold a slot, measured on a lattice of
	// its own, so the same on every platform and for every map seed
	static float measureFill(s32 cell, s32 apart);

private:
	struct Slot {
		bool taken;
		v2s32 pos;
	};

	u64 hash(s32 cx, s32 cz, u32 n) const;
	Slot findSlot(s32 cx, s32 cz);

	u64 m_base;
	s32 m_cell;
	s32 m_apart;
	std::unordered_map<u64, Slot> m_slots;
};

// The lattice a decoration is placed on and the part of a slot's roll
// that is its own. A cell of 0 is no lattice.
struct DecoSpacing {
	s32 cell = 0;
	s32 apart = 0;
	u32 seed = 0;
	float from = 0.0f;
	float to = 1.0f;
};
#endif


class Decoration : public ObjDef, public NodeResolver {
public:
	Decoration() = default;
	virtual ~Decoration() = default;

	virtual void resolveNodeNames();

	bool canPlaceDecoration(MMVManip *vm, v3s16 p);
	void placeDeco(Mapgen *mg, u32 blockseed, v3s16 nmin, v3s16 nmax);
#if IS_VOPI_ENGINE
	bool filtersAtSurface(const Mapgen *mg) const;
	void placeOnHeightmap(Mapgen *mg, PcgRandom *ps, v3s16 nmin, v3s16 nmax,
		s16 x, s16 z, int mapindex, bool at_surface);
	void placeSpaced(Mapgen *mg, PcgRandom *ps, v3s16 nmin, v3s16 nmax,
		bool at_surface);
#endif

	virtual size_t generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling) = 0;

	u32 flags = 0;
	int mapseed = 0;
	std::vector<content_t> c_place_on;
	s16 sidelen = 1;
	s16 y_min;
	s16 y_max;
	float fill_ratio = 0.0f;
	NoiseParams np;
	std::vector<content_t> c_spawnby;
	s16 nspawnby;
	s16 place_offset_y = 0;
	s16 check_offset = -1;

	std::unordered_set<biome_t> biomes;
#if IS_VOPI_ENGINE
	DecoSpacing spacing;
#endif

protected:
	void cloneTo(Decoration *def) const;
};


class DecoSimple : public Decoration {
public:
	ObjDef *clone() const;

	virtual void resolveNodeNames();
	virtual size_t generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling);

	std::vector<content_t> c_decos;
	s16 deco_height;
	s16 deco_height_max;
	u8 deco_param2;
	u8 deco_param2_max;
};


class DecoSchematic : public Decoration {
public:
	ObjDef *clone() const;

	DecoSchematic() = default;
	virtual ~DecoSchematic();

	virtual size_t generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling);

	Rotation rotation;
	Schematic *schematic = nullptr;
	bool was_cloned = false; // see FIXME inside DecoSchemtic::clone()
};


class DecoLSystem : public Decoration {
public:
	ObjDef *clone() const;

	virtual size_t generate(MMVManip *vm, PcgRandom *pr, v3s16 p, bool ceiling);

	// In case it gets cloned it uses the same tree def.
	std::shared_ptr<treegen::TreeDef> tree_def;
};


class DecorationManager : public ObjDefManager {
public:
	DecorationManager(IGameDef *gamedef);
	virtual ~DecorationManager() = default;

	DecorationManager *clone() const;

	const char *getObjectTitle() const
	{
		return "decoration";
	}

	static Decoration *create(DecorationType type)
	{
		switch (type) {
		case DECO_SIMPLE:
			return new DecoSimple;
		case DECO_SCHEMATIC:
			return new DecoSchematic;
		case DECO_LSYSTEM:
			return new DecoLSystem;
		default:
			return NULL;
		}
	}

	void placeAllDecos(Mapgen *mg, u32 blockseed, v3s16 nmin, v3s16 nmax);

private:
	DecorationManager() {};
};
