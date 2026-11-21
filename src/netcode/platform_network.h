// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#if IS_VOPI_ENGINE

#include "netcode/address.h"
#include "netcode/socket.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

/*
	The platform network (VOPI Engine): datagram sockets that the platform
	layer supplies in place of the device's own UDP sockets, for a world that
	others join through the platform, and for a client that joins a world
	through it. What carries the datagrams, and how its peers are
	authenticated, is the platform's; the engine runs its usual connection
	(con::Connection) on the sockets it is given.

	A platform installs one Provider at startup (setProvider, from the
	startup hook vopi_install_platform_services, platform_state.h). Without
	one, as in a build with no platform layer, a world started for hosting
	refuses to start and a client refuses to join: neither ever falls back
	to a UDP socket.
*/
namespace platform_network
{

// How a game started from the main menu uses the platform network
enum class Mode {
	None, // the device's own sockets, as upstream
	Host, // a world this device hosts through the platform network
	Join, // a world this device joins through the platform network
};

/*
	A world hosted through the platform network.

	The server and this device's own client talk over an in-process channel
	that the platform supplies with both ends; remote peers reach the server
	through the platform. The platform presents the local client and the
	server at addresses in 127.0.0.0/8 and every remote peer at an address
	of its own outside it, which no other remote peer has while that one is
	connected.
*/
class Hosting
{
public:
	virtual ~Hosting() = default;

	// The server's socket; taken once, by the server. nullptr when the
	// hosting ended before the server started.
	virtual std::unique_ptr<IDatagramSocket> takeServerSocket() = 0;
	// The socket of this device's own client, the other end of the
	// in-process channel; taken once, by the client.
	virtual std::unique_ptr<IDatagramSocket> takeLocalClientSocket() = 0;
	// Where the local client reaches the server, on the local client socket
	virtual Address serverAddress() const = 0;
	// Where the server sees the local client, on the server socket
	virtual Address localClientAddress() const = 0;

	// Whether the remote peer at `address` may log in as `name`. A
	// platform that authenticates its peers binds each one to one name.
	// Server thread.
	virtual bool admitsPlayer(const Address &address, const std::string &name) = 0;
	// The remote peers whose sessions the platform ended since the last
	// call; the server drops them at once. Server thread.
	virtual std::vector<Address> takeEndedPeers() = 0;
};

// A client joining a world through the platform network
struct Joining
{
	// The client's socket
	std::unique_ptr<IDatagramSocket> socket;
	// Where the client reaches the server on that socket
	Address server_address;
	// The name and password to log in with. They come from the platform
	// and never pass through a script.
	std::string player_name;
	std::string password;
	// Whether the platform ended the session (the server dropped it, or it
	// was lost); the game polls it every frame and ends at once. Main
	// thread; may be empty.
	std::function<bool()> session_ended;
};

class Provider
{
public:
	virtual ~Provider() = default;

	// Hosting for the world at `world_path`; nullptr and a message for the
	// player in `error` when it cannot start. Main thread.
	virtual std::shared_ptr<Hosting> host(const std::string &world_path,
			std::string &error) = 0;
	// The joining the platform has prepared; false and a message for the
	// player in `error` when there is none. Main thread.
	virtual bool join(Joining &out, std::string &error) = 0;
};

// The installed provider; nullptr until the platform installs one. Set
// once at startup, before any thread reads it; a second call replaces it.
inline Provider *&providerSlot()
{
	static Provider *provider = nullptr;
	return provider;
}

inline void setProvider(Provider *provider) { providerSlot() = provider; }
inline Provider *getProvider() { return providerSlot(); }

}

#endif
