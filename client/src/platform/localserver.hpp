#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.hpp"

// The single-player server (docs/CLIENT_GOALS.md "Session front end"): OpenKenshi-server run as a child process on localhost, port 0 (the system picks
// one), killed with the client (PR_SET_PDEATHSIG; Windows: a kill-on-close job, platform/process.hpp). Ready once its stdout says "listening HOST:PORT"; its stderr is read on the same pipe, echoed to the client's stderr unchanged, and gives stage(). stop() sends
// SIGTERM (the server shuts down cleanly) and reaps it; the destructor stops it too.
// The `kenshi` target (full: client + server in one process, KENSHI_EMBED_SERVER, client/README.md "Release targets") has no child: start() runs
// ksim::runServer on a std::thread (the exe argument is ignored), its "listening HOST:PORT" line is read from the server's out stream, stop() sets the
// server's stop flag and joins. The client still connects over loopback TCP, so single player uses the multiplayer path.
// Where a starting server is (kenshi-k16.2.431.2), from its stderr lines: step of steps, with the seconds since start().
struct ServerStage
{
	std::string text;	// "Starting the server", "Reading the game data", "Loading the dialogue", "Opening the connection"
	int step = 0, steps = 4;
	int seconds = 0;
};

class LocalServer
{
public:
	// The server's last non-empty stderr line without its "ksim_server: " prefix: why it stopped ("no game data directory: ..."); empty if it said nothing.
	std::string lastLine() const;
	ServerStage stage() const;	// text empty before start()
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
	void reader(FILE* f);
	void line(const char* l);	// one server line: echo, stage, address
	std::chrono::steady_clock::time_point t0_;
	int step_ = 0;
	std::string last_;
	const char* stageText_ = "";
	plat::Child child_;
	std::atomic<bool> stopFlag_{ false };	// KENSHI_EMBED_SERVER: ksim::runServer's stop flag
	std::atomic<bool> done_{ true };	// KENSHI_EMBED_SERVER: the server thread has returned (or none runs)
	std::atomic<int> code_{ 0 };	// its exit code
	bool embedded_ = false;
	std::thread thread_;
	mutable std::mutex m_;
	std::string address_, exited_;
};
