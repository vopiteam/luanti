// VOPI Engine
// SPDX-License-Identifier: Proprietary
// Copyright (C) 2026 VOPI Team

#pragma once

#if IS_VOPI_ENGINE

#include <string>
#include "irrlichttypes.h"

namespace video
{
	class IVideoDriver;
	class ITexture;
	class IImage;
}

struct ItemMesh;
class Client;

// Output directory of the --dump-baked-icons command line option;
// empty when the dump mode is inactive.
extern std::string g_dump_baked_icons_path;

/*
	Renders a generic-node ItemMesh once into a shared offscreen target
	at `supersample` times the configured resolution, box-downsamples it,
	applies a screen-space outline (alpha dilate, driven by the
	inventory_mesh_outline settings) plus an edge bleed for clean bilinear
	filtering, and returns the result as a CPU-side image (caller drops).

	Returns nullptr when baking is unavailable (no render-to-texture
	support, texture creation or readback failure).

	Must be called from the render thread.
*/
video::IImage *bakeItemIconImage(video::IVideoDriver *driver,
		ItemMesh *imesh, u32 supersample);

/*
	--dump-baked-icons implementation: bakes the icon of every item
	carrying the "icon_bake" group into
	`<out_dir>/<modname>/<inventory_image>` PNG files (the group value
	sets the icon facedir, see createItemMesh), reusing already-produced
	filenames when several nodes share one icon. Also logs every
	generic-node item that has neither an inventory image nor the group —
	the candidates that would silently render as live meshes.

	Returns false when any marked item failed to validate or bake; the
	summary is logged either way, ending with an "ICON DUMP OK" or
	"ICON DUMP FAILED" marker line for wrapper scripts.
*/
bool dumpBakedIcons(Client *client, const std::string &out_dir);

#endif
