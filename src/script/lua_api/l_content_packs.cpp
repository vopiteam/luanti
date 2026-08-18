// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "lua_api/l_content_packs.h"
#include "lua_api/l_internal.h"
#include "content_vfs.h"

static void push_mount_info(lua_State *L, const ContentVFS::MountInfo &m)
{
	lua_newtable(L);
	lua_pushstring(L, m.pack->id().c_str());
	lua_setfield(L, -2, "id");
	lua_pushstring(L, m.pack->type().c_str());
	lua_setfield(L, -2, "type");
	lua_pushinteger(L, m.pack->version());
	lua_setfield(L, -2, "version");
	lua_pushstring(L, ContentVFS::sourceName(m.source));
	lua_setfield(L, -2, "source");
	lua_pushstring(L, m.pack->mountSpec().c_str());
	lua_setfield(L, -2, "mount");
	lua_pushboolean(L, m.pack->isEncrypted());
	lua_setfield(L, -2, "encrypted");
	lua_pushinteger(L, (lua_Integer)m.pack->entries().size());
	lua_setfield(L, -2, "files");
}

// is_content_pack_mounted(id)
int ModApiContentPacks::l_is_content_pack_mounted(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::string id = readParam<std::string>(L, 1);
	lua_pushboolean(L, ContentVFS::get().getPack(id) != nullptr);
	return 1;
}

// get_content_pack_info(id)
int ModApiContentPacks::l_get_content_pack_info(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::string id = readParam<std::string>(L, 1);
	for (const ContentVFS::MountInfo &m : ContentVFS::get().getMounts()) {
		if (m.pack->id() == id) {
			push_mount_info(L, m);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

// get_mounted_content_packs()
int ModApiContentPacks::l_get_mounted_content_packs(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::vector<ContentVFS::MountInfo> mounts = ContentVFS::get().getMounts();
	lua_createtable(L, (int)mounts.size(), 0);
	int i = 1;
	for (const ContentVFS::MountInfo &m : mounts) {
		push_mount_info(L, m);
		lua_rawseti(L, -2, i++);
	}
	return 1;
}

void ModApiContentPacks::Initialize(lua_State *L, int top)
{
	API_FCT(is_content_pack_mounted);
	API_FCT(get_content_pack_info);
	API_FCT(get_mounted_content_packs);
}
