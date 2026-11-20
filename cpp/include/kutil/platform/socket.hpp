#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// TCP sockets for the SIMLINK server and client (ksim/net/simlink_server.cpp; client/src/platform/socket.hpp's plat:: functions have the
// same shape and can be replaced by these, kenshi-379.31.5). POSIX sockets (src/kutil/platform/posix/socket.cpp, Linux and macOS) or
// Winsock (windows/socket.cpp). A Socket is the OS handle widened to fit a Win64 SOCKET; kNoSocket is "none". All sockets made here are
// non-blocking, and a send to a closed peer is an error, never SIGPIPE. Errors come back as text: no errno / WSAGetLastError in callers.
namespace kutil::net {

using Socket = std::intptr_t;
inline constexpr Socket kNoSocket = -1;

// Winsock startup (WSAStartup, once; a no-op on POSIX). Every function below calls it, so calling it first is optional. False + err on failure.
bool init(std::string& err);

// Connects to host:port (any address family; blocking connect), then non-blocking. kNoSocket and err set on failure.
Socket tcpConnect(const std::string& host, const std::string& port, std::string& err);
// Listens on an IPv4 literal (host) and port (0: the OS picks); boundPort is the port actually bound. SO_REUSEADDR, non-blocking.
// kNoSocket and err ("not an IPv4 address: ...", "bind HOST:PORT: reason") on failure.
Socket tcpListen(const std::string& host, int port, int backlog, int& boundPort, std::string& err);
// Accepts one pending connection (non-blocking, TCP_NODELAY); kNoSocket when none is ready or on error. loopback: the peer is 127.0.0.0/8.
Socket tcpAccept(Socket listener, bool& loopback);
void tcpClose(Socket s); // kNoSocket is ignored

enum class Io { Ok, Closed, WouldBlock, Error };
// One non-blocking receive: Ok sets n (> 0); Closed is an orderly shutdown by the peer. Error sets err. An interrupted call is retried.
Io tcpRecv(Socket s, void* buf, std::size_t cap, std::size_t& n, std::string& err);
// One non-blocking send of up to len bytes: Ok sets n (> 0, may be short); WouldBlock when the buffer is full; Error on reset / broken pipe.
Io tcpSend(Socket s, const void* data, std::size_t len, std::size_t& n, std::string& err);
// Sends all of data or fails (a short write on a non-blocking socket counts as failure, as the client's tcpSend does).
bool tcpSendAll(Socket s, const void* data, std::size_t len);

struct PollEntry
{
	Socket socket = kNoSocket;
	bool wantRead = false;
	bool wantWrite = false;
	bool readable = false; // out: data, a pending accept, a hang-up or an error (a recv will say which)
	bool writable = false; // out
};
// Waits up to timeoutMs for any of the first n entries (poll / WSAPoll). The number ready; 0 on timeout or an interrupted wait; -1 on a
// failure (err set).
int pollSockets(PollEntry* entries, int n, int timeoutMs, std::string& err);

} // namespace kutil::net
