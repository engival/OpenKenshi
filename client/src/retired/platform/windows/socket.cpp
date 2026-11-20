#include "platform/socket.hpp"

#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>

// Winsock (kenshi-379.31.1): WSAStartup once, on the first connect; never cleaned up (the process exit does it). No SIGPIPE on Windows.
namespace plat {

namespace {
std::string wsaError(int code)
{
	char msg[256] = {};
	if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)code, 0, msg, sizeof msg, nullptr) == 0) { return "winsock error " + std::to_string(code); }
	std::string s = msg;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) { s.pop_back(); }
	return s;
}
}

Socket tcpConnect(const std::string& host, const std::string& port, std::string& err)
{
	static std::once_flag once;
	static int startup = 0;
	std::call_once(once, [] { WSADATA wsa; startup = WSAStartup(MAKEWORD(2, 2), &wsa); });
	if (startup != 0) { err = "WSAStartup: " + wsaError(startup); return kNoSocket; }
	addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) { err = wsaError(rc); return kNoSocket; }
	SOCKET fd = INVALID_SOCKET;
	int last = 0;
	for (addrinfo* a = res; a && fd == INVALID_SOCKET; a = a->ai_next)
	{
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd != INVALID_SOCKET && connect(fd, a->ai_addr, (int)a->ai_addrlen) != 0) { last = WSAGetLastError(); closesocket(fd); fd = INVALID_SOCKET; }
		else if (fd == INVALID_SOCKET) { last = WSAGetLastError(); }
	}
	freeaddrinfo(res);
	if (fd == INVALID_SOCKET) { err = wsaError(last); return kNoSocket; }
	u_long nonblocking = 1;
	ioctlsocket(fd, FIONBIO, &nonblocking);
	return (Socket)fd;
}

void tcpClose(Socket s)
{
	if (s != kNoSocket) { closesocket((SOCKET)s); }
}

Recv tcpRecv(Socket s, void* buf, size_t cap, size_t& n, std::string& err)
{
	const int r = recv((SOCKET)s, static_cast<char*>(buf), (int)cap, 0);
	if (r > 0) { n = (size_t)r; return Recv::Data; }
	if (r == 0) { return Recv::Closed; }
	const int e = WSAGetLastError();
	if (e == WSAEWOULDBLOCK) { return Recv::WouldBlock; }
	err = wsaError(e);
	return Recv::Error;
}

bool tcpSend(Socket s, const void* data, size_t len)
{
	return send((SOCKET)s, static_cast<const char*>(data), (int)len, 0) == (int)len;
}

}
