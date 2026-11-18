#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

// The single-player server (docs/CLIENT_GOALS.md "Session front end"): OpenKenshi-server run as a child process on localhost, port 0 (the system picks
// one), killed with the client (PR_SET_PDEATHSIG). Ready once its stdout says "listening HOST:PORT"; its stderr stays the client's. stop() sends
// SIGTERM (the server shuts down cleanly) and reaps it; the destructor stops it too.
// The `kenshi` target (full: client + server in one process, KENSHI_EMBED_SERVER, client/README.md "Release targets") has no child: start() runs
// ksim::runServer on a std::thread (the exe argument is ignored), its "listening HOST:PORT" line is read from the server's out stream, stop() sets the
// server's stop flag and joins. The client still connects over loopback TCP, so single player uses the multiplayer path.
class LocalServer
{
public:
	~LocalServer();
	bool start(const std::string& exe, const std::vector<std::string>& args, std::string& err);	// args: without --listen
	// The address to connect to once it listens; empty before. False: not started, or it exited (exited() says why).
	bool running();
	std::string address() const;
	const std::string& exited() const { return exited_; }
	// Ends the server. graceful: SIGTERM, then up to 2 s for it to exit, then SIGKILL (a child that has not installed its handler yet, or is stuck in
	// load, must not hold the client: kenshi-k16.2.271.1). Not graceful (the no-cleanup _exit paths): SIGKILL at once, nothing needs saving.
	void stop(bool graceful = true);

private:
	void reader(int fd);
	pid_t pid_ = -1;
	std::atomic<bool> stopFlag_{ false };	// KENSHI_EMBED_SERVER: ksim::runServer's stop flag
	std::atomic<bool> done_{ true };	// KENSHI_EMBED_SERVER: the server thread has returned (or none runs)
	std::atomic<int> code_{ 0 };	// its exit code
	bool embedded_ = false;
	std::thread thread_;
	mutable std::mutex m_;
	std::string address_, exited_;
};
