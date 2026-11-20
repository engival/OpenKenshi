#include "platform/socket.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL	// macOS: no per-call flag (SO_NOSIGPIPE is set on the socket instead)
#define MSG_NOSIGNAL 0
#endif

namespace plat {

Socket tcpConnect(const std::string& host, const std::string& port, std::string& err)
{
	addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) { err = gai_strerror(rc); return kNoSocket; }
	int fd = -1;
	for (addrinfo* a = res; a && fd < 0; a = a->ai_next)
	{
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) { close(fd); fd = -1; }
	}
	freeaddrinfo(res);
	if (fd < 0) { err = strerror(errno); return kNoSocket; }
#ifdef SO_NOSIGPIPE	// macOS has no MSG_NOSIGNAL
	{ const int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

void tcpClose(Socket s)
{
	if (s != kNoSocket) { close((int)s); }
}

Recv tcpRecv(Socket s, void* buf, size_t cap, size_t& n, std::string& err)
{
	for (;;)
	{
		const ssize_t r = recv((int)s, buf, cap, 0);
		if (r > 0) { n = (size_t)r; return Recv::Data; }
		if (r == 0) { return Recv::Closed; }
		if (errno == EAGAIN || errno == EWOULDBLOCK) { return Recv::WouldBlock; }
		if (errno != EINTR) { err = strerror(errno); return Recv::Error; }
	}
}

bool tcpSend(Socket s, const void* data, size_t len)
{
	return send((int)s, data, len, MSG_NOSIGNAL) == (ssize_t)len;
}

}
