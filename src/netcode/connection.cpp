// Minetest
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "netcode/connection.h"
#include "netcode/mtp/impl.h"

namespace con
{

IConnection *createMTP(float timeout, bool ipv6, PeerHandler *handler)
{
	// safe minimum across internet networks for ipv4 and ipv6
	constexpr u32 MAX_PACKET_SIZE = 512;
	return new con::Connection(MAX_PACKET_SIZE, timeout, ipv6, handler);
}

#if IS_VOPI_ENGINE
IConnection *createMTP(float timeout, std::unique_ptr<IDatagramSocket> socket,
		PeerHandler *handler)
{
	// The packet size of the overload above: a supplied socket adds its own
	// framing outside it
	constexpr u32 MAX_PACKET_SIZE = 512;
	FATAL_ERROR_IF(!socket, "createMTP: no socket supplied");
	return new con::Connection(MAX_PACKET_SIZE, timeout, std::move(socket), handler);
}
#endif

}
