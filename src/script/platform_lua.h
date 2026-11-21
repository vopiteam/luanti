// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#if IS_VOPI_ENGINE

extern "C" {
struct lua_State;
}

/*
	Lua functions of the platform layer (VOPI Engine). The platform installs
	one registrar at startup (setRegistrar, from the startup hook
	vopi_install_platform_services, platform_state.h); the engine calls it
	where it registers its own functions in each environment below, with the
	`core` table at stack index `top`: the main menu's at every start of a
	main menu session, the pause menu's for each game, the server's on the
	thread that starts the server. Without a registrar, nothing is
	registered. A second setRegistrar replaces the first.
*/
namespace platform_lua
{

enum class Environment {
	MainMenu,
	PauseMenu,
	Server,
};

typedef void (*Registrar)(lua_State *L, int top, Environment environment);

inline Registrar &registrarSlot()
{
	static Registrar registrar = nullptr;
	return registrar;
}

inline void setRegistrar(Registrar registrar) { registrarSlot() = registrar; }

inline void registerFunctions(lua_State *L, int top, Environment environment)
{
	if (Registrar registrar = registrarSlot())
		registrar(L, top, environment);
}

}

#endif
