// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include "lua_api/l_base.h"

/*
	Read-only view of the ContentVFS mount table for Lua (VOPI Engine).

	Game code decides at mod-load time which registrations to make based on
	which content packs are mounted (CONTENT_DELIVERY_DESIGN §1, §5.4):

	    if core.is_content_pack_mounted("interior_nodes_kitchen_set1") then
	        dofile(modpath .. "/extra/kitchen_set1.lua")
	    end

	Available in every scripting environment (main menu, server, emerge and
	async workers) — the mount table is global and thread-safe to read.
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

public:
	static void Initialize(lua_State *L, int top);
};
