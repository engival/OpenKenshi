#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The client's one TCP connection (net/simview.cpp, the simlink): connect, non-blocking receive, whole-frame send. POSIX sockets on Linux and macOS
// (platform/posix/socket.cpp), Winsock on Windows (platform/windows/socket.cpp, kenshi-379.31.1). A Socket is the OS handle widened to fit a Win64
// SOCKET; kNoSocket is "none".
namespace plat {

using Socket = std::intptr_t;
inline constexpr Socket kNoSocket = -1;

// Connects to host:port (any address family) and makes the socket non-blocking. kNoSocket and err set on failure.
Socket tcpConnect(const std::string& host, const std::string& port, std::string& err);
void tcpClose(Socket s);
enum class Recv { Data, Closed, WouldBlock, Error };
// One non-blocking receive into buf: Data sets n; Error sets err.
Recv tcpRecv(Socket s, void* buf, size_t cap, size_t& n, std::string& err);
// Sends all of data in one call, no SIGPIPE; false on an error or a short write.
bool tcpSend(Socket s, const void* data, size_t len);

}
