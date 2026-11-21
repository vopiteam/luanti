// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "irrlichttypes.h"
#include "content/subgames.h"
#if IS_VOPI_ENGINE
#include "netcode/platform_network.h"
#include <memory>
#endif

// Information provided from "main"
struct GameParams
{
	GameParams() = default;

	u16 socket_port;
	std::string world_path;
	SubgameSpec game_spec;
	bool is_dedicated_server;
};

enum class ELoginRegister {
	Any = 0,
	Login,
	Register
};

// Information processed by main menu
// TODO: unify with MainMenuData
struct GameStartData : GameParams
{
	GameStartData() = default;

	bool isSinglePlayer() const { return address.empty() && !local_server; }

	std::string name;
	std::string password;
	// If empty, we're hosting a server.
	// This may or may not be in "simple singleplayer mode".
	std::string address;
	// If true, we're hosting a server and are *not* in "simple singleplayer
	// mode".
	bool local_server;

	ELoginRegister allow_login_or_register = ELoginRegister::Any;

	// "world_path" must be kept in sync!
	WorldSpec world_spec;

#if IS_VOPI_ENGINE
	// Host: a local server on the platform network, its own client on an
	// in-process channel (local_server is true, address empty).
	// Join: `joining` holds the socket and the login; `address` names its
	// server address, which no resolution reads, and, not being empty,
	// keeps a local server from starting.
	platform_network::Mode platform_network = platform_network::Mode::None;
	std::shared_ptr<platform_network::Joining> joining;
#endif
};
