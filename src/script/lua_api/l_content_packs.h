// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include "lua_api/l_base.h"

/*
	The ContentVFS mount table for Lua, read-only (VOPI Engine), and, for the
	main menu only, the world packs and the worlds made from them
	(InitializeMainMenu).

	Game code decides at mod-load time which registrations to make based on
	which content packs are mounted (CONTENT_DELIVERY_DESIGN §1, §5.4):

	    if core.is_content_pack_mounted("interior_nodes_kitchen_set1") then
	        dofile(modpath .. "/extra/kitchen_set1.lua")
	    end

	The mount table functions are available in every scripting environment
	(main menu, server, emerge and async workers) — the mount table is
	global and thread-safe to read.
*/
class ModApiContentPacks : public ModApiBase
{
private:
	// is_content_pack_mounted(id) -> bool
	static int l_is_content_pack_mounted(lua_State *L);

	// get_content_pack_info(id) -> table or nil
	//   {id=, type=, version=, source="bundled"|"installed", mount=,
	//    encrypted=, files=}
	static int l_get_content_pack_info(lua_State *L);

	// get_mounted_content_packs() -> array of the tables above, in lookup
	// priority order
	static int l_get_mounted_content_packs(lua_State *L);

	// get_world_packs() -> array of {id=, version=, source=, name=,
	//   description=}, sorted by id (ContentVFS::listWorldPacks); name and
	//   description are absent when the pack's preview has none
	static int l_get_world_packs(lua_State *L);

	// create_world_from_pack(id, name, gameid[, settings]) -> path, or nil
	//   and "unknown_pack" | "no_space" | "damaged" | "error"
	//   (createWorldFromPack in content_vfs.h). settings: world.mt keys to
	//   set, string keys, string, number or boolean values; other settings
	//   are "error". Blocks while it writes.
	static int l_create_world_from_pack(lua_State *L);

public:
	static void Initialize(lua_State *L, int top);

	// The main menu and its async environment only, not games: the two
	// world pack functions above.
	static void InitializeMainMenu(lua_State *L, int top);
};
