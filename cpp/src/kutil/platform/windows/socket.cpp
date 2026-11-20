#include "kutil/platform/socket.hpp"

#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>

// Winsock (kenshi-379.31.5): WSAStartup once, on the first call; never cleaned up (the process exit does it). No SIGPIPE on Windows.
// Links ws2_32 (cpp/CMakeLists.txt, kutil).
namespace kutil::net {

namespace {

std::string wsaError(int code)
{
	char msg[256] = {};
	if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code), 0, msg, sizeof msg, nullptr) == 0)
		return "winsock error " + std::to_string(code);
	std::string s = msg;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.'))
		s.pop_back();
	return s;
}

void prepare(SOCKET fd)
{
	u_long nonblocking = 1;
	ioctlsocket(fd, FIONBIO, &nonblocking);
}

Socket fail(std::string& err, const std::string& what, int code)
{
	err = what + wsaError(code);
	return kNoSocket;
}

} // namespace

bool init(std::string& err)
{
	static std::once_flag once;
	static int startup = 0;
	std::call_once(once, [] { WSADATA wsa; startup = WSAStartup(MAKEWORD(2, 2), &wsa); });
	if (startup != 0)
		err = "WSAStartup: " + wsaError(startup);
	return startup == 0;
}

Socket tcpConnect(const std::string& host, const std::string& port, std::string& err)
{
	if (!init(err))
		return kNoSocket;
	addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0)
		return fail(err, "", rc);
	SOCKET fd = INVALID_SOCKET;
	int last = 0;
	for (addrinfo* a = res; a && fd == INVALID_SOCKET; a = a->ai_next)
	{
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd == INVALID_SOCKET)
			last = WSAGetLastError();
		else if (connect(fd, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0)
		{
			last = WSAGetLastError();
			closesocket(fd);
			fd = INVALID_SOCKET;
		}
	}
	freeaddrinfo(res);
	if (fd == INVALID_SOCKET)
		return fail(err, "", last);
	prepare(fd);
	return static_cast<Socket>(fd);
}

Socket tcpListen(const std::string& host, int port, int backlog, int& boundPort, std::string& err)
{
	if (!init(err))
		return kNoSocket;
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<u_short>(port));
	if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
	{
		err = "not an IPv4 address: " + host;
		return kNoSocket;
	}
	const SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == INVALID_SOCKET)
		return fail(err, "socket: ", WSAGetLastError());
	// no SO_REUSEADDR: on Windows it lets a second process bind the same port (SO_EXCLUSIVEADDRUSE is the safe default)
	if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
	{
		const int code = WSAGetLastError();
		closesocket(fd);
		return fail(err, "bind " + host + ":" + std::to_string(port) + ": ", code);
	}
	if (listen(fd, backlog) != 0)
	{
		const int code = WSAGetLastError();
		closesocket(fd);
		return fail(err, "listen: ", code);
	}
	int length = sizeof address;
	getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
	boundPort = ntohs(address.sin_port);
	prepare(fd);
	return static_cast<Socket>(fd);
}

Socket tcpAccept(Socket listener, bool& loopback)
{
	sockaddr_in peer {};
	int peerLen = sizeof peer;
	const SOCKET fd = accept(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&peer), &peerLen);
	if (fd == INVALID_SOCKET)
		return kNoSocket;
	const char one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	prepare(fd);
	loopback = peer.sin_family == AF_INET && (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
	return static_cast<Socket>(fd);
}

void tcpClose(Socket s)
{
	if (s != kNoSocket)
		closesocket(static_cast<SOCKET>(s));
}

Io tcpRecv(Socket s, void* buf, std::size_t cap, std::size_t& n, std::string& err)
{
	const int r = recv(static_cast<SOCKET>(s), static_cast<char*>(buf), static_cast<int>(cap), 0);
	if (r > 0) { n = static_cast<std::size_t>(r); return Io::Ok; }
	if (r == 0) return Io::Closed;
	const int e = WSAGetLastError();
	if (e == WSAEWOULDBLOCK) return Io::WouldBlock;
	err = wsaError(e);
	return Io::Error;
}

Io tcpSend(Socket s, const void* data, std::size_t len, std::size_t& n, std::string& err)
{
	const int r = send(static_cast<SOCKET>(s), static_cast<const char*>(data), static_cast<int>(len), 0);
	if (r > 0) { n = static_cast<std::size_t>(r); return Io::Ok; }
	const int e = WSAGetLastError();
	if (r < 0 && e == WSAEWOULDBLOCK) return Io::WouldBlock;
	err = r == 0 ? "send wrote nothing" : wsaError(e);
	return Io::Error;
}

bool tcpSendAll(Socket s, const void* data, std::size_t len)
{
	return send(static_cast<SOCKET>(s), static_cast<const char*>(data), static_cast<int>(len), 0) == static_cast<int>(len);
}

int pollSockets(PollEntry* entries, int n, int timeoutMs, std::string& err)
{
	WSAPOLLFD fds[8]; // the callers poll a listener and one client
	if (n < 1 || n > 8) { err = "pollSockets: 1..8 entries"; return -1; }
	for (int i = 0; i < n; i++)
		fds[i] = {static_cast<SOCKET>(entries[i].socket), static_cast<SHORT>((entries[i].wantRead ? POLLRDNORM : 0) | (entries[i].wantWrite ? POLLWRNORM : 0)), 0};
	const int ready = WSAPoll(fds, static_cast<ULONG>(n), timeoutMs);
	if (ready == SOCKET_ERROR)
	{
		err = wsaError(WSAGetLastError());
		return -1;
	}
	for (int i = 0; i < n; i++)
	{
		entries[i].readable = (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
		entries[i].writable = (fds[i].revents & POLLWRNORM) != 0;
	}
	return ready;
}

} // namespace kutil::net
