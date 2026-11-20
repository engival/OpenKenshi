// Socket headers and thin wrappers for the simlink tests, POSIX or Winsock (kenshi-379.31.4). The tests call test_net::send / recv / close / poll;
// on Windows they map onto Winsock and WSAStartup runs once per test binary. kMsgNoSignal is MSG_NOSIGNAL (0 on Windows, which has no SIGPIPE).
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/types.h>
#include <unistd.h>

namespace test_net
{
constexpr int kMsgNoSignal = 0;
inline int send(int fd, const void* data, size_t size, int flags) { return ::send(static_cast<SOCKET>(fd), static_cast<const char*>(data), static_cast<int>(size), flags); }
inline int recv(int fd, void* data, size_t size, int flags) { return ::recv(static_cast<SOCKET>(fd), static_cast<char*>(data), static_cast<int>(size), flags); }
inline int close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
inline int poll(pollfd* fds, unsigned long count, int timeoutMs) { return ::WSAPoll(fds, count, timeoutMs); }
struct WinsockInit
{
	WinsockInit() { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); }
};
inline const WinsockInit winsockInit;
} // namespace test_net
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace test_net
{
constexpr int kMsgNoSignal = MSG_NOSIGNAL;
inline ssize_t send(int fd, const void* data, size_t size, int flags) { return ::send(fd, data, size, flags); }
inline ssize_t recv(int fd, void* data, size_t size, int flags) { return ::recv(fd, data, size, flags); }
inline int close(int fd) { return ::close(fd); }
inline int poll(pollfd* fds, unsigned long count, int timeoutMs) { return ::poll(fds, count, timeoutMs); }
} // namespace test_net
#endif
