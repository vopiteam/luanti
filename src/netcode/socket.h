// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "irrlichttypes.h"

class Address;

void sockets_init();
void sockets_cleanup();

class UDPSocket
{
public:
	UDPSocket() = default;
	UDPSocket(bool ipv6); // calls init()
	~UDPSocket();
	bool init(bool ipv6, bool noExceptions = false);

	void Bind(Address addr);

	void Send(const Address &destination, const void *data, int size);
	// Returns -1 if there is no data
	int Receive(Address &sender, void *data, int size);
	void setTimeoutMs(int timeout_ms);
	// Returns true if there is data, false if timeout occurred
	bool WaitData(int timeout_ms);

	// Debugging purposes only
	int GetHandle() const { return m_handle; };

private:
	int m_handle = -1;
	int m_timeout_ms = -1;
	unsigned short m_addr_family = 0;
};

#if IS_VOPI_ENGINE
#include "address.h"
#include <memory>

/*
	The operations a connection (con::Connection) uses on its datagram
	socket. A connection runs on a UDPSocket unless its creator supplies
	another implementation (con::createMTP).

	The contract the connection's threads rely on:
	- setTimeoutMs() is called once, before the threads start.
	- Receive() may run before Bind() and while Send() runs.
	- The destructor runs after both threads have stopped.
	- Bind() is called once, from the send thread, before any Send(). On
	  the serving side it may throw SocketException, which becomes a bind
	  failure; on the connecting side it must not throw.
	- Send() is called from the send thread only. A datagram that cannot
	  be delivered may be dropped silently, as UDP drops it.
	- Receive() is called from the receive thread only. It waits for a
	  datagram up to the timeout of the last setTimeoutMs() and returns -1
	  when none came; it never blocks longer.
	- GetHandle() may be called from any thread; it only names the socket
	  in log lines.
*/
class IDatagramSocket
{
public:
	virtual ~IDatagramSocket() = default;

	virtual void Bind(Address addr) = 0;
	virtual void Send(const Address &destination, const void *data, int size) = 0;
	virtual int Receive(Address &sender, void *data, int size) = 0;
	virtual void setTimeoutMs(int timeout_ms) = 0;
	virtual int GetHandle() const = 0;
};

// The default implementation: a UDPSocket
class UDPDatagramSocket final : public IDatagramSocket
{
public:
	UDPDatagramSocket(bool ipv6) : m_socket(ipv6) {}

	void Bind(Address addr) override { m_socket.Bind(addr); }
	void Send(const Address &destination, const void *data, int size) override
	{
		m_socket.Send(destination, data, size);
	}
	int Receive(Address &sender, void *data, int size) override
	{
		return m_socket.Receive(sender, data, size);
	}
	void setTimeoutMs(int timeout_ms) override { m_socket.setTimeoutMs(timeout_ms); }
	int GetHandle() const override { return m_socket.GetHandle(); }

private:
	UDPSocket m_socket;
};
#endif
