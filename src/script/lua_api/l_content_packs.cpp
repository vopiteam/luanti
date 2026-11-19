// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "lua_api/l_content_packs.h"
#include "lua_api/l_internal.h"
#include "content/subgames.h"
#include "content_vfs.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"

#include <algorithm>
#include <map>

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

// get_world_packs()
int ModApiContentPacks::l_get_world_packs(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::vector<ContentVFS::WorldPack> packs = ContentVFS::get().listWorldPacks();
	lua_createtable(L, (int)packs.size(), 0);
	int i = 1;
	for (const ContentVFS::WorldPack &w : packs) {
		lua_newtable(L);
		lua_pushstring(L, w.pack->id().c_str());
		lua_setfield(L, -2, "id");
		lua_pushinteger(L, w.pack->version());
		lua_setfield(L, -2, "version");
		lua_pushstring(L, ContentVFS::sourceName(w.source));
		lua_setfield(L, -2, "source");
		if (!w.pack->previewName().empty()) {
			lua_pushstring(L, w.pack->previewName().c_str());
			lua_setfield(L, -2, "name");
		}
		if (!w.pack->previewDescription().empty()) {
			lua_pushstring(L, w.pack->previewDescription().c_str());
			lua_setfield(L, -2, "description");
		}
		lua_rawseti(L, -2, i++);
	}
	return 1;
}

// create_world_from_pack(id, name, gameid[, settings])
int ModApiContentPacks::l_create_world_from_pack(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::string id = readParam<std::string>(L, 1);
	const std::string name = readParam<std::string>(L, 2);
	const std::string gameid = readParam<std::string>(L, 3);
	// Invalid settings are the error "error", not a raised one: raised in
	// an async job, it would leave the menu without a result
	std::map<std::string, std::string> settings;
	bool settings_valid = lua_isnoneornil(L, 4) || lua_istable(L, 4);
	if (lua_istable(L, 4)) {
		lua_pushnil(L);
		while (lua_next(L, 4) != 0) {
			// key at -2, value at -1; only the value may be converted in
			// place (a converted key would break lua_next)
			const int key_type = lua_type(L, -2);
			const int value_type = lua_type(L, -1);
			if (key_type == LUA_TSTRING && value_type == LUA_TBOOLEAN) {
				settings[lua_tostring(L, -2)] = lua_toboolean(L, -1) ? "true" : "false";
			} else if (key_type == LUA_TSTRING &&
					(value_type == LUA_TSTRING || value_type == LUA_TNUMBER)) {
				const std::string key = lua_tostring(L, -2);
				settings[key] = lua_tostring(L, -1);
			} else {
				settings_valid = false;
			}
			lua_pop(L, 1);
		}
	}
	if (!settings_valid) {
		errorstream << "create_world_from_pack: settings must be a table of string keys "
				"with string, number or boolean values" << std::endl;
		lua_pushnil(L);
		lua_pushstring(L, worldPackErrorId(WorldPackError::Error));
		return 2;
	}

	std::shared_ptr<const ContentPack> pack;
	for (const ContentVFS::WorldPack &w : ContentVFS::get().listWorldPacks()) {
		if (w.pack->id() == id)
			pack = w.pack;
	}
	if (!pack) {
		lua_pushnil(L);
		lua_pushstring(L, "unknown_pack");
		return 2;
	}

	// As core.create_world: only for a game that is there
	const std::vector<SubgameSpec> games = getAvailableGames();
	if (std::none_of(games.begin(), games.end(),
			[&](const SubgameSpec &spec) { return spec.id == gameid; })) {
		errorstream << "create_world_from_pack: no game " << gameid << std::endl;
		lua_pushnil(L);
		lua_pushstring(L, worldPackErrorId(WorldPackError::Error));
		return 2;
	}

	std::string path;
	const WorldPackError error = createWorldFromPack(*pack,
			porting::path_user + DIR_DELIM + "worlds",
			porting::path_user + DIR_DELIM + "world_packs.staging",
			name, gameid, settings, path);
	if (error != WorldPackError::None) {
		lua_pushnil(L);
		lua_pushstring(L, worldPackErrorId(error));
		return 2;
	}
	lua_pushstring(L, path.c_str());
	return 1;
}

void ModApiContentPacks::Initialize(lua_State *L, int top)
{
	API_FCT(is_content_pack_mounted);
	API_FCT(get_content_pack_info);
	API_FCT(get_mounted_content_packs);
}

void ModApiContentPacks::InitializeMainMenu(lua_State *L, int top)
{
	API_FCT(get_world_packs);
	API_FCT(create_world_from_pack);
}
