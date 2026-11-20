#include "kutil/platform/socket.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL	// macOS: no per-call flag (SO_NOSIGPIPE is set on the socket instead)
#define MSG_NOSIGNAL 0
#endif

namespace kutil::net {

namespace {

void prepare(int fd)
{
#ifdef SO_NOSIGPIPE	// macOS has no MSG_NOSIGNAL
	const int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
	::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

} // namespace

bool init(std::string&) { return true; }

Socket tcpConnect(const std::string& host, const std::string& port, std::string& err)
{
	addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) { err = ::gai_strerror(rc); return kNoSocket; }
	int fd = -1;
	for (addrinfo* a = res; a && fd < 0; a = a->ai_next)
	{
		fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd >= 0 && ::connect(fd, a->ai_addr, a->ai_addrlen) != 0) { ::close(fd); fd = -1; }
	}
	::freeaddrinfo(res);
	if (fd < 0) { err = std::strerror(errno); return kNoSocket; }
	prepare(fd);
	return fd;
}

Socket tcpListen(const std::string& host, int port, int backlog, int& boundPort, std::string& err)
{
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<std::uint16_t>(port));
	if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) { err = "not an IPv4 address: " + host; return kNoSocket; }
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { err = std::string("socket: ") + std::strerror(errno); return kNoSocket; }
	const int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
	{
		err = "bind " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
		::close(fd);
		return kNoSocket;
	}
	if (::listen(fd, backlog) != 0)
	{
		err = std::string("listen: ") + std::strerror(errno);
		::close(fd);
		return kNoSocket;
	}
	socklen_t length = sizeof address;
	::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
	boundPort = ntohs(address.sin_port);
	prepare(fd);
	return fd;
}

Socket tcpAccept(Socket listener, bool& loopback)
{
	sockaddr_in peer {};
	socklen_t peerLen = sizeof peer;
	const int fd = ::accept(static_cast<int>(listener), reinterpret_cast<sockaddr*>(&peer), &peerLen);
	if (fd < 0)
		return kNoSocket;
	const int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	prepare(fd);
	loopback = peerLen >= sizeof(sa_family_t) && peer.sin_family == AF_INET && (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
	return fd;
}

void tcpClose(Socket s)
{
	if (s != kNoSocket)
		::close(static_cast<int>(s));
}

Io tcpRecv(Socket s, void* buf, std::size_t cap, std::size_t& n, std::string& err)
{
	for (;;)
	{
		const ssize_t r = ::recv(static_cast<int>(s), buf, cap, MSG_DONTWAIT);
		if (r > 0) { n = static_cast<std::size_t>(r); return Io::Ok; }
		if (r == 0) return Io::Closed;
		if (errno == EAGAIN || errno == EWOULDBLOCK) return Io::WouldBlock;
		if (errno != EINTR) { err = std::strerror(errno); return Io::Error; }
	}
}

Io tcpSend(Socket s, const void* data, std::size_t len, std::size_t& n, std::string& err)
{
	for (;;)
	{
		const ssize_t r = ::send(static_cast<int>(s), data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (r > 0) { n = static_cast<std::size_t>(r); return Io::Ok; }
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Io::WouldBlock;
		if (r < 0 && errno == EINTR) continue;
		err = r == 0 ? "send wrote nothing" : std::strerror(errno);
		return Io::Error;
	}
}

bool tcpSendAll(Socket s, const void* data, std::size_t len)
{
	return ::send(static_cast<int>(s), data, len, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
}

int pollSockets(PollEntry* entries, int n, int timeoutMs, std::string& err)
{
	pollfd fds[8]; // the callers poll a listener and one client
	if (n < 1 || n > 8) { err = "pollSockets: 1..8 entries"; return -1; }
	for (int i = 0; i < n; i++)
		fds[i] = {static_cast<int>(entries[i].socket), static_cast<short>((entries[i].wantRead ? POLLIN : 0) | (entries[i].wantWrite ? POLLOUT : 0)), 0};
	const int ready = ::poll(fds, static_cast<nfds_t>(n), timeoutMs);
	if (ready < 0)
	{
		if (errno == EINTR)
			return 0;
		err = std::strerror(errno);
		return -1;
	}
	for (int i = 0; i < n; i++)
	{
		entries[i].readable = (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
		entries[i].writable = (fds[i].revents & POLLOUT) != 0;
	}
	return ready;
}

} // namespace kutil::net
